#include "slic3r/GUI/StudioAssistantClient.hpp"
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <cassert>
#include <iostream>

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
    assert(changes_project("set_settings")); assert(!changes_project("inspect_project")); assert(requires_confirmation("document"));
    for (const auto& tool : tools.at(0).at("tools")) assert(tool.at("name") != "send_print");
    auto call=[](const std::string& name,const Json& args,const std::string& call_id="call_1") {
        return Json{{"status","completed"},{"output",Json::array({{{"type","function_call"},{"namespace","studio"},{"name",name},{"call_id",call_id},{"arguments",args.dump()}}})}};
    };
    const Json final={{"status","completed"},{"output",Json::array({{{"type","message"},{"role","assistant"},{"content",Json::array({{{"type","output_text"},{"text","Resliced. The loop still needs inspection."}}})}}})}};
    std::vector<Json> responses={call("inspect_project",Json::object()),call("set_settings",{{"revision","r1"},{"scope","global"},{"settings",Json::array({{{"key","enable_support"},{"value","1"}}})}}),call("slice",Json::object()),final};
    size_t response_index=0; std::vector<std::string> executed;
    SessionCallbacks session;
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

    // A missing revision or an unsupported command never reaches the app.
    for (const auto& invalid : std::vector<Json>{call("set_settings",{{"scope","global"},{"settings",Json::array()}}),call("shell",{{"command","anything"}}),call("copies",{{"revision","r1"},{"object_id","o"},{"additional",999}})}) {
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
    session.respond=[&](const Json&) { cancelled=true; return call("slice",Json::object()); };
    rejects([&] { run_turn(Json::array(),Json::object(),"test","test-model",session); }); assert(!executed_partial);
    session.cancelled=[] { return false; };
    // Native-tool failure is returned as an error, not converted into success or automatically replayed.
    response_index=0; int attempts=0;
    session.respond=[&](const Json& request) {
        if (response_index++==0) return call("arrange",{{"revision","r1"}});
        assert(Json::parse(request.at("input").back().at("output").get<std::string>()).at("error")=="stale project"); return final;
    };
    session.tool=[&](const std::string&,const Json&) -> Json { ++attempts; throw std::runtime_error("stale project"); };
    run_turn(Json::array(),Json::object(),"test","test-model",session); assert(attempts==1);
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
