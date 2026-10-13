// Shared bridge lifecycle checks; no Connect installation, login or printer.
#include "catch_main.hpp"
#include "slic3r/GUI/BambuConnectBridge.cpp"

namespace Slic3r { namespace GUI { namespace BambuConnect {
namespace {
struct FakeRuntime {
    int generation = 1, launches = 0, attaches = 0, detaches = 0, prepares = 0, submits = 0;
    bool running = false, fail_pipe = false, fail_prepare = false, fail_submit = false;
    json request;
    std::string object(const std::string& name) const { return name + "@" + std::to_string(generation); }
    bool stale(const std::string& id) const {
        const auto at = id.find('@');
        return at != std::string::npos && id.substr(at + 1) != std::to_string(generation);
    }
};
std::shared_ptr<FakeRuntime>& latest_runtime() {
    static std::shared_ptr<FakeRuntime> state;
    return state;
}
class FakeTransport final : public Transport {
    std::shared_ptr<FakeRuntime> state = std::make_shared<FakeRuntime>();
public:
    FakeTransport() { latest_runtime() = state; }
    void launch() override { state->running = true; ++state->launches; }
    void close() override { state->running = false; }
    bool running() override { return state->running; }
    void hide(bool) override {}
    void write(const std::string& bytes, Clock::time_point) override {
        if (state->fail_pipe) throw std::runtime_error("Connect closed its pipe.");
        state->request = json::parse(bytes.substr(0, bytes.size() - 1));
    }
    std::string read(Clock::time_point) override {
        const auto& request = state->request;
        const auto method = request.at("method").get<std::string>();
        const auto params = request.at("params");
        json result = json::object();
        bool stale = state->stale(params.value("objectId", ""));
        for (const auto& arg : params.value("arguments", json::array()))
            stale = stale || state->stale(arg.value("objectId", ""));
        json response = {{"id", request.at("id")}};
        if (stale) response["error"] = {{"code", -32000}, {"message", "Could not find object with given id"}};
        else {
            if (method == "Browser.getVersion") result = {{"userAgent", "BambuConnect/2.5.0-beta.15"}};
            else if (method == "Target.getTargets") result = {{"targetInfos", json::array({{
                {"type", "page"}, {"url", "file:///app.asar/.vite/renderer/main_window/index.html"}, {"targetId", "page"}}})}};
            else if (method == "Target.attachToTarget") { ++state->attaches; result = {{"sessionId", "session"}}; }
            else if (method == "Target.detachFromTarget") ++state->detaches;
            else if (method == "Runtime.evaluate") {
                const auto expression = params.at("expression").get<std::string>();
                result = {{"result", expression.find("!!document") != std::string::npos
                    ? json{{"value", true}} : json{{"objectId", state->object(expression.find("import(") == 0 ? "component" : "factory")}}}};
            } else if (method == "Runtime.getProperties") {
                const auto id = params.at("objectId").get<std::string>();
                if (id.find("scopes@") == 0) result = {{"result", json::array({{
                    {"name", "0"}, {"value", {{"description", "Module"}, {"objectId", state->object("scope")}}}}})}};
                else if (id.find("scope@") == 0) {
                    result["result"] = json::array();
                    for (const auto* name : {"tk", "T_", "Ro", "xGe", "Qp", "Yi", "n"})
                        result["result"].push_back({{"name", name}, {"value", {{"objectId", state->object(name)}}}});
                } else result = {{"internalProperties", json::array({{
                    {"name", "[[Scopes]]"}, {"value", {{"objectId", state->object("scopes")}}}}})}};
            } else if (method == "Runtime.callFunctionOn") {
                const auto function = params.at("functionDeclaration").get<std::string>();
                json value = json::object();
                if (function.find("typeof this.prepare") != std::string::npos) value["value"] = true;
                else if (function.find("return {auth:this") != std::string::npos) value["objectId"] = state->object("account");
                else if (function.find("setPrintOptions") != std::string::npos) value["objectId"] = state->object("setter-method");
                else if (function.find("this(p,d,s,a)") != std::string::npos) value["objectId"] = state->object("facade");
                else if (function.find("return this.prepare(r)") != std::string::npos) {
                    ++state->prepares;
                    if (state->fail_prepare) result["exceptionDetails"] = {{"exception", {{"description", "Import failed"}}}};
                    value["value"] = {{"status", "ready"}};
                } else if (function.find("return this.submit(id)") != std::string::npos) {
                    ++state->submits;
                    if (state->fail_submit) result["exceptionDetails"] = {{"exception", {{"description", "Uncertain send"}}}};
                    value["value"] = {{"status", "submitted"}};
                } else if (function.find("expected") != std::string::npos) value["value"] = "account";
                else if (function.find("restart") != std::string::npos) value["value"] = {{"restart", false}};
                result["result"] = value;
            }
            response["result"] = result;
        }
        return response.dump() + '\0';
    }
};
}
std::unique_ptr<Transport> create_transport() { return std::make_unique<FakeTransport>(); }
}}}

using namespace Slic3r::GUI::BambuConnect;

TEST_CASE("Connect rebinds a replaced renderer without restarting its main process", "[ConnectRuntime]")
{
    Pipe pipe;
    const auto state = latest_runtime();
    REQUIRE_FALSE(pipe.start("resources/scripts/bambu_connect_bridge.js"));
    REQUIRE_FALSE(pipe.start("resources/scripts/bambu_connect_bridge.js"));
    REQUIRE(state->attaches == 1);
    ++state->generation;
    REQUIRE(pipe.start("resources/scripts/bambu_connect_bridge.js"));
    REQUIRE(state->launches == 1);
    REQUIRE(state->attaches == 2);
    REQUIRE(state->detaches == 1);
    REQUIRE(state->prepares == 0);
    REQUIRE(state->submits == 0);
    pipe.sync_account({{"userId", "account"}, {"token", ""}, {"countryCode", "US"}}, "resources/scripts/bambu_connect_bridge.js");
    REQUIRE(pipe.handoff({{"id", "next"}, {"submit", false}}).at("status") == "ready");
    REQUIRE(state->prepares == 1);
    REQUIRE(state->submits == 0);
    REQUIRE_FALSE(pipe.start("resources/scripts/bambu_connect_bridge.js"));
}

TEST_CASE("Connect does not restart or prepare a job after a cached pipe failure", "[ConnectRuntime]")
{
    Pipe pipe;
    const auto state = latest_runtime();
    pipe.start("resources/scripts/bambu_connect_bridge.js");
    state->fail_pipe = true;
    REQUIRE_THROWS_WITH(pipe.start("resources/scripts/bambu_connect_bridge.js"), "Connect closed its pipe.");
    REQUIRE(state->launches == 1);
    REQUIRE(state->attaches == 1);
    REQUIRE(state->prepares == 0);
    REQUIRE(state->submits == 0);
}

TEST_CASE("Connect never retries a failed import or uncertain submission", "[ConnectRuntime]")
{
    for (bool submission : {false, true}) {
        Pipe pipe;
        const auto state = latest_runtime();
        pipe.start("resources/scripts/bambu_connect_bridge.js");
        state->fail_prepare = !submission;
        state->fail_submit = submission;
        std::string phase;
        REQUIRE_THROWS(pipe.handoff({{"id", "once"}, {"submit", submission}}, &phase));
        REQUIRE(phase == (submission ? "submit" : "prepare"));
        REQUIRE(state->prepares == 1);
        REQUIRE(state->submits == (submission ? 1 : 0));
        REQUIRE(state->attaches == 1);
        REQUIRE(state->launches == 1);
    }
}
