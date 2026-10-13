#include "BambuConnectBridge.hpp"
#include "BambuConnectTransport.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cerrno>
#include <array>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <sys/proc.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#import <Cocoa/Cocoa.h>

extern char **environ;
namespace Slic3r { namespace GUI { namespace BambuConnect {
namespace {
using Clock = std::chrono::steady_clock;

#if defined(__arm64__)
bool read_own_memory(uintptr_t address, void* destination, size_t size) {
    if (!address || !size) return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
        reinterpret_cast<mach_vm_address_t>(destination), &copied) == KERN_SUCCESS && copied == size;
}

bool supported_session_layout(void* identity_function) {
    Dl_info image{};
    if (!identity_function || !dladdr(identity_function, &image)) return false;
    const auto base = reinterpret_cast<uintptr_t>(image.dli_fbase);
    mach_header_64 header{};
    if (!read_own_memory(base, &header, sizeof(header)) || header.magic != MH_MAGIC_64 ||
        header.ncmds > 1024 || header.sizeofcmds > 1024 * 1024) return false;
    // LC_UUID of the installed networking plugin, not its marketing version.
    // Re-verify the account/string layout before adding any other binary UUID.
    const std::array<unsigned char, 16> expected = {
        0x22,0x25,0x30,0x83,0x62,0x32,0x32,0x5a,0x87,0xfb,0xed,0x90,0x92,0x62,0x5f,0x5e};
    size_t offset = sizeof(header);
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        load_command command{};
        if (offset + sizeof(command) > sizeof(header) + header.sizeofcmds ||
            !read_own_memory(base + offset, &command, sizeof(command)) || command.cmdsize < sizeof(command) ||
            command.cmdsize > sizeof(header) + header.sizeofcmds - offset) return false;
        if (command.cmd == LC_UUID) {
            uuid_command uuid{};
            return command.cmdsize == sizeof(uuid) && read_own_memory(base + offset, &uuid, sizeof(uuid)) &&
                std::memcmp(uuid.uuid, expected.data(), expected.size()) == 0;
        }
        offset += command.cmdsize;
    }
    return false;
}

std::string read_session_string(uintptr_t address) {
    // This pinned arm64 build uses libc++'s 24-byte alternate string layout.
    // Read via Mach so an expired pointer returns failure instead of crashing.
    std::array<unsigned char, 24> before{}, after{};
    if (!read_own_memory(address, before.data(), before.size())) return {};
    size_t length = before[23];
    std::string result;
    if (length & 0x80) {
        uintptr_t pointer = 0;
        std::memcpy(&pointer, before.data(), sizeof(pointer));
        std::memcpy(&length, before.data() + 8, sizeof(length));
        if (!length || length > 8192) return {};
        result.resize(length);
        if (!read_own_memory(pointer, result.data(), length)) return {};
    } else {
        if (length > 22) return {};
        result.assign(reinterpret_cast<const char*>(before.data()), length);
    }
    if (!read_own_memory(address, after.data(), after.size()) || before != after) return {};
    return result;
}
#endif

bool process_running(pid_t pid) {
    proc_bsdinfo info{};
    return pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_status != SZOMB;
}

class MacTransport final : public Transport {
    int input = -1, output = -1;
    pid_t child = -1;
public:
    ~MacTransport() override { close(); }
    void close() override {
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        input = output = -1;
        if (child > 0) waitpid(child, nullptr, WNOHANG);
        child = -1;
    }
    bool running() override { return process_running(child); }
    void hide(bool hidden) override {
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:child];
            if (hidden) { if (app && ![app isHidden]) [app hide]; }
            else { [app unhide]; [app activateWithOptions:NSApplicationActivateIgnoringOtherApps]; }
        }
    }
    void write(const std::string& bytes, Clock::time_point deadline) override {
        size_t written = 0;
        while (written < bytes.size()) {
            pollfd fd{input, POLLOUT, 0};
            if (Clock::now() >= deadline) throw std::runtime_error("Connect pipe write timed out.");
            if (poll(&fd, 1, 200) <= 0) continue;
            auto count = ::write(input, bytes.data() + written, bytes.size() - written);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::runtime_error("Connect closed its pipe.");
            written += count;
        }
    }
    std::string read(Clock::time_point deadline) override {
        while (Clock::now() < deadline) {
            pollfd fd{output, POLLIN, 0};
            if (poll(&fd, 1, 200) <= 0) continue;
            char chunk[65536];
            auto count = ::read(output, chunk, sizeof(chunk));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::runtime_error("Connect closed its pipe.");
            return std::string(chunk, count);
        }
        throw std::runtime_error("Connect did not respond in time.");
    }
    void launch() override {
        std::string executable;
        @autoreleasepool {
            NSURL* url = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.bambulab.bambu-connect"];
            if (!url) throw std::runtime_error("Bambu Connect is not installed.");
            NSBundle* bundle = [NSBundle bundleWithURL:url];
            executable = [[bundle executablePath] UTF8String];
            // A running Electron singleton cannot acquire new inherited pipes.
            // Quit gracefully once when Studio first establishes this session.
            for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.bambulab.bambu-connect"]) {
                if (![app terminate]) throw std::runtime_error("Close Bambu Connect once, then try again.");
                auto deadline = Clock::now() + std::chrono::seconds(10);
                const pid_t pid = [app processIdentifier];
                while (process_running(pid) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (process_running(pid)) throw std::runtime_error("Close Bambu Connect once, then try again.");
            }
        }
        int in[2], out[2];
        if (pipe(in)) throw std::runtime_error("Could not create Connect pipes.");
        if (pipe(out)) { ::close(in[0]); ::close(in[1]); throw std::runtime_error("Could not create Connect pipes."); }
        // Move descriptors away from Chromium's reserved descriptor numbers 3/4.
        int descriptors[4] = {in[0], in[1], out[0], out[1]};
        for (int& fd : descriptors) {
            int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
            if (copy < 0) {
                for (int owned : descriptors) ::close(owned);
                throw std::runtime_error("Could not reserve Connect pipe descriptors.");
            }
            ::close(fd); fd = copy;
        }
        input = descriptors[1]; output = descriptors[2];
        fcntl(input, F_SETNOSIGPIPE, 1);
        fcntl(input, F_SETFL, O_NONBLOCK); fcntl(output, F_SETFL, O_NONBLOCK);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, descriptors[0], 3);
        posix_spawn_file_actions_adddup2(&actions, descriptors[3], 4);
        for (int fd = 0; fd < 3; ++fd) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        // A hidden Electron renderer otherwise throttles timers/initialization,
        // making the same handoff work in front but stall in background mode.
        const char* argv[] = {executable.c_str(), "--remote-debugging-pipe",
            "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
            "--disable-backgrounding-occluded-windows", nullptr};
        int error = posix_spawn(&child, executable.c_str(), &actions, &attributes, const_cast<char**>(argv), environ);
        posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
        ::close(descriptors[0]); ::close(descriptors[3]);
        if (error) throw std::runtime_error("Could not launch Bambu Connect.");
    }
};
}
std::unique_ptr<Transport> create_transport() { return std::make_unique<MacTransport>(); }
std::string studio_access_token(void* agent, void* identity_function, const std::string& user_id)
{
#if defined(__arm64__)
    if (!agent || user_id.empty() || !supported_session_layout(identity_function)) return {};
    uintptr_t implementation = 0, account = 0, current = 0;
    if (!read_own_memory(reinterpret_cast<uintptr_t>(agent), &implementation, sizeof(implementation)) ||
        !implementation || !read_own_memory(implementation + 0x20, &account, sizeof(account)) || !account) return {};
    // Verified against get_user_id and the plugin's account serializer:
    // shared account pointer at impl+0x20, UID at +0x48, access token at +0x80.
    if (read_session_string(account + 0x48) != user_id) return {};
    auto token = read_session_string(account + 0x80);
    if (!read_own_memory(implementation + 0x20, &current, sizeof(current)) || current != account ||
        read_session_string(account + 0x48) != user_id) return {};
    return token;
#else
    // Intel's libc++ string ABI differs from the pinned arm64 plugin. Use the
    // shared bridge's matching-account check with Connect's own sign-in.
    (void)agent; (void)identity_function; (void)user_id;
    return {};
#endif
}
}}}
