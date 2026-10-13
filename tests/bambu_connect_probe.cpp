// Developer smoke check with optional prepare-only imports. Never calls Send.
#include "slic3r/GUI/BambuConnectBridge.cpp"
#include <iostream>
#include <filesystem>
static nlohmann::json signed_in_user(const std::string& auth)
{
    return Slic3r::GUI::BambuConnect::bridge.invoke(auth, R"JS(async function(){
        const current = () => String(this().user?.uid ?? '');
        for (const deadline = Date.now() + 10000; !current() && Date.now() < deadline;)
            await new Promise(resolve => setTimeout(resolve, 100));
        return current();
    })JS").at("value");
}
int main(int argc, char** argv)
{
    if (argc < 2 || argc > 4) { std::cerr << "Usage: bambu-connect-probe <adapter.js> [--account-state | --reload | --reload-handles | prepare-only-fixtures.json [printer-name]]\n"; return 2; }
    try {
        using namespace Slic3r::GUI::BambuConnect;
        bridge.start(argv[1]);
        std::cout << "Connect version, private pipe, renderer and store bindings verified. No file imported or print sent.\n";
        if (argc == 3 && std::string(argv[2]) == "--account-state") {
            // Return presence flags only. Credentials never leave Connect and
            // this diagnostic cannot write/delete credentials or import a file.
            auto state = bridge.invoke(bridge.call("Runtime.evaluate", {{"expression",
                "import('./assets/index-B_vXgx8V.js').then(m=>m.a2)"}, {"awaitPromise", true}}).at("result").at("objectId"),
                R"JS(async function(){
                    try { return {storedCredentialReadable: !!(await this.invoke('secureStorageRead', {key:'token'}))}; }
                    catch (_) { return {storedCredentialReadable:false, storageReadFailed:true}; }
                })JS").at("value");
            // account_facade is private; obtain the same pinned module bindings.
            const auto component = bridge.call("Runtime.evaluate", {{"expression",
                "import('./assets/router-DcHfLi1L.js').then(m=>m.router.options.routeTree.options.component)"}, {"awaitPromise", true}}).at("result").at("objectId").get<std::string>();
            state["credentialLoaded"] = bridge.invoke(bridge.scope_binding(component, "Qp"), "function(){return !!this();}").at("value");
            state["accountLoaded"] = signed_in_user(bridge.scope_binding(component, "Ro")) != "";
            std::cout << state.dump() << '\n';
            bridge.show();
            return 0;
        }
        if (argc == 3 && (std::string(argv[2]) == "--reload" || std::string(argv[2]) == "--reload-handles")) {
            const bool check_account = std::string(argv[2]) == "--reload";
            const auto component = bridge.call("Runtime.evaluate", {{"expression",
                "import('./assets/router-DcHfLi1L.js').then(m=>m.router.options.routeTree.options.component)"}, {"awaitPromise", true}}).at("result").at("objectId").get<std::string>();
            const auto auth = bridge.scope_binding(component, "Ro");
            const auto environment = bridge.scope_binding(component, "Yi");
            const auto uid = check_account ? signed_in_user(auth) : nlohmann::json("");
            if (check_account && uid == "") throw std::runtime_error("Sign in to Connect before the account/renderer reload check.");
            const auto region = bridge.invoke(environment, "function(){return this().key === 'mainland' ? 'CN' : 'US';}").at("value");
            bridge.call("Runtime.evaluate", {{"expression", "window.__studioReloadProbe = true"}});
            bridge.call("Page.reload");
            bool loaded = false;
            for (int attempt = 0; attempt < 300 && !loaded; ++attempt) {
                try {
                    const auto state = bridge.call("Runtime.evaluate", {{"expression",
                        "!window.__studioReloadProbe && !!document.querySelector('#root a[href*=\"/devices\"]')"}, {"returnByValue", true}});
                    loaded = state.at("result").value("value", false);
                } catch (const BridgeCommandError&) { /* The execution context is being replaced. */ }
                if (!loaded) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!loaded) throw std::runtime_error("Reloaded Connect interface did not finish loading.");
            bool stale = false;
            try { bridge.invoke(auth, "function(){return true;}"); }
            catch (const BridgeCommandError&) { stale = true; }
            if (!stale) throw std::runtime_error("Reload did not invalidate the original account-store handle.");
            if (!bridge.start(argv[1])) throw std::runtime_error("The cached renderer was not rebound after reload.");
            if (check_account) bridge.sync_account({{"userId", uid}, {"token", ""}, {"countryCode", region}}, argv[1]);
            if (bridge.start(argv[1])) throw std::runtime_error("The refreshed renderer handles were not reusable.");
            std::cout << "Renderer reload: original account handle rejected, fresh handles rebound and reused on the same private pipe; " <<
                (check_account ? "signed-in account match verified" : "account state left unchanged") << ". No file imported or print sent.\n";
            bridge.show();
            return 0;
        }
        if (argc >= 3) {
            std::ifstream manifest(std::filesystem::u8path(argv[2]));
            const auto fixtures = json::parse(manifest);
            if (!fixtures.is_array() || fixtures.empty() || fixtures.size() > 20) throw std::runtime_error("Invalid fixture list.");
            const auto component = bridge.call("Runtime.evaluate", {{"expression",
                "import('./assets/router-DcHfLi1L.js').then(m=>m.router.options.routeTree.options.component)"}, {"awaitPromise", true}}).at("result").at("objectId").get<std::string>();
            const auto auth = bridge.scope_binding(component, "Ro");
            const auto environment = bridge.scope_binding(component, "Yi");
            const auto devices = bridge.scope_binding(component, "T_");
            const auto uid = signed_in_user(auth);
            if (uid == "") throw std::runtime_error("Sign in to Connect before preparing fixtures.");
            const auto region = bridge.invoke(environment, "function(){return this().key === 'mainland' ? 'CN' : 'US';}").at("value");
            bridge.sync_account({{"userId", uid}, {"token", ""}, {"countryCode", region}}, argv[1]);
            const auto device = bridge.invoke(devices, R"JS(async function(name){
                for (const deadline=Date.now()+10000; Date.now()<deadline;) {
                    const all=this().devices;
                    const d=name ? all.find(d=>d.name===name) : all.find(d=>d.online);
                    if(d) return {id:d.id,name:d.name,online:!!d.online};
                    await new Promise(resolve=>setTimeout(resolve,100));
                }
                return null;
            })JS", {{{"value", argc == 4 ? argv[3] : ""}}}).at("value");
            if (device.is_null()) throw std::runtime_error("The requested test printer was not found.");
            std::cout << "Test printer: " << device.at("name").get<std::string>() << "; online: " << device.at("online") << '\n';
            int index = 0;
            for (auto request : fixtures) {
                bridge.start(argv[1]);
                request["id"] = "windows-prepare-only-" + std::to_string(++index);
                request["submit"] = false; // This harness never calls the submission path.
                request["accountUserId"] = uid;
                request["deviceId"] = device.at("id");
                request["options"] = {{"timelapse", false}, {"bedLeveling", true}, {"flowCali", false}};
                request["useAms"] = true;
                const auto filament_ids = request.at("filaments");
                request.erase("filaments");
                // Opening the selected printer subscribes to its live AMS state.
                // Fresh Connect profiles have no tray reports before that step.
                bridge.handoff(request);
                const auto trays = bridge.invoke(devices, R"JS(async function(id,count){
                    for (const deadline=Date.now()+15000; Date.now()<deadline;) {
                        const d=this().devices.find(d=>d.id===id);
                        const trays=(d?.amsFilament?.amsItems??[]).flatMap(a=>a.trays??[]).filter(t=>t.type==='ams');
                        if(trays.length>=count) return trays;
                        await new Promise(resolve=>setTimeout(resolve,100));
                    }
                    return [];
                })JS", {{{"value", device.at("id")}}, {{"value", filament_ids.size()}}}).at("value");
                request["mappings"] = json::array();
                size_t slot = 0;
                const bool has_trays = trays.size() >= filament_ids.size();
                for (const auto& filament : filament_ids) {
                    if (!has_trays) break;
                    const auto& tray = trays[slot++];
                    request["mappings"].push_back({{"filamentId", filament}, {"amsId", std::stoi(tray.at("amsId").get<std::string>())},
                        {"slotId", std::stoi(tray.at("slotId").get<std::string>())}, {"external", false}});
                }
                const auto result = bridge.handoff(request);
                if (result.value("status", "") != "ready" || !result.value("dialogReady", false) ||
                    result.at("plateIndex") != request.at("plateIndex") || !result.value("useAms", false))
                    throw std::runtime_error("Prepared fixture does not match its requested plate/dialog/AMS state.");
                for (const auto& item : request.at("options").items())
                    if (result.at("options").at(item.key()) != item.value()) throw std::runtime_error("Print option mismatch.");
                for (const auto& expected : request.at("mappings")) {
                    bool found = false;
                    for (const auto& actual : result.at("mappings")) {
                        if (actual.at("filamentId") != std::to_string(expected.at("filamentId").get<int>())) continue;
                        found = actual.at("type") == "ams" && std::stoi(actual.at("amsId").get<std::string>()) == expected.at("amsId").get<int>() &&
                            std::stoi(actual.at("slotId").get<std::string>()) == expected.at("slotId").get<int>();
                    }
                    if (!found) throw std::runtime_error("AMS mapping mismatch.");
                }
                std::cout << request.at("name").get<std::string>() << ": plate/options verified; " <<
                    (has_trays ? "AMS mappings verified" : "live AMS validation pending (no slot reports)") << "; no print sent.\n";
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
