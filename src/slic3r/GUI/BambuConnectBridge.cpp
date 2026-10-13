#include "BambuConnectBridge.hpp"
#include "BambuConnectTransport.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace Slic3r { namespace GUI { namespace BambuConnect {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
class BridgeCommandError : public std::runtime_error {
public:
    BridgeCommandError() : std::runtime_error("Connect rejected a bridge command.") {}
};
class StartupHider {
    std::atomic<bool> stopped{false};
    std::thread worker;
public:
    explicit StartupHider(Transport& transport) : worker([this, &transport] {
        while (!stopped.load()) {
            transport.hide(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }) {}
    ~StartupHider() { stopped = true; worker.join(); }
};
class Pipe {
    std::unique_ptr<Transport> transport = create_transport();
    int sequence = 0;
    std::string buffer, session, facade, account_facade;
public:
    std::mutex mutex;
    ~Pipe() { close_pipe(); }
    void close_pipe() {
        transport->close();
        session.clear(); facade.clear(); account_facade.clear(); buffer.clear();
    }
    json call(const std::string& method, json params = json::object(), bool renderer = true) {
        const int id = ++sequence;
        json message = {{"id", id}, {"method", method}, {"params", params}};
        if (renderer && !session.empty()) message["sessionId"] = session;
        const std::string bytes = message.dump() + '\0';
        auto deadline = Clock::now() + std::chrono::seconds(45);
        transport->write(bytes, deadline);
        while (Clock::now() < deadline) {
            auto end = buffer.find('\0');
            if (end != std::string::npos) {
                json response = json::parse(buffer.substr(0, end));
                buffer.erase(0, end + 1);
                if (response.value("id", 0) != id) continue; // Discard protocol events.
                if (response.contains("error")) {
                    throw BridgeCommandError();
                }
                return response.at("result");
            }
            buffer += transport->read(deadline);
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
        transport->hide(state == "minimized");
    }
    void show() {
        transport->hide(false);
        if (!facade.empty()) {
            try { invoke(facade, "function(){this.endBackground();}"); } catch (...) {}
        }
    }
    bool start(const std::string& adapter_path) {
        bool renderer_rebound = false;
        if (!facade.empty() && transport->running()) {
            // Electron can replace its renderer while the main process and
            // private pipe survive. Validate both cached objects before any
            // account change, file import or submission for a new request.
            try {
                if (invoke(facade, "function(a){return typeof this.prepare === 'function' && typeof this.submit === 'function' && typeof a.auth === 'function' && typeof a.print === 'function';}",
                    {{{"objectId", account_facade}}}).value("value", false)) return false;
            } catch (const BridgeCommandError&) {
                // Only stale protocol handles are recoverable here. Pipe
                // failures propagate; a handoff is never retried after import.
            }
            if (!session.empty()) {
                try { call("Target.detachFromTarget", {{"sessionId", session}}, false); }
                catch (const BridgeCommandError&) { /* The old target may already be gone. */ }
            }
            session.clear(); facade.clear(); account_facade.clear();
            renderer_rebound = true;
        } else {
            close_pipe();
            transport->launch();
        }
        StartupHider startup_hider(*transport);
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
        std::ifstream stream(std::filesystem::u8path(adapter_path));
        if (!stream) throw std::runtime_error("The Studio Connect adapter is missing.");
        const std::string script((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        auto factory = call("Runtime.evaluate", {{"expression", script}});
        if (factory.contains("exceptionDetails")) throw std::runtime_error("Could not load the Connect adapter.");
        facade = invoke(factory.at("result").at("objectId"), "function(p,d,s,a){return this(p,d,s,a);}",
            {{{"objectId", print}}, {{"objectId", devices}}, {{"objectId", setter}}, {{"objectId", auth}}}, false).at("objectId");
        return renderer_rebound;
    }
    void sync_account(const json& account, const std::string& adapter_path) {
        // Always check before importing: a job must never reach another account's
        // printer picker merely because Connect was signed in independently.
        auto result = invoke(account_facade, R"JS(async function(a) {
            // After a renderer reload the navigation shell can precede restored
            // authentication. Give Connect's own sign-in time to finish on the
            // manual-token path; a different signed-in account still fails.
            if (a.userId && !a.token) {
                const current = () => String(this.auth().user?.uid ?? '');
                for (const deadline = Date.now() + 10000; !current() && Date.now() < deadline;)
                    await new Promise(resolve => setTimeout(resolve, 100));
            }
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
                if (!a.token) throw Error('Sign in to the same account in Bambu Connect, then try again.');
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
    json handoff(const json& request, std::string* phase = nullptr) {
        if (phase) *phase = "prepare";
        invoke(facade, "function(){this.beginBackground();}");
        bounds("minimized");
        auto result = invoke(facade, "function(r){return this.prepare(r);}", {{{"value", request}}}).at("value");
        if (request.value("submit", false)) {
            if (phase) *phase = "submit";
            result = invoke(facade, "function(id){return this.submit(id);}", {{{"value", request.at("id")}}}).at("value");
            if (phase) *phase = "confirm";
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
    bool renderer_rebound = false;
    std::string phase = "connect";
    try {
        renderer_rebound = bridge.start(adapter_path);
        prepared = true;
        phase = "account";
        auto data = json::parse(request);
        if (!data.contains("account")) throw std::runtime_error("Sign in to Studio before printing with Connect.");
        try {
            bridge.sync_account(data.at("account"), adapter_path);
        } catch (...) {
            // Session-bearing protocol exceptions must not enter print logs.
            throw std::runtime_error("Could not verify the Studio account in Bambu Connect. Sign in to the same account in both apps and check their regions before printing.");
        }
        data["accountUserId"] = data.at("account").at("userId");
        data.erase("account");
        auto result = bridge.handoff(data, &phase);
        result["phase"] = phase;
        result["renderer_rebound"] = renderer_rebound;
        return result.dump();
    } catch (const std::exception& error) {
        bridge.show();
        // After entering handoff, submission may have happened: never retry or
        // switch to the URL importer automatically on an ambiguous outcome.
        return json({{"status", prepared ? "attention" : "unavailable"}, {"error", error.what()},
            {"phase", phase}, {"failure_code", phase + "_failed"}, {"renderer_rebound", renderer_rebound}}).dump();
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
        if (show) bridge.show();
        return json({{"status", "attention"}, {"error", "Sign in to the same account in Studio and Bambu Connect, and check that both apps use the same region."}}).dump();
    }
}

}}}
