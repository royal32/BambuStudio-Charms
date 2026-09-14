#include "BambuConnectBridge.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <libproc.h>
#include <sys/proc.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#import <Cocoa/Cocoa.h>

extern char **environ;
namespace Slic3r { namespace GUI { namespace BambuConnect {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
bool process_running(pid_t pid) {
    proc_bsdinfo info{};
    return pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) && info.pbi_status != SZOMB;
}

class Pipe {
    int input = -1, output = -1, sequence = 0;
    pid_t child = -1;
    std::string buffer, session, facade;
public:
    std::mutex mutex;
    ~Pipe() { close_pipe(); }
    void close_pipe() {
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        input = output = -1;
        session.clear(); facade.clear(); buffer.clear();
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
        const auto setter_method = invoke(print, "function(){return this().setPrintOptions;}", json::array(), false).at("objectId").get<std::string>();
        const auto setter = scope_binding(setter_method, "n");
        std::ifstream stream(adapter_path);
        if (!stream) throw std::runtime_error("The Studio Connect adapter is missing.");
        const std::string script((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        auto factory = call("Runtime.evaluate", {{"expression", script}});
        if (factory.contains("exceptionDetails")) throw std::runtime_error("Could not load the Connect adapter.");
        facade = invoke(factory.at("result").at("objectId"), "function(p,d,s){return this(p,d,s);}",
            {{{"objectId", print}}, {{"objectId", devices}}, {{"objectId", setter}}}, false).at("objectId");
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
        return bridge.handoff(json::parse(request)).dump();
    } catch (const std::exception& error) {
        bridge.show();
        // After entering handoff, submission may have happened: never retry or
        // switch to the URL importer automatically on an ambiguous outcome.
        return json({{"status", prepared ? "attention" : "unavailable"}, {"error", error.what()}}).dump();
    }
}
}}}
