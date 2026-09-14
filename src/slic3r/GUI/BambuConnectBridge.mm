#include "BambuConnectBridge.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <atomic>
#include <fstream>
#include <mutex>
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
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

bool read_own_memory(uintptr_t address, void* destination, size_t size) {
    if (!address || !size) return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
        reinterpret_cast<mach_vm_address_t>(destination), &copied) == KERN_SUCCESS && copied == size;
}

bool supported_session_layout(void* identity_function) {
#if defined(__arm64__)
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
#endif
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

bool process_running(pid_t pid) {
    proc_bsdinfo info{};
    return pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_status != SZOMB;
}

// A directly spawned app registers with Launch Services before its first
// window is ready. Hide it at that point, instead of waiting for the renderer
// to load. Keep it hidden through startup activation and window restoration.
class StartupHider {
    std::atomic<bool> stopped{false};
    std::thread worker;
public:
    explicit StartupHider(pid_t pid) : worker([this, pid] {
        while (!stopped.load()) {
            @autoreleasepool {
                NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
                if (app && ![app isHidden]) [app hide];
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }) {}
    ~StartupHider() { stopped = true; worker.join(); }
};

class Pipe {
    int input = -1, output = -1, sequence = 0;
    pid_t child = -1;
    std::string buffer, session, facade, account_facade;
public:
    std::mutex mutex;
    ~Pipe() { close_pipe(); }
    void close_pipe() {
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        input = output = -1;
        session.clear(); facade.clear(); account_facade.clear(); buffer.clear();
        if (child > 0) waitpid(child, nullptr, WNOHANG);
        child = -1;
    }
    json call(const std::string& method, json params = json::object(), bool renderer = true) {
        const int id = ++sequence;
        json message = {{"id", id}, {"method", method}, {"params", params}};
        if (renderer && !session.empty()) message["sessionId"] = session;
        const std::string bytes = message.dump() + '\0';
        size_t written = 0;
        auto deadline = Clock::now() + std::chrono::seconds(45);
        while (written < bytes.size()) {
            pollfd fd{input, POLLOUT, 0};
            if (Clock::now() >= deadline) throw std::runtime_error("Connect pipe write timed out.");
            if (poll(&fd, 1, 200) <= 0) continue;
            auto count = write(input, bytes.data() + written, bytes.size() - written);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::runtime_error("Connect closed its pipe.");
            written += count;
        }
        while (Clock::now() < deadline) {
            auto end = buffer.find('\0');
            if (end != std::string::npos) {
                json response = json::parse(buffer.substr(0, end));
                buffer.erase(0, end + 1);
                if (response.value("id", 0) != id) continue; // Discard protocol events.
                if (response.contains("error")) {
                    throw std::runtime_error("Connect rejected a bridge command.");
                }
                return response.at("result");
            }
            pollfd fd{output, POLLIN, 0};
            if (poll(&fd, 1, 200) <= 0) continue;
            char chunk[65536];
            auto count = read(output, chunk, sizeof(chunk));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::runtime_error("Connect closed its pipe.");
            buffer.append(chunk, count);
            if (buffer.size() > 32 * 1024 * 1024) throw std::runtime_error("Unexpected Connect protocol response.");
        }
        throw std::runtime_error("Connect did not respond in time.");
    }
    json properties(const std::string& object) {
        return call("Runtime.getProperties", {{"objectId", object}, {"ownProperties", true}});
    }
    static std::string object_named(const json& entries, const std::string& name) {
        for (const auto& entry : entries)
            if (entry.value("name", "") == name) return entry.at("value").value("objectId", "");
        return {};
    }
    json scopes(const std::string& function) {
        const auto p = properties(function);
        const auto id = object_named(p.at("internalProperties"), "[[Scopes]]");
        return properties(id).at("result");
    }
    std::string scope_binding(const std::string& function, const std::string& name) {
        for (const auto& scope : scopes(function)) {
            if (scope.at("value").value("description", "") == "Global") continue;
            auto found = object_named(properties(scope.at("value").at("objectId")).at("result"), name);
            if (!found.empty()) return found;
        }
        throw std::runtime_error("Connect's internal print interface changed.");
    }
    json invoke(const std::string& object, const std::string& function, json arguments = json::array(), bool by_value = true) {
        auto response = call("Runtime.callFunctionOn", {{"objectId", object}, {"functionDeclaration", function},
            {"arguments", arguments}, {"awaitPromise", true}, {"returnByValue", by_value}});
        if (response.contains("exceptionDetails")) {
            const auto exception = response.at("exceptionDetails").value("exception", json::object());
            const auto description = exception.value("description", "Connect could not complete the print handoff.");
            throw std::runtime_error(description.substr(0, description.find('\n')));
        }
        return response.at("result");
    }
    void bounds(const std::string& state) {
        @autoreleasepool {
            for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.bambulab.bambu-connect"]) {
                if (state == "minimized") [app hide];
                else [app unhide];
            }
        }
    }
    void show() {
        try { bounds("normal"); } catch (...) {}
        if (!facade.empty()) {
            try { invoke(facade, "function(){this.endBackground();}"); } catch (...) {}
        }
        @autoreleasepool {
            for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.bambulab.bambu-connect"])
                [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
        }
    }
    void start(const std::string& adapter_path) {
        if (!facade.empty() && process_running(child)) return;
        close_pipe();
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
        if (pipe(out)) { close(in[0]); close(in[1]); throw std::runtime_error("Could not create Connect pipes."); }
        // Move descriptors away from Chromium's reserved descriptor numbers 3/4.
        int descriptors[4] = {in[0], in[1], out[0], out[1]};
        for (int& fd : descriptors) {
            int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
            if (copy < 0) {
                for (int owned : descriptors) close(owned);
                throw std::runtime_error("Could not reserve Connect pipe descriptors.");
            }
            close(fd); fd = copy;
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
        close(descriptors[0]); close(descriptors[3]);
        if (error) throw std::runtime_error("Could not launch Bambu Connect.");
        StartupHider startup_hider(child);
        auto version = call("Browser.getVersion", json::object(), false);
        if (version.value("userAgent", "").find("BambuConnect/2.5.0-beta.15") == std::string::npos)
            throw std::runtime_error("This Connect version needs an updated bridge adapter.");
        std::string target;
        for (int attempt = 0; attempt < 100 && target.empty(); ++attempt) {
            const auto targets = call("Target.getTargets", json::object(), false).at("targetInfos");
            for (const auto& item : targets)
                if (item.value("type", "") == "page" && item.value("url", "").find("/app.asar/.vite/renderer/main_window/index.html") != std::string::npos)
                    target = item.at("targetId");
            if (target.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (target.empty()) throw std::runtime_error("Connect's print window was not found.");
        session = call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, false).at("sessionId");
        bool ready = false;
        // The initial root child is only a sign-in spinner. Wait for the real
        // navigation shell so its startup redirect cannot supersede our request.
        for (int attempt = 0; attempt < 300 && !ready; ++attempt) {
            auto state = call("Runtime.evaluate", {{"expression", "!!document.querySelector('#root a[href*=\"/devices\"]')"}, {"returnByValue", true}});
            ready = state.at("result").value("value", false);
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!ready) throw std::runtime_error("Connect's interface did not finish loading.");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto component = call("Runtime.evaluate", {{"expression", "import('./assets/router-DcHfLi1L.js').then(m=>m.router.options.routeTree.options.component)"}, {"awaitPromise", true}});
        if (component.contains("exceptionDetails")) throw std::runtime_error("Connect's print adapter needs updating.");
        const auto function = component.at("result").at("objectId").get<std::string>();
        const auto print = scope_binding(function, "tk");
        const auto devices = scope_binding(function, "T_");
        const auto auth = scope_binding(function, "Ro");
        const auto save_token = scope_binding(function, "xGe");
        const auto get_token = scope_binding(function, "Qp");
        const auto environment = scope_binding(function, "Yi");
        account_facade = invoke(auth, "function(p,s,t,e){return {auth:this,print:p,save:s,token:t,environment:e};}",
            {{{"objectId", print}}, {{"objectId", save_token}}, {{"objectId", get_token}}, {{"objectId", environment}}}, false).at("objectId");
        const auto setter_method = invoke(print, "function(){return this().setPrintOptions;}", json::array(), false).at("objectId").get<std::string>();
        const auto setter = scope_binding(setter_method, "n");
        std::ifstream stream(adapter_path);
        if (!stream) throw std::runtime_error("The Studio Connect adapter is missing.");
        const std::string script((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        auto factory = call("Runtime.evaluate", {{"expression", script}});
        if (factory.contains("exceptionDetails")) throw std::runtime_error("Could not load the Connect adapter.");
        facade = invoke(factory.at("result").at("objectId"), "function(p,d,s,a){return this(p,d,s,a);}",
            {{{"objectId", print}}, {{"objectId", devices}}, {{"objectId", setter}}, {{"objectId", auth}}}, false).at("objectId");
    }
    void sync_account(const json& account, const std::string& adapter_path) {
        // Always check before importing: a job must never reach another account's
        // printer picker merely because Connect was signed in independently.
        auto result = invoke(account_facade, R"JS(async function(a) {
            if (this.print().state.sending) throw Error('Connect is sending a job. Wait before switching accounts.');
            const user = this.auth().user;
            const current = user ? String(user.uid) : '';
            if (a.userId && (this.environment().key === 'mainland') !== (a.countryCode === 'CN'))
                throw Error('Studio and Connect use different regions. Set the same region in Connect first.');
            // Also refresh an expiring Connect session when Studio renewed its
            // token, and clear a persisted token even if user info is not loaded.
            if (current === a.userId && (a.userId
                ? (!a.token || this.token() === a.token) : !this.token())) return {restart:false};
            if (!a.userId) {
                if (!await this.auth().logout()) throw Error('Connect could not sign out securely.');
            } else {
                if (!a.token) throw Error('Sign in to this account in Studio again.');
                if (!await this.save(a.token)) throw Error('Connect could not securely save the selected account.');
            }
            return {restart:true};
        })JS", {{{"value", account}}}).at("value");
        if (result.value("restart", false)) {
            // A fresh process drops every old MQTT connection and cached printer.
            facade.clear();
            start(adapter_path);
        }
        auto identity = invoke(account_facade, R"JS(async function(expected) {
            const current = () => this.auth().user ? String(this.auth().user.uid) : '';
            for (const deadline = Date.now() + 10000; expected && current() !== expected && Date.now() < deadline;)
                await new Promise(resolve => setTimeout(resolve, 100));
            return current();
        })JS", {{{"value", account.at("userId")}}}).at("value");
        if (identity != account.at("userId"))
            throw std::runtime_error("Connect could not verify the selected Studio account. Sign in to Studio again before printing.");
    }
    json handoff(const json& request) {
        invoke(facade, "function(){this.beginBackground();}");
        bounds("minimized");
        auto result = invoke(facade, "function(r){return this.prepare(r);}", {{{"value", request}}}).at("value");
        if (request.value("submit", false)) {
            result = invoke(facade, "function(id){return this.submit(id);}", {{{"value", request.at("id")}}}).at("value");
            auto deadline = Clock::now() + std::chrono::minutes(5);
            while (result.value("status", "") == "sending" && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                result = invoke(facade, "function(){return this.status();}").at("value");
            }
            if (result.value("status", "") != "submitted") result["status"] = "attention";
        }
        if (result.value("status", "") != "submitted") show();
        else invoke(facade, "function(){this.endBackground();}");
        return result;
    }
};
Pipe bridge;
}

std::string direct_handoff(const std::string& request, const std::string& adapter_path)
{
    std::unique_lock<std::mutex> guard(bridge.mutex, std::try_to_lock);
    if (!guard.owns_lock()) return json({{"status", "attention"}, {"error", "Another Connect handoff is in progress."}}).dump();
    bool prepared = false;
    try {
        bridge.start(adapter_path);
        prepared = true;
        auto data = json::parse(request);
        if (!data.contains("account")) throw std::runtime_error("Sign in to Studio before printing with Connect.");
        try {
            bridge.sync_account(data.at("account"), adapter_path);
        } catch (...) {
            // Session-bearing protocol exceptions must not enter print logs.
            throw std::runtime_error("Could not synchronize the Studio account with Bambu Connect. Sign in to Studio again or update the account adapter before printing.");
        }
        data["accountUserId"] = data.at("account").at("userId");
        data.erase("account");
        return bridge.handoff(data).dump();
    } catch (const std::exception& error) {
        bridge.show();
        // After entering handoff, submission may have happened: never retry or
        // switch to the URL importer automatically on an ambiguous outcome.
        return json({{"status", prepared ? "attention" : "unavailable"}, {"error", error.what()}}).dump();
    }
}

std::string synchronize_account(const std::string& session, const std::string& adapter_path, bool show)
{
    std::unique_lock<std::mutex> guard(bridge.mutex, std::try_to_lock);
    if (!guard.owns_lock()) return json({{"status", "attention"}, {"error", "A Connect handoff is in progress."}}).dump();
    try {
        bridge.start(adapter_path);
        bridge.sync_account(json::parse(session), adapter_path);
        if (show) bridge.show();
        return json({{"status", "account_ready"}}).dump();
    } catch (...) {
        // Never propagate session-bearing protocol/JSON errors into GUI logs.
        return json({{"status", "attention"}, {"error", "Could not synchronize the Studio account with Bambu Connect. Sign in to Studio again and check that both apps use the same region."}}).dump();
    }
}

std::string studio_access_token(void* agent, void* identity_function, const std::string& user_id)
{
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
}
}}}
