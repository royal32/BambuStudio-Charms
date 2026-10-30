#ifndef slic3r_StudioAssistantClient_hpp_
#define slic3r_StudioAssistantClient_hpp_

#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <stdexcept>

namespace Slic3r { namespace GUI { namespace Assistant {
using Json = nlohmann::json;

// The chat gets a short recovery message. Safe diagnostics can be copied
// separately; raw provider bodies are retained in memory, never shown/logged.
class RequestError : public std::runtime_error {
public:
    RequestError(long status, Json response, const std::string& request_id = {});
    static RequestError connection(int code);
    static RequestError sign_in(const std::string& message);
    const Json& diagnostics() const { return m_diagnostics; }
    const Json& response() const { return m_response; }
private:
    RequestError(const std::string& message, Json diagnostics);
    Json m_diagnostics, m_response;
};

// No window pointers or application globals cross the network worker boundary.
struct Callbacks {
    std::function<Json(const std::string&)> load_secret;
    std::function<void(const std::string&, const Json&)> save_secret;
    std::function<bool(const std::string&)> delete_secret;
    // Optional, explicit user action. Automatic loads/saves must not show OS UI.
    std::function<Json(const std::string&)> authorize_secret;
};

std::string random_string(size_t bytes = 32);
std::string base64url(const std::string& bytes);
std::string sha256(const std::string& bytes);
std::string form_encode(const Json& values);
Json callback_parameters(const std::string& request, const std::string& host);
Json verify_id_token(const std::string& token, const Json& jwks, const std::string& client,
                     const std::string& nonce, int64_t now);

// SSE framing is independent of curl's arbitrary byte/chunk boundaries.
class ResponseStream {
public:
    void feed(const std::string& bytes);
    Json finish();
    std::function<void(const std::string&)> on_text;
private:
    void event();
    std::string m_buffer, m_data;
    Json m_response;
    std::map<size_t, Json> m_output;
    bool m_text_streamed{false};
    bool m_completed{false};
};

class Client {
public:
    Client(std::string host, Callbacks callbacks, std::string credential_lock = {});
    Json sign_in(const Json& registration, std::atomic<bool>& cancel, std::function<void(const std::string&)> open_browser,
                 std::function<void(const Json&)> registration_issued = {});
    Json models(const Json& registration, std::atomic<bool>& cancel);
    void authorize_credentials(const Json& registration, std::atomic<bool>& cancel);
    Json respond(const Json& registration, const Json& request, std::atomic<bool>& cancel,
                 std::function<void(const std::string&)> on_text);
    // Returns whether remote revocation was confirmed. Local secrets are always removed.
    bool sign_out(const Json& registration, std::atomic<bool>& cancel);
private:
    friend struct ClientTestAccess;
    Json credentials(const Json& registration, std::atomic<bool>& cancel);
    std::string m_host;
    std::string m_credential_lock;
    Callbacks m_callbacks;
    std::map<std::string, Json> m_credentials;
    std::mutex m_mutex;
};

Json tool_definitions();
Json workbench_tool_definitions();
bool workbench_changes_project(const std::string& name);
bool workbench_requires_confirmation(const std::string& name);
std::string instructions();
bool changes_project(const std::string& tool);
bool requires_confirmation(const std::string& tool);
bool requires_confirmation(const std::string& tool, const Json& arguments);
struct SessionCallbacks {
    std::function<Json(const Json&)> respond;
    std::function<Json(const std::string&, const Json&)> tool;
    std::function<bool()> cancelled;
    // Optional local diagnostics; failures must never alter task execution.
    std::function<void(const std::string&,const Json&)> trace;
};
Json run_turn(Json history, const Json& context, const std::string& message, const std::string& model, const SessionCallbacks& callbacks);

}}}
#endif
