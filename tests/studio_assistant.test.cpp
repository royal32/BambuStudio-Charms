#include "slic3r/GUI/StudioAssistantClient.hpp"
#include "slic3r/GUI/StudioAssistantProfiles.hpp"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <curl/curl.h>
#include <cassert>
#include <iostream>
#include <set>

using namespace Slic3r::GUI::Assistant;
namespace Slic3r { namespace GUI { namespace Assistant {
struct ClientTestAccess {
    static Json credentials(Client& client,const Json& account,std::atomic<bool>& cancel) { return client.credentials(account,cancel); }
    static void expire(Client& client,const std::string& id) { client.m_credentials.at(id)["expires_at"]=0; }
};
}}}
namespace {
template<class F> void rejects(F&& fn) { bool rejected=false; try { fn(); } catch (...) { rejected=true; } assert(rejected); }
std::string bignum(const BIGNUM* value) {
    std::string bytes(BN_num_bytes(value), '\0'); BN_bn2bin(value, reinterpret_cast<unsigned char*>(&bytes[0])); return base64url(bytes);
}
std::string jwt(EVP_PKEY* key, const Json& claims, const Json& header = {{"alg", "RS256"}, {"kid", "test"}}) {
    const auto body = base64url(header.dump()) + "." + base64url(claims.dump());
    auto* ctx = EVP_MD_CTX_new(); assert(EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1);
    size_t size=0; assert(EVP_DigestSign(ctx, nullptr, &size, reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 1);
    std::string signature(size, '\0'); assert(EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(&signature[0]), &size, reinterpret_cast<const unsigned char*>(body.data()), body.size()) == 1);
    signature.resize(size); EVP_MD_CTX_free(ctx); return body + "." + base64url(signature);
}
}
int main()
{
    {
        const Json body={{"error",{{"code","subscription_sharing_usage_limit_exceeded"},{"param",nullptr},{"message","provider detail must not appear in chat"}}}};
        RequestError limit(429,body,"req_synthetic");
        assert(std::string(limit.what()).find("Manage usage")!=std::string::npos);
        assert(std::string(limit.what()).find("provider detail")==std::string::npos);
        assert(limit.response()==body && limit.diagnostics().at("request_id")=="req_synthetic");
        assert(limit.diagnostics().at("param")=="");
        RequestError admission(403,{{"detail","unstructured admission text"}},"req_admission");
        assert(std::string(admission.what()).find("permissions")!=std::string::npos);
        assert(admission.response().contains("detail"));
        assert(admission.diagnostics().dump().find("unstructured admission text")==std::string::npos);
        RequestError temporary(503,{{"error",{{"code","subscription_sharing_usage_unavailable"}}}});
        assert(std::string(temporary.what()).find("temporarily unavailable")!=std::string::npos);
        assert(std::string(temporary.what()).find("sign in")==std::string::npos);
        assert(!temporary.diagnostics().contains("recovery"));
        RequestError expired(400,{{"error",{{"code","invalid_grant"}}}});
        assert(expired.diagnostics().at("recovery")=="sign_in");
        Callbacks missing;
        missing.load_secret=[](const std::string&) { return Json::object(); };
        Client no_session("test-host",missing); std::atomic<bool> not_cancelled{false};
        try {
            ClientTestAccess::credentials(no_session,{{"client_id","test"},{"subject","test"}},not_cancelled);
            assert(false);
        } catch (const RequestError& error) { assert(error.diagnostics().at("recovery")=="sign_in"); }
        RequestError ineligible(403,{{"error",{{"code","subscription_sharing_user_not_eligible"}}}});
        assert(std::string(ineligible.what()).find("isn't available for this account")!=std::string::npos);
        const auto timeout=RequestError::connection(CURLE_OPERATION_TIMEDOUT);
        assert(std::string(timeout.what()).find("too long")!=std::string::npos);
        assert(timeout.diagnostics().at("connection_error")==CURLE_OPERATION_TIMEDOUT);
        assert(std::string(RequestError::connection(CURLE_COULDNT_CONNECT).what()).find("internet connection")!=std::string::npos);
        RequestError unusual(400,{{"error",{{"code",std::string(255,'x')+"é"},{"param","a\nparameter"}}}},std::string(255,'r')+"é");
        assert(!unusual.diagnostics().dump().empty());
        assert(unusual.diagnostics().at("code").get<std::string>().size()<=256);
        assert(unusual.diagnostics().at("param")=="a?parameter");
        for (const auto& packet:std::vector<std::string>{"{\"private\":\"PRIVATE_SENTINEL", "{\"type\":7,\"private\":\"PRIVATE_SENTINEL\"}","null"}) {
            try {
                ResponseStream decoder;
                decoder.feed("data: "+packet+"\n\n");
                assert(false);
            } catch (const RequestError& error) {
                assert(std::string(error.what()).find("PRIVATE_SENTINEL")==std::string::npos);
                assert(error.diagnostics().dump().find("PRIVATE_SENTINEL")==std::string::npos);
            }
        }
        // Error events inside an otherwise-open stream keep the structured
        // cause, rather than discarding it behind a generic parse failure.
        try {
            ResponseStream decoder;
            decoder.feed("data: "+Json({{"type","response.failed"},{"response",{{"error",{{"code","subscription_sharing_usage_limit_exceeded"}}}}}}).dump()+"\n\n");
            assert(false);
        } catch (const RequestError& error) {
            assert(error.diagnostics().at("code")=="subscription_sharing_usage_limit_exceeded");
            assert(std::string(error.what()).find("Manage usage")!=std::string::npos);
        }
        try {
            ResponseStream decoder;
            decoder.feed("data: "+Json({{"type","response.failed"},{"response",{{"error",{{"code","subscription_sharing_usage_unavailable"}}}}}}).dump());
            decoder.finish(); // A final frame need not end with a blank line.
            assert(false);
        } catch (const RequestError& error) {
            assert(error.diagnostics().at("code")=="subscription_sharing_usage_unavailable");
            assert(std::string(error.what()).find("temporarily unavailable")!=std::string::npos);
        }
    }
    {
        int reads=0,deletes=0; bool removed=false; std::string token="first";
        Callbacks callbacks;
        callbacks.load_secret=[&](const std::string& id) {
            ++reads;
            return removed?Json::object():Json{{"subject",id},{"access_token",token},{"expires_at",INT64_C(4102444800)}};
        };
        callbacks.save_secret=[](const std::string&,const Json&) { assert(false); };
        callbacks.delete_secret=[&](const std::string&) { ++deletes; removed=true; return true; };
        Client client("test-host",callbacks); std::atomic<bool> cancel{false};
        const Json account={{"client_id","account-a"},{"subject","account-a"}};
        for (int i=0;i<100;++i) assert(ClientTestAccess::credentials(client,account,cancel).at("access_token")=="first");
        assert(reads==1); // A tool loop must not repeatedly prompt in Keychain.
        ClientTestAccess::credentials(client,{{"client_id","account-b"},{"subject","account-b"}},cancel);
        assert(reads==2); // Accounts have independent in-memory credentials.
        token="renewed-by-another-process"; ClientTestAccess::expire(client,"account-a");
        assert(ClientTestAccess::credentials(client,account,cancel).at("access_token")==token); assert(reads==3);
        assert(client.sign_out(account,cancel)); assert(deletes==1); assert(reads==3);
        rejects([&] { ClientTestAccess::credentials(client,account,cancel); }); assert(reads==4);
    }
    {
        int automatic_reads=0,explicit_authorizations=0;
        Callbacks callbacks;
        callbacks.load_secret=[&](const std::string&) -> Json { ++automatic_reads; throw std::runtime_error("Keychain authorization needed"); };
        callbacks.authorize_secret=[&](const std::string& client,std::atomic<bool>& cancellation) {
            assert(!cancellation.load());
            ++explicit_authorizations;
            return Json{{"subject",client},{"access_token","synthetic"},{"expires_at",INT64_C(4102444800)}};
        };
        Client client("test-host",callbacks); std::atomic<bool> cancel{false};
        const Json account={{"client_id","account-a"},{"subject","account-a"}};
        rejects([&] { ClientTestAccess::credentials(client,account,cancel); });
        assert(automatic_reads==1 && explicit_authorizations==0);
        client.authorize_credentials(account,cancel);
        for (int i=0;i<100;++i) assert(ClientTestAccess::credentials(client,account,cancel).at("access_token")=="synthetic");
        assert(automatic_reads==1 && explicit_authorizations==1);
        rejects([&] { client.authorize_credentials({{"client_id","account-a"},{"subject","someone-else"}},cancel); });
    }
    assert(base64url("f") == "Zg"); assert(base64url("fo") == "Zm8"); assert(base64url("foo") == "Zm9v");
    assert(base64url(sha256("abc")) == "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0");
    assert(random_string() != random_string());
    assert(form_encode({{"a", "x y&+=/?"}}) == "a=x%20y%26%2B%3D%2F%3F");
    const std::string prefix = "GET /auth/callback?", suffix = " HTTP/1.1\r\nHost: 127.0.0.1:1234\r\n\r\n";
    const auto callback = callback_parameters(prefix + "code=a%2Bb&state=xyz&client_id=oaiapp_test" + suffix, "127.0.0.1:1234");
    assert(callback.at("code") == "a+b"); assert(callback.at("state") == "xyz");
    rejects([&] { callback_parameters(prefix + "state=x&state=y" + suffix, "127.0.0.1:1234"); });
    rejects([&] { callback_parameters(prefix + "code=%0A" + suffix, "127.0.0.1:1234"); });
    rejects([&] { callback_parameters(prefix + "code=%QQ" + suffix, "127.0.0.1:1234"); });
    rejects([&] { callback_parameters(prefix + "code=x" + suffix, "evil.example"); });
    rejects([&] { callback_parameters("GET /callback?state=x" + suffix, "127.0.0.1:1234"); });
    rejects([&] { callback_parameters(prefix + "code=x HTTP/1.1\r\nHost: 127.0.0.1:1234\r\nHost: evil\r\n\r\n", "127.0.0.1:1234"); });

    EVP_PKEY_CTX* generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr); assert(generator);
    assert(EVP_PKEY_keygen_init(generator) == 1); assert(EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) == 1);
    EVP_PKEY* key=nullptr; assert(EVP_PKEY_keygen(generator, &key) == 1); EVP_PKEY_CTX_free(generator);
    const RSA* rsa = EVP_PKEY_get0_RSA(key); const BIGNUM *n=nullptr,*e=nullptr; RSA_get0_key(rsa,&n,&e,nullptr);
    const Json jwks = {{"keys", Json::array({{{"kid", "test"}, {"kty", "RSA"}, {"n", bignum(n)}, {"e", bignum(e)}}})}};
    Json claims = {{"iss", "https://auth.openai.com"}, {"aud", "oaiapp_test"}, {"sub", "test-person"}, {"nonce", "expected"}, {"exp", 2000}, {"iat", 900}};
    auto signed_token = jwt(key, claims);
    assert(verify_id_token(signed_token, jwks, "oaiapp_test", "expected", 1000).at("sub") == "test-person");
    rejects([&] { verify_id_token(signed_token, jwks, "other-client", "expected", 1000); });
    rejects([&] { verify_id_token(signed_token, jwks, "oaiapp_test", "other-nonce", 1000); });
    rejects([&] { verify_id_token(signed_token, jwks, "oaiapp_test", "expected", 2000); });
    auto forged = signed_token; forged[forged.find('.')+2] = forged[forged.find('.')+2] == 'a' ? 'b' : 'a';
    rejects([&] { verify_id_token(forged, jwks, "oaiapp_test", "expected", 1000); });
    claims["iss"] = "https://evil.example";
    rejects([&] { verify_id_token(jwt(key, claims), jwks, "oaiapp_test", "expected", 1000); });
    claims["iss"] = "https://auth.openai.com"; claims["aud"] = Json::array({"oaiapp_test", "other"});
    rejects([&] { verify_id_token(jwt(key, claims), jwks, "oaiapp_test", "expected", 1000); });
    claims["azp"] = "oaiapp_test";
    assert(verify_id_token(jwt(key, claims), jwks, "oaiapp_test", "expected", 1000).at("sub") == "test-person");
    rejects([&] { verify_id_token(jwt(key, claims, {{"alg", "none"}, {"kid", "test"}}), jwks, "oaiapp_test", "expected", 1000); });
    EVP_PKEY_free(key);

    const Json completed = {{"type", "response.completed"}, {"response", {{"status", "completed"}, {"output", Json::array({{{"type", "function_call"}, {"name", "slice"}, {"call_id", "call_1"}, {"arguments", "{}"}}})}}}};
    const auto stream = std::string(": keepalive\r\n\r\n") + "data: " + Json({{"type", "response.output_text.delta"}, {"delta", "Hello 🌸"}}).dump() + "\r\n\r\ndata: " + completed.dump() + "\r\n\r\ndata: [DONE]\r\n\r\n";
    for (size_t chunk : {1u, 2u, 7u, 1024u}) {
        ResponseStream decoder; std::string text; decoder.on_text = [&](const std::string& delta) { text += delta; };
        for (size_t offset=0;offset<stream.size();offset+=chunk) decoder.feed(stream.substr(offset,chunk));
        assert(decoder.finish().at("output").at(0).at("name") == "slice"); assert(text == "Hello 🌸");
    }
    rejects([] { ResponseStream decoder; decoder.feed("data: {\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"); decoder.finish(); });
    rejects([] { ResponseStream decoder; decoder.feed("data: {\"type\":\"response.incomplete\"}\n\n"); });
    rejects([] { ResponseStream decoder; decoder.feed("data: {\"type\":\"error\"}\n\n"); });
    {
        ResponseStream decoder; std::string text; decoder.on_text=[&](const std::string& delta) { text+=delta; };
        Json message={{"type","message"},{"role","assistant"},{"content",Json::array({{{"type","output_text"},{"text","Completed item text"}}})}};
        decoder.feed("data: "+Json({{"type","response.output_item.done"},{"output_index",0},{"item",message}}).dump()+"\n\n");
        decoder.feed("data: "+Json({{"type","response.completed"},{"response",{{"status","completed"},{"output",Json::array()}}}}).dump()+"\n\n");
        assert(decoder.finish().at("output").at(0)==message); assert(text=="Completed item text");
    }
    rejects([] {
        ResponseStream decoder;
        decoder.feed("data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\"}}\n\n");
        decoder.finish(); // A completed item alone never authorizes execution.
    });
    const auto tools = tool_definitions(); assert(tools.at(0).at("type") == "namespace"); assert(tools.at(0).at("name") == "studio");
    const auto runtime=runtime_capabilities();std::set<std::string> declared,advertised;
    for(const auto& tool:tools.at(0).at("tools"))declared.insert(tool.at("name").get<std::string>());
    for(const auto& name:runtime.at("available_tools"))advertised.insert(name.get<std::string>());
    assert(declared==advertised && !advertised.count("shell") && !advertised.count("browser"));
    assert(runtime.at("turn_limits").at("model_rounds")==assistant_max_rounds && runtime.at("turn_limits").at("tool_actions")==assistant_max_actions);
    assert(changes_project("set_settings")); assert(!changes_project("inspect_project")); assert(requires_confirmation("document"));
    assert(changes_project("plane_cut") && changes_project("add_primitive") && changes_project("delete_parts"));
    assert(!changes_project("model_section") && requires_confirmation("delete_parts"));
    std::set<std::string> geometry_names;
    for (const auto& tool:tools.at(0).at("tools")) geometry_names.insert(tool.at("name").get<std::string>());
    for (const auto* name:{"plane_cut","add_primitive","delete_parts","model_section"}) assert(geometry_names.count(name));
    assert(geometry_names.size()==tools.at(0).at("tools").size() && geometry_names.size()<=128);
    for(const auto* name:{"inspect_surfaces","surface_view","paint_surfaces","clear_paint","transform_item","place_on_bed","place_on_face","create_object","list_fonts","add_text","select_instances","detach_instances","layout_selection","instance_printable","object_structure","part_properties","plate_operation","delete_plate","reset_overrides","load_model_file","mesh_boolean","simplify_mesh","inspect_slice","slice_layer_view","slice_all","library_export","review_print"})assert(geometry_names.count(name));
    for(const auto& definition:workbench_tool_definitions())if(changes_project(definition.at("name"))) {
        const auto& schema=definition.at("parameters");assert(schema.at("properties").contains("revision"));
        const auto required=schema.at("required").get<std::vector<std::string>>();assert(std::find(required.begin(),required.end(),"revision")!=required.end());
    }
    assert(!requires_confirmation("plate_operation") && requires_confirmation("delete_plate"));
    assert(!requires_confirmation("use_profile",{{"kind","filament"},{"slot",0}}));
    assert(requires_confirmation("use_profile",{{"kind","filament"},{"slot",1}}));
    assert(requires_confirmation("use_profile",{{"kind","printer"}}));
    assert(geometry_names.count("inspect_printers") && !changes_project("inspect_printers"));
    assert(geometry_names.count("inspect_profile_settings") && !changes_project("inspect_profile_settings"));
    assert(geometry_names.count("part_map_view") && !changes_project("part_map_view"));
    const auto profile_config=Slic3r::DynamicPrintConfig::full_print_config();
    assert(readable_profile_parameter("nozzle_temperature",*profile_config.def()->get("nozzle_temperature")));
    assert(readable_profile_parameter("filament_type",*profile_config.def()->get("filament_type")));
    Slic3r::ConfigOptionDef text_parameter;text_parameter.type=Slic3r::coString;
    assert(!readable_profile_parameter("bed_custom_model",text_parameter));
    assert(!readable_profile_parameter("printhost_apikey",text_parameter));
    assert(!readable_profile_parameter("machine_start_gcode",text_parameter));
    assert(!changes_project("surface_view") && !changes_project("slice_layer_view") && !changes_project("review_print"));
    for (const auto& tool : tools.at(0).at("tools")) assert(tool.at("name") != "send_print");
    auto call=[](const std::string& name,const Json& args,const std::string& call_id="call_1") {
        return Json{{"status","completed"},{"output",Json::array({{{"type","function_call"},{"namespace","studio"},{"name",name},{"call_id",call_id},{"arguments",args.dump()}}})}};
    };
    const Json final={{"status","completed"},{"output",Json::array({{{"type","message"},{"role","assistant"},{"content",Json::array({{{"type","output_text"},{"text","Resliced. The loop still needs inspection."}}})}}})}};
    std::vector<Json> responses={call("inspect_project",Json::object()),call("set_settings",{{"revision","r1"},{"scope","global"},{"settings",Json::array({{{"key","enable_support"},{"value","1"}}})}}),call("slice",Json::object()),final};
    size_t response_index=0; std::vector<std::string> executed;
    SessionCallbacks session;
    std::vector<Json> trace_events;
    session.trace=[&](const std::string& event,const Json& data) { trace_events.push_back({{"event",event},{"data",data}}); };
    session.cancelled=[] { return false; };
    session.respond=[&](const Json& request) {
        assert(request.at("store")==false); assert(request.at("stream")==true);
        assert(!request.contains("previous_response_id"));
        if (response_index==3) {
            const auto& last=request.at("input").back(); assert(last.at("type")=="function_call_output");
            assert(Json::parse(last.at("output").get<std::string>()).at("warnings").size()==1);
        }
        return responses.at(response_index++);
    };
    session.tool=[&](const std::string& name,const Json&) {
        executed.push_back(name);
        return Json{{"revision",name=="set_settings"?"r2":"r1"},{"warnings",Json::array({"unsupported loop"})}};
    };
    const auto history=run_turn(Json::array(),{{"revision","r1"}},"Help with this warning","test-model",session);
    assert((executed==std::vector<std::string>{"inspect_project","set_settings","slice"}));
    assert(history.back().at("type")=="message");
    assert(trace_events.front().at("event")=="turn_start");
    assert(trace_events.front().at("data").at("context").at("revision")=="r1");
    assert(trace_events.back().at("event")=="turn_end");
    assert(trace_events.back().at("data").at("status")=="completed");
    std::vector<std::string> traced_tools;
    for(const auto& entry:trace_events) if(entry.at("event")=="tool_result") {
        traced_tools.push_back(entry.at("data").at("name"));
        assert(entry.at("data").at("call_id")=="call_1");
        assert(entry.at("data").at("result").at("warnings").size()==1);
    }
    assert(traced_tools==executed);
    // A diagnostic sink failure must not prevent the response or tool action.
    session.trace=[](const std::string&,const Json&){throw std::runtime_error("Disk full");};
    response_index=0;executed.clear();
    run_turn(Json::array(),{{"revision","r1"}},"Help with this warning","test-model",session);
    assert((executed==std::vector<std::string>{"inspect_project","set_settings","slice"}));

    // Keep visible replies and proposed tools, but never persist opaque model
    // reasoning/encrypted state from an otherwise valid provider response.
    trace_events.clear();session.trace=[&](const std::string& event,const Json& data){trace_events.push_back({{"event",event},{"data",data}});};
    session.respond=[&](const Json&) {
        auto response=final;
        response["output"].push_back({{"type","reasoning"},{"encrypted_content","private-opaque-state"}});
        return response;
    };
    run_turn(Json::array(),Json::object(),"test","test-model",session);
    const auto logged=Json(trace_events).dump();
    assert(logged.find("Resliced. The loop still needs inspection.")!=std::string::npos);
    assert(logged.find("private-opaque-state")==std::string::npos);
    session.trace={};

    // Model labels, requests and logs use the same explicit effort policy.
    session.respond=[&](const Json& request) {
        assert(request.at("reasoning").at("effort")=="max"); return final;
    };
    run_turn(Json::array(),Json::object(),"test","gpt-6-luna",session);
    session.respond=[&](const Json& request) {
        assert(request.at("reasoning").at("effort")=="high");
        bool contract=false;for(const auto& item:request.at("input"))if(item.value("role",std::string())=="developer"&&item.value("content",std::string()).find("Application-provided Studio runtime contract:")==0)contract=true;
        assert(contract);return final;
    };
    session.trace=[&](const std::string& event,const Json& data){if(event=="turn_start")assert(data.at("reasoning_effort")=="high");};
    run_turn(Json::array(),Json::object(),"test","gpt-6.1-terra",session);
    session.trace={};
    session.respond=[&](const Json& request) {
        assert(!request.contains("reasoning")); return final;
    };
    run_turn(Json::array(),Json::object(),"test","gpt-6.1-sol",session);

    // A missing revision or an unsupported command never reaches the app.
    for (const auto& invalid : std::vector<Json>{call("set_settings",{{"scope","global"},{"settings",Json::array()}}),call("shell",{{"command","anything"}}),call("copies",{{"revision","r1"},{"object_id","o"},{"additional",999}}),
        call("assign_part_filament",{{"revision","r1"},{"object_id","o"},{"part_ids",Json::array({"v"})},{"slot",0}}),
        call("assign_part_filament",{{"object_id","o"},{"part_ids",Json::array({"v"})},{"slot",1}}),
        call("document",{{"action","unknown"}}),
        call("split_parts",{{"revision","r1"},{"object_id","o"},{"part_ids",Json::array({"v1","v2"})}}),
        call("plane_cut",{{"object_id","o"},{"instance_id","i"},{"part_ids",{"v"}},{"point_mm",{0,0,0}},{"normal",{0,1,0}},{"keep","negative"}}),
        call("add_primitive",{{"revision","r1"},{"object_id","o"},{"instance_id","i"},{"shape","cylinder"},{"type","negative"},{"center_mm",{0,0}},{"size_mm",{2,2,4}},{"axis",{0,0,1}}}),
        call("delete_parts",{{"object_id","o"},{"part_ids",{"v"}}})}) {
        response_index=0; bool executed_bad=false;
        session.respond=[&](const Json& request) {
            if (response_index++==0) return invalid;
            assert(Json::parse(request.at("input").back().at("output").get<std::string>()).contains("error"));
            return final;
        };
        session.tool=[&](const std::string&,const Json&) { executed_bad=true; return Json::object(); };
        run_turn(Json::array(),Json::object(),"test","test-model",session); assert(!executed_bad);
    }
    // Partial/failed model output cannot dispatch actions, even if it contains a call.
    bool executed_partial=false;
    session.tool=[&](const std::string&,const Json&) { executed_partial=true; return Json::object(); };
    session.respond=[&](const Json&) { auto partial=call("slice",Json::object()); partial["status"]="incomplete"; return partial; };
    rejects([&] { run_turn(Json::array(),Json::object(),"test","test-model",session); }); assert(!executed_partial);
    // Stop between inference and execution must prevent the pending action.
    bool cancelled=false; session.cancelled=[&] { return cancelled; };
    trace_events.clear();session.trace=[&](const std::string& event,const Json& data){trace_events.push_back({{"event",event},{"data",data}});};
    session.respond=[&](const Json&) { cancelled=true; return call("slice",Json::object()); };
    rejects([&] { run_turn(Json::array(),Json::object(),"test","test-model",session); }); assert(!executed_partial);
    assert(trace_events.back().at("data").at("status")=="cancelled");
    session.trace={};
    session.cancelled=[] { return false; };
    // Native-tool failure is returned as an error, not converted into success or automatically replayed.
    response_index=0; int attempts=0;
    session.respond=[&](const Json& request) {
        if (response_index++==0) return call("arrange",{{"revision","r1"}});
        assert(Json::parse(request.at("input").back().at("output").get<std::string>()).at("error")=="stale project"); return final;
    };
    session.tool=[&](const std::string&,const Json&) -> Json { ++attempts; throw std::runtime_error("stale project"); };
    run_turn(Json::array(),Json::object(),"test","test-model",session); assert(attempts==1);
    const Json rename_args={{"revision","r1"},{"object_id","object-a"},{"name","First"}};
    // A model can retry on later responses even when parallel calls are off.
    // Native exceptions and both failure-result forms must all stop exact repeats.
    for (int failure_kind=0;failure_kind<3;++failure_kind) {
        response_index=0; attempts=0; int inspections=0;
        session.respond=[&](const Json& request) {
            switch (response_index++) {
            case 0: return call("rename",rename_args,"failed_first");
            case 1: {
                auto retry=call("studio.rename",rename_args,"failed_retry");
                retry["output"][0]["arguments"]="{\"revision\":\"r1\",\"object_id\":\"object-a\",\"name\":\"First\"}";
                return retry;
            }
            case 2: {
                const auto skipped=Json::parse(request.at("input").back().at("output").get<std::string>());
                assert(skipped.at("skipped")==true && skipped.contains("error"));
                return call("inspect_project",Json::object(),"inspect_failed");
            }
            case 3: {
                auto changed=rename_args; changed["revision"]="r2";
                return call("rename",changed,"changed_revision");
            }
            case 4: {
                auto changed=rename_args; changed["revision"]="r2"; changed["name"]="Second";
                return call("rename",changed,"changed_arguments");
            }
            default: return final;
            }
        };
        session.tool=[&](const std::string& name,const Json&) -> Json {
            if (name=="inspect_project") { ++inspections; return Json{{"revision","r2"}}; }
            assert(name=="rename"); ++attempts;
            if (failure_kind==0) throw std::runtime_error("native rename failed");
            return failure_kind==1?Json{{"error","native rename failed"}}:Json{{"operation_error","native rename failed"}};
        };
        const auto failure_history=run_turn(Json::array(),Json::object(),"Rename the object","test-model",session);
        assert(attempts==3 && inspections==1);
        // The failed-signature memory belongs to this user turn only.
        response_index=0; attempts=0;
        session.respond=[&](const Json&) { return response_index++==0?call("rename",rename_args,"new_user_retry"):final; };
        run_turn(failure_history,Json::object(),"Try renaming again","test-model",session);
        assert(attempts==1);
    }
    // Successful operations are not deduplicated merely because args repeat.
    response_index=0; attempts=0;
    session.respond=[&](const Json&) { return response_index++<2?call("rename",rename_args,"success_"+std::to_string(response_index)):final; };
    session.tool=[&](const std::string&,const Json&) { ++attempts; return Json{{"revision","r2"}}; };
    run_turn(Json::array(),Json::object(),"Rename the object","test-model",session);
    assert(attempts==2);
    // Validation failures never dispatch, and an untouched sibling skipped
    // after such a failure remains eligible on the next response.
    response_index=0; attempts=0;
    session.respond=[&](const Json&) {
        switch (response_index++) {
        case 0: {
            auto batch=call("rename",{{"name","First"}},"invalid_rename");
            batch["output"].push_back(call("rename",rename_args,"skipped_rename").at("output").at(0));
            return batch;
        }
        case 1: return call("rename",rename_args,"dispatch_after_skip");
        default: return final;
        }
    };
    run_turn(Json::array(),Json::object(),"Rename the object","test-model",session);
    assert(attempts==1);
    // Read-only inspections and slicing retain their existing retry behavior.
    for (const auto& item : std::vector<std::pair<std::string,Json>>{{"inspect_settings",{{"query","support"}}},{"slice",Json::object()}}) {
        response_index=0; attempts=0;
        session.respond=[&](const Json&) { return response_index++<2?call(item.first,item.second,"retry_"+std::to_string(response_index)):final; };
        session.tool=[&](const std::string&,const Json&) { ++attempts; return attempts==1?Json{{"error","temporary failure"}}:Json{{"revision","r2"}}; };
        run_turn(Json::array(),Json::object(),"test","test-model",session);
        assert(attempts==2);
    }
    // A failed/cancelled action invalidates the rest of a preplanned batch.
    // Preserve one output per call so the model can reassess the actual state.
    for (const auto& failure : std::vector<Json>{{{"cancelled",true}},{{"error","stale project"}},{{"operation_error","Slicing failed"}}}) {
        response_index=0; executed.clear();
        auto batch=call("arrange",{{"revision","r1"}},"call_first");
        batch["output"].push_back(call("slice",Json::object(),"call_second").at("output").at(0));
        session.respond=[&](const Json& request) {
            if (response_index++==0) return batch;
            const auto& input=request.at("input");
            assert(input.at(input.size()-2).at("call_id")=="call_first");
            assert(Json::parse(input.at(input.size()-2).at("output").get<std::string>())==failure);
            assert(input.back().at("call_id")=="call_second");
            const auto skipped=Json::parse(input.back().at("output").get<std::string>());
            assert(skipped.at("skipped")==true && skipped.contains("error"));
            return final;
        };
        session.tool=[&](const std::string& name,const Json&) { executed.push_back(name); return failure; };
        run_turn(Json::array(),Json::object(),"test","test-model",session);
        assert((executed==std::vector<std::string>{"arrange"}));
    }
    response_index=0; executed.clear();
    session.respond=[&](const Json&) {
        if (response_index++!=0) return final;
        auto batch=call("inspect_project",Json::object(),"inspect_first");
        batch["output"].push_back(call("inspect_settings",{{"query","supports"}},"inspect_second").at("output").at(0));
        return batch;
    };
    session.tool=[&](const std::string& name,const Json&) { executed.push_back(name); return Json{{"revision","r1"}}; };
    run_turn(Json::array(),Json::object(),"test","test-model",session);
    assert((executed==std::vector<std::string>{"inspect_project","inspect_settings"}));
    // A declined native dialog cannot be reopened by a model retry in the same
    // turn, including the alternate qualified tool name. A new user turn can.
    response_index=0; executed.clear();
    session.respond=[&](const Json& request) {
        if (response_index++==0) return call("document",{{"action","save"}},"save_first");
        if (response_index==2) return call("studio.document",{{"action","save"}},"save_retry");
        if (response_index==3) {
            const auto skipped=Json::parse(request.at("input").back().at("output").get<std::string>());
            assert(skipped.at("cancelled")==true && skipped.at("skipped")==true);
            return call("inspect_project",Json::object(),"inspect_after_cancel");
        }
        return final;
    };
    session.tool=[&](const std::string& name,const Json&) {
        executed.push_back(name); return name=="document"?Json{{"cancelled",true}}:Json{{"revision","r1"}};
    };
    const auto cancelled_history=run_turn(Json::array(),Json::object(),"Save this project","test-model",session);
    assert((executed==std::vector<std::string>{"document","inspect_project"}));
    response_index=0; executed.clear();
    session.respond=[&](const Json&) { return response_index++==0?call("document",{{"action","save"}},"save_new_turn"):final; };
    run_turn(cancelled_history,Json::object(),"Try saving again","test-model",session);
    assert((executed==std::vector<std::string>{"document"}));
    // Import/export dialogs may report no native outcome. Never assume their
    // dependent actions are ready, or reopen the same chooser to find out.
    response_index=0; executed.clear();
    const Json unconfirmed={{"outcome","unconfirmed"},{"message","Studio did not report whether the import completed."}};
    session.respond=[&](const Json& request) {
        switch (response_index++) {
        case 0: {
            auto batch=call("document",{{"action","import"}},"import_first");
            batch["output"].push_back(call("document",{{"action","save"}},"save_before_inspection").at("output").at(0));
            return batch;
        }
        case 1: {
            const auto& input=request.at("input");
            assert(input.at(input.size()-2).at("call_id")=="import_first");
            assert(Json::parse(input.at(input.size()-2).at("output").get<std::string>())==unconfirmed);
            assert(input.back().at("call_id")=="save_before_inspection");
            const auto skipped=Json::parse(input.back().at("output").get<std::string>());
            assert(skipped.at("skipped")==true);
            assert(skipped.at("error").get<std::string>().find("needs its result checked")!=std::string::npos);
            return call("studio.document",{{"action","import"}},"import_retry");
        }
        case 2: {
            const auto skipped=Json::parse(request.at("input").back().at("output").get<std::string>());
            assert(skipped.at("outcome")=="unconfirmed" && skipped.at("skipped")==true);
            assert(!skipped.contains("cancelled") && !skipped.contains("error"));
            return call("inspect_project",Json::object(),"inspect_import_result");
        }
        case 3: {
            const auto inspected=Json::parse(request.at("input").back().at("output").get<std::string>());
            assert(inspected.at("objects").at(0).at("id")=="imported-model");
            return call("document",{{"action","save"}},"save_after_inspection");
        }
        default: return final;
        }
    };
    session.tool=[&](const std::string& name,const Json& args) {
        executed.push_back(name=="document"?name+":"+args.at("action").get<std::string>():name);
        if (name=="document" && args.at("action")=="import") return unconfirmed;
        if (name=="inspect_project") return Json{{"revision","r2"},{"objects",Json::array({{{"id","imported-model"}}})}};
        return Json{{"saved",true}};
    };
    const auto import_history=run_turn(Json::array(),Json::object(),"Import a model and save the project","test-model",session);
    assert((executed==std::vector<std::string>{"document:import","inspect_project","document:save"}));
    response_index=0; executed.clear();
    session.respond=[&](const Json&) { return response_index++==0?call("document",{{"action","import"}},"import_new_turn"):final; };
    run_turn(import_history,Json::object(),"Import another model","test-model",session);
    assert((executed==std::vector<std::string>{"document:import"}));
    // Library opens have no native success signal either. Block retrying the
    // same validated entry ID, while leaving other entries and new turns usable.
    response_index=0; executed.clear();
    session.respond=[&](const Json& request) {
        switch (response_index++) {
        case 0: return call("open_library_project",{{"entry_id","source-a"}},"library_first");
        case 1: return call("studio.open_library_project",{{"entry_id","source-a"}},"library_retry");
        case 2: {
            const auto skipped=Json::parse(request.at("input").back().at("output").get<std::string>());
            assert(skipped.at("outcome")=="unconfirmed" && skipped.at("skipped")==true);
            assert(!skipped.contains("cancelled") && !skipped.contains("error"));
            return call("inspect_library_source",{{"entry_id","source-a"}},"library_inspect");
        }
        case 3: return call("open_library_project",{{"entry_id","source-b"}},"library_distinct");
        default: return final;
        }
    };
    session.tool=[&](const std::string& name,const Json& args) {
        executed.push_back(name+":"+args.at("entry_id").get<std::string>());
        return name=="open_library_project"?unconfirmed:Json{{"objects",Json::array()}};
    };
    const auto library_history=run_turn(Json::array(),Json::object(),"Open the requested library project","test-model",session);
    assert((executed==std::vector<std::string>{"open_library_project:source-a","inspect_library_source:source-a","open_library_project:source-b"}));
    response_index=0; executed.clear();
    session.respond=[&](const Json&) { return response_index++==0?call("open_library_project",{{"entry_id","source-a"}},"library_new_turn"):final; };
    run_turn(library_history,Json::object(),"Try opening that source again","test-model",session);
    assert((executed==std::vector<std::string>{"open_library_project:source-a"}));
    // Explicit file loads cannot reopen an uncertain native workflow by
    // refreshing the revision. A genuinely different requested file is usable.
    response_index=0;executed.clear();
    session.respond=[&](const Json& request) {
        const int step=response_index++;
        if(step==0)return call("load_model_file",{{"revision","r1"},{"path","/tmp/a.3mf"},{"mode","project"}},"file_first");
        if(step==1)return call("studio.load_model_file",{{"revision","r2"},{"path","/tmp/a.3mf"},{"mode","project"}},"file_retry");
        if(step==2){const auto skipped=Json::parse(request.at("input").back().at("output").get<std::string>());assert(skipped.at("skipped")==true && skipped.at("outcome")=="unconfirmed");return call("load_model_file",{{"revision","r2"},{"path","/tmp/b.3mf"},{"mode","project"}},"different_file");}
        return final;
    };
    session.tool=[&](const std::string& name,const Json& args){executed.push_back(name+":"+args.at("path").get<std::string>());return unconfirmed;};
    run_turn(Json::array(),Json::object(),"Open the requested files","test-model",session);
    assert((executed==std::vector<std::string>{"load_model_file:/tmp/a.3mf","load_model_file:/tmp/b.3mf"}));
    // Images use multimodal tool results, not a base64 blob in ordinary text.
    response_index=0;
    session.respond=[&](const Json& request) {
        if (response_index++==0) return call("model_view",{{"direction","top"}});
        const auto& content=request.at("input").back().at("output"); assert(content.is_array()); assert(content.at(1).at("type")=="input_image"); return final;
    };
    session.tool=[](const std::string&,const Json&) { return Json{{"image_data","data:image/png;base64,test"},{"plate",1}}; };
    run_turn(Json::array(),Json::object(),"test","test-model",session);
    std::cout << "Studio assistant: callback validation, cryptographic identity validation, fragmented SSE, interrupted responses, and tool contracts passed.\n";
}
