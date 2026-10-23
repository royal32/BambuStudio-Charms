#include "StudioAssistantClient.hpp"

#include <boost/asio.hpp>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <boost/nowide/convert.hpp>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Assistant {
namespace {
constexpr const char* issuer = "https://auth.openai.com";
constexpr const char* token_url = "https://auth.openai.com/api/accounts/oauth/token";
constexpr const char* api = "https://api.openai.com/v1";
int64_t epoch() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void check_cancel(const std::atomic<bool>& cancel) { if (cancel) throw std::runtime_error("Stopped. Changes already applied remain available through Undo."); }
std::string field(const Json& value,const std::string& key) {
    const auto found=value.is_object()?value.find(key):value.end();
    return found!=value.end() && found->is_string()?found->get<std::string>():std::string();
}
std::string diagnostic_field(std::string value) {
    // Provider identifiers/parameter names are diagnostic labels, not prose.
    // Keep copied diagnostics bounded, printable, and valid even if a proxy
    // sends malformed bytes or truncation lands inside a Unicode character.
    value.resize(std::min(value.size(),size_t(256)));
    for (auto& ch:value) if (static_cast<unsigned char>(ch)<32 || static_cast<unsigned char>(ch)>126) ch='?';
    return value;
}
const Json& error_fields(const Json& response) {
    if (response.is_object() && response.contains("error") && response.at("error").is_object()) return response.at("error");
    return response;
}
bool sign_in_error(const std::string& code) {
    return code=="invalid_grant" || code=="invalid_refresh_token" || code=="token_expired" || code=="refresh_token_expired" || code=="refresh_token_invalidated" || code=="refresh_token_reused";
}
std::string request_message(long status,const Json& response) {
    const auto code=field(error_fields(response),"code");
    if (code=="subscription_sharing_usage_limit_exceeded" || status==429)
        return "ChatGPT needs a pause. Check ⋯ → Manage usage, then try again later.";
    if (code=="subscription_sharing_user_not_eligible")
        return "ChatGPT plan use isn't available for this account. Check the selected account and its permissions.";
    if (code=="subscription_sharing_unsupported_capability")
        return "ChatGPT can't use part of this request. Try another model. If it happens again, copy the problem details from ⋯ when asking for help.";
    if (code=="subscription_sharing_route_not_supported" || code=="chatpass_v2_scope_not_authorized" || code=="chatpass_v2_invalid_authorization_context")
        return "Studio's ChatGPT connection needs attention. Copy the problem details from ⋯ when asking for help.";
    if (sign_in_error(code))
        return "This ChatGPT sign-in has expired. Choose ⋯ → Sign in to ChatGPT to reconnect.";
    if (status>=500 || code=="subscription_sharing_usage_unavailable" || code=="subscription_sharing_user_unavailable")
        return "ChatGPT is temporarily unavailable. Please try again in a little while.";
    if (status==401)
        return "ChatGPT couldn't verify this connection. Check the selected account in ⋯. If it keeps happening, sign in again.";
    if (status==403)
        return "ChatGPT hasn't allowed this request. Check the account's permissions. If it keeps happening, copy the problem details from ⋯ when asking for help.";
    return "ChatGPT couldn't finish this request. Try again or choose another model. If it keeps happening, copy the problem details from ⋯ when asking for help.";
}
class CredentialLock {
public:
    CredentialLock(const std::string& path,std::atomic<bool>& cancel) {
        if (path.empty()) return;
#ifdef _WIN32
        handle=CreateFileW(boost::nowide::widen(path).c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("The ChatGPT credential lock could not be opened.");
#else
        descriptor=::open(path.c_str(),O_CREAT|O_RDWR,0600);
        if (descriptor<0) throw std::runtime_error("The ChatGPT credential lock could not be opened.");
#endif
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        for (;;) {
#ifdef _WIN32
            if (LockFileEx(handle,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlapped)) return;
#else
            if (::flock(descriptor,LOCK_EX|LOCK_NB)==0) return;
#endif
            if (cancel || std::chrono::steady_clock::now()>=deadline) {
                release(); check_cancel(cancel); throw std::runtime_error("Another Studio window is renewing this ChatGPT connection. Try again shortly.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ~CredentialLock() { release(); }
private:
    void release() {
#ifdef _WIN32
        if (handle!=INVALID_HANDLE_VALUE) { UnlockFileEx(handle,0,1,0,&overlapped); CloseHandle(handle); handle=INVALID_HANDLE_VALUE; }
#else
        if (descriptor>=0) { ::flock(descriptor,LOCK_UN); ::close(descriptor); descriptor=-1; }
#endif
    }
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE}; OVERLAPPED overlapped{};
#else
    int descriptor{-1};
#endif
};
std::string unbase64(std::string value)
{
    if (value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos || value.size() % 4 == 1)
        throw std::runtime_error("Invalid sign-in token encoding.");
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    const size_t padding = (4 - value.size() % 4) % 4;
    value.append(padding, '=');
    std::string result(value.size(), '\0');
    const int count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&result[0]), reinterpret_cast<const unsigned char*>(value.data()), int(value.size()));
    if (count < int(padding)) throw std::runtime_error("Invalid sign-in token encoding.");
    result.resize(count - padding);
    return result;
}
std::string decode_uri(const std::string& input)
{
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%') {
            if (i + 2 >= input.size() || !std::isxdigit(static_cast<unsigned char>(input[i+1])) || !std::isxdigit(static_cast<unsigned char>(input[i+2])))
                throw std::runtime_error("Invalid sign-in callback encoding.");
            char c = static_cast<char>(std::stoi(input.substr(i+1, 2), nullptr, 16));
            if (c == '\0' || c == '\r' || c == '\n') throw std::runtime_error("Invalid sign-in callback.");
            out += c; i += 2;
        } else out += input[i] == '+' ? ' ' : input[i];
    }
    return out;
}
struct Transfer {
    std::string body;
    std::atomic<bool>* cancel;
    ResponseStream* stream;
    std::exception_ptr error;
    size_t received{0};
    long status{0};
    std::string request_id;
};
size_t receive_header(char* data,size_t size,size_t count,void* ptr) {
    auto& transfer=*static_cast<Transfer*>(ptr);
    const auto bytes=size*count;
    try {
        const std::string line(data,bytes);
        if (line.rfind("HTTP/",0)==0) {
            std::istringstream status(line); std::string protocol;
            status>>protocol>>transfer.status; transfer.request_id.clear();
        } else if (const auto colon=line.find(':'); colon!=std::string::npos) {
            auto key=line.substr(0,colon);
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char ch) { return std::tolower(ch); });
            if (key=="x-request-id") {
                const auto begin=line.find_first_not_of(" \t",colon+1),end=line.find_last_not_of(" \t\r\n");
                if (begin!=std::string::npos && end>=begin) transfer.request_id=line.substr(begin,std::min(size_t(256),end-begin+1));
            }
        }
    } catch (...) { transfer.error=std::current_exception(); return 0; }
    return bytes;
}
size_t receive(char* data, size_t size, size_t count, void* ptr)
{
    auto& t = *static_cast<Transfer*>(ptr);
    const size_t bytes = size * count;
    if ((t.received += bytes) > 32 * 1024 * 1024 || *t.cancel) return 0;
    try {
        if (t.status>=400) {
            // Admission errors may be plain JSON rather than SSE. Never feed
            // them to the streaming decoder or discard their structured code.
            t.body.append(data,std::min(bytes,size_t(65536)-t.body.size()));
        } else if (t.stream) t.stream->feed(std::string(data, bytes));
        else t.body.append(data, bytes);
    } catch (...) { t.error = std::current_exception(); return 0; }
    return bytes;
}
int progress(void* ptr, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return *static_cast<std::atomic<bool>*>(ptr) ? 1 : 0; }
Json request(const std::string& url, const std::string& body, const std::string& bearer,
             bool form, std::atomic<bool>& cancel, ResponseStream* stream = nullptr, bool allow_empty = false)
{
    check_cancel(cancel);
    // Dedicated curl handle: no Studio/Bambu headers, cookies, logging, or redirects.
    static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (initialized != CURLE_OK) throw std::runtime_error("Networking could not be initialized.");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Networking could not be initialized.");
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, form ? "Content-Type: application/x-www-form-urlencoded" : "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    if (!bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> guard(headers, curl_slist_free_all);
    Transfer transfer{{}, &cancel, stream, {}, 0};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
#ifdef __APPLE__
    // The bundled OpenSSL curl must use a system CA bundle, not a build-machine path.
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "/etc/ssl/cert.pem");
#endif
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancel);
    if (!body.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
    }
    const auto result = curl_easy_perform(curl.get());
    check_cancel(cancel);
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status >= 400) {
        auto response=Json::parse(transfer.body,nullptr,false);
        if (response.is_discarded()) response=transfer.body;
        throw RequestError(status,std::move(response),transfer.request_id);
    }
    if (transfer.error) {
        try { std::rethrow_exception(transfer.error); }
        catch (const RequestError& error) { throw RequestError(status,error.response(),transfer.request_id); }
    }
    if (result != CURLE_OK) throw RequestError::connection(result);
    if (status < 200 || status >= 300) throw std::runtime_error("OpenAI returned an unexpected response.");
    if (stream) {
        try { return stream->finish(); }
        catch (const RequestError& error) { throw RequestError(status,error.response(),transfer.request_id); }
    }
    if (allow_empty && transfer.body.empty()) return Json::object();
    auto json = Json::parse(transfer.body, nullptr, false);
    if (json.is_discarded()) throw std::runtime_error("OpenAI returned an unreadable response.");
    return json;
}
bool has_scope(const Json& credentials, const std::string& scope)
{
    std::istringstream input(credentials.value("scope", std::string()));
    for (std::string word; input >> word;) if (word == scope) return true;
    return false;
}
}

RequestError::RequestError(long status,Json response,const std::string& request_id)
    : std::runtime_error(request_message(status,response)),m_response(std::move(response))
{
    const auto& error=error_fields(m_response);
    m_diagnostics={{"http_status",status},{"code",diagnostic_field(field(error,"code"))},
        {"param",diagnostic_field(field(error,"param"))},{"request_id",diagnostic_field(request_id)}};
    if (sign_in_error(field(error,"code"))) m_diagnostics["recovery"]="sign_in";
}
RequestError::RequestError(const std::string& message,Json diagnostics)
    : std::runtime_error(message),m_diagnostics(std::move(diagnostics)) {}
RequestError RequestError::connection(int code) {
    std::string message="The ChatGPT connection was interrupted. Please try again.";
    if (code==CURLE_COULDNT_RESOLVE_HOST || code==CURLE_COULDNT_RESOLVE_PROXY || code==CURLE_COULDNT_CONNECT)
        message="I couldn't reach ChatGPT. Check your internet connection, then try again.";
    else if (code==CURLE_OPERATION_TIMEDOUT)
        message="ChatGPT took too long to reply. Please try again when you're ready.";
    else if (code==CURLE_PEER_FAILED_VERIFICATION || code==CURLE_SSL_CONNECT_ERROR)
        message="A secure connection to ChatGPT couldn't be verified. Check this computer's date and internet connection, then try again.";
    return RequestError(message,{{"connection_error",code}});
}
RequestError RequestError::sign_in(const std::string& message) {
    return RequestError(message,{{"recovery","sign_in"}});
}

std::string base64url(const std::string& bytes)
{
    std::string result(4 * ((bytes.size() + 2) / 3), '\0');
    if (!bytes.empty()) EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&result[0]), reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}
std::string random_string(size_t bytes)
{
    std::string value(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value[0]), int(bytes)) != 1) throw std::runtime_error("Secure randomness is unavailable.");
    return base64url(value);
}
std::string sha256(const std::string& bytes)
{
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
    if (!EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr)) throw std::runtime_error("SHA-256 is unavailable.");
    return std::string(reinterpret_cast<char*>(digest), length);
}
std::string form_encode(const Json& values)
{
    auto encode = [](const std::string& input) {
        const char* hex = "0123456789ABCDEF"; std::string out;
        for (unsigned char c : input) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
            else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        }
        return out;
    };
    std::string result;
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!result.empty()) result += '&';
        result += encode(it.key()) + '=' + encode(it.value().get<std::string>());
    }
    return result;
}
Json callback_parameters(const std::string& request, const std::string& host)
{
    const size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos || request.size() > 16384) throw std::runtime_error("Invalid sign-in callback.");
    std::istringstream lines(request.substr(0, end));
    std::string line; std::getline(lines, line);
    std::istringstream first(line); std::string method, target, version, extra;
    first >> method >> target >> version;
    if (method != "GET" || version != "HTTP/1.1" || (first >> extra) || target.rfind("/auth/callback?", 0) != 0)
        throw std::runtime_error("Invalid sign-in callback path.");
    std::string actual_host;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name == "host") {
            if (!actual_host.empty()) throw std::runtime_error("Invalid sign-in callback host.");
            actual_host = line.substr(colon+1); actual_host.erase(0, actual_host.find_first_not_of(" \t"));
        }
    }
    if (actual_host != host) throw std::runtime_error("Invalid sign-in callback host.");
    Json parameters = Json::object();
    std::istringstream query(target.substr(15));
    for (std::string part; std::getline(query, part, '&');) {
        auto equal = part.find('=');
        auto key = decode_uri(part.substr(0, equal));
        if (equal == std::string::npos || parameters.contains(key)) throw std::runtime_error("Invalid sign-in callback parameters.");
        parameters[key] = decode_uri(part.substr(equal+1));
    }
    return parameters;
}

Json verify_id_token(const std::string& token, const Json& jwks, const std::string& client, const std::string& nonce, int64_t now)
{
    auto first = token.find('.'), second = token.find('.', first == std::string::npos ? 0 : first+1);
    if (first == std::string::npos || second == std::string::npos || token.find('.', second+1) != std::string::npos)
        throw std::runtime_error("Invalid sign-in token.");
    auto header = Json::parse(unbase64(token.substr(0, first)));
    auto claims = Json::parse(unbase64(token.substr(first+1, second-first-1)));
    if (header.value("alg", "") != "RS256" || header.value("kid", "").empty()) throw std::runtime_error("Unsupported sign-in token signature.");
    Json key;
    for (const auto& item : jwks.at("keys")) if (item.value("kid", "") == header.at("kid") && item.value("kty", "") == "RSA") key = item;
    if (key.is_null() || (key.contains("alg") && key["alg"] != "RS256") || (key.contains("use") && key["use"] != "sig"))
        throw std::runtime_error("The sign-in signing key was not found.");
    auto modulus = unbase64(key.at("n")), exponent = unbase64(key.at("e"));
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
    BIGNUM* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(modulus.data()), int(modulus.size()), nullptr);
    BIGNUM* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(exponent.data()), int(exponent.size()), nullptr);
    if (!rsa || !n || !e || !RSA_set0_key(rsa.get(), n, e, nullptr)) { BN_free(n); BN_free(e); throw std::runtime_error("Invalid sign-in signing key."); }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(EVP_PKEY_new(), EVP_PKEY_free);
    if (!pkey || !EVP_PKEY_set1_RSA(pkey.get(), rsa.get())) throw std::runtime_error("Invalid sign-in signing key.");
    auto signature = unbase64(token.substr(second+1));
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1 ||
        EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(), reinterpret_cast<const unsigned char*>(token.data()), second) != 1)
        throw std::runtime_error("The sign-in token signature could not be verified.");
    const auto audience = claims.at("aud");
    const bool matches = audience.is_string() ? audience == client : audience.is_array() && std::find(audience.begin(), audience.end(), Json(client)) != audience.end();
    if (claims.value("iss", "") != issuer || !matches || (claims.contains("azp") && claims.at("azp") != client) || (audience.is_array() && audience.size() > 1 && claims.value("azp", "") != client) ||
        claims.value("exp", int64_t(0)) <= now || claims.value("iat", now) > now+60 || claims.value("nbf", int64_t(0)) > now+60 ||
        claims.value("nonce", "") != nonce || claims.value("sub", "").empty())
        throw std::runtime_error("The sign-in token identity, audience, or lifetime could not be verified.");
    return claims;
}

void ResponseStream::event()
{
    if (m_data.empty()) return;
    if (m_data == "[DONE]") { m_data.clear(); return; }
    const auto value = Json::parse(m_data,nullptr,false);
    m_data.clear();
    if (!value.is_object()) throw RequestError(0,Json::object());
    try {
        const auto type = value.value("type", "");
        if (type == "response.output_text.delta" && on_text) { m_text_streamed = true; on_text(value.value("delta", "")); }
        if (type == "response.output_item.done") m_output[value.at("output_index").get<size_t>()] = value.at("item");
        if (type == "response.completed") {
            m_response = value.at("response");
            if (m_response.value("status", "") != "completed") throw std::runtime_error("The assistant response did not complete.");
            m_completed = true;
        }
        if (type == "response.failed" || type == "response.incomplete" || type == "error")
            throw RequestError(0,value.value("response",value));
    } catch (const Json::exception&) { throw RequestError(0,value); }
}
void ResponseStream::feed(const std::string& bytes)
{
    m_buffer += bytes;
    if (m_buffer.size() > 8*1024*1024) throw std::runtime_error("The assistant response was too large.");
    for (size_t end; (end = m_buffer.find('\n')) != std::string::npos;) {
        std::string line = m_buffer.substr(0, end); m_buffer.erase(0, end+1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) event();
        else if (line.rfind("data:", 0) == 0) {
            if (!m_data.empty()) m_data += '\n';
            m_data += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
        }
    }
}
Json ResponseStream::finish()
{
    if (!m_buffer.empty()) feed("\n");
    event();
    if (!m_completed) throw std::runtime_error("The connection ended before the assistant finished. No incomplete action will be executed.");
    // Some streaming gateways omit items from the terminal envelope. Only use
    // completed output items, and only after the whole response completed.
    if ((!m_response.contains("output") || m_response.at("output").empty()) && !m_output.empty()) {
        m_response["output"] = Json::array();
        for (const auto& item : m_output) {
            if (item.first != m_response["output"].size()) throw std::runtime_error("The assistant response was missing a completed output item.");
            m_response["output"].push_back(item.second);
        }
    }
    if (!m_text_streamed && on_text) for (const auto& item : m_response.value("output", Json::array()))
        if (item.value("type", "") == "message") for (const auto& part : item.value("content", Json::array()))
            if (part.value("type", "") == "output_text") on_text(part.value("text", ""));
    return m_response;
}

Client::Client(std::string host, Callbacks callbacks, std::string credential_lock)
    : m_host(std::move(host)),m_credential_lock(std::move(credential_lock)), m_callbacks(std::move(callbacks)) {}
Json Client::sign_in(const Json& registration, std::atomic<bool>& cancel, std::function<void(const std::string&)> open_browser,
                     std::function<void(const Json&)> registration_issued)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::acceptor listener(io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    listener.non_blocking(true);
    const std::string host = "127.0.0.1:" + std::to_string(listener.local_endpoint().port());
    const std::string redirect = "http://" + host + "/auth/callback";
    const auto state = random_string(), nonce = random_string(), verifier = random_string(48);
    const std::string saved_client = registration.value("client_id", "");
    Json params = {{"client_id", saved_client.empty() ? "dynamic_agent_client" : saved_client},
        {"ext_agent_host_id", m_host}, {"response_type", "code"}, {"redirect_uri", redirect},
        {"scope", "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"},
        {"resource", api}, {"state", state}, {"nonce", nonce}, {"code_challenge_method", "S256"}, {"code_challenge", base64url(sha256(verifier))}};
    if (saved_client.empty()) params["agent_name_hint"] = "Charms Studio Assistant";
    else {
        const auto cached=m_credentials.find(saved_client);
        const auto existing = cached==m_credentials.end()?m_callbacks.load_secret(saved_client):cached->second;
        if (existing.contains("id_token")) params["id_token_hint"] = existing.at("id_token");
    }
    open_browser(std::string(issuer) + "/api/accounts/authorize?" + form_encode(params));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    Json callback;
    while (std::chrono::steady_clock::now() < deadline) {
        check_cancel(cancel);
        tcp::socket socket(io); boost::system::error_code ec;
        listener.accept(socket, ec);
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        if (ec) throw std::runtime_error("The local sign-in listener failed.");
        socket.non_blocking(true);
        std::string data; char buffer[2048];
        const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (data.find("\r\n\r\n") == std::string::npos && data.size() < 16384 && std::chrono::steady_clock::now() < read_deadline && !cancel) {
            const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
            if (!ec) data.append(buffer, count);
            else if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) break;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        bool accepted = false;
        try {
            callback = callback_parameters(data, host);
            accepted = callback.value("state", "") == state;
        } catch (...) { }
        const std::string page = accepted ? "Return to Studio to finish connecting your account." : "This sign-in callback was not accepted. Return to the sign-in tab.";
        const std::string response = std::string("HTTP/1.1 ") + (accepted ? "200 OK" : "400 Bad Request") + "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(page.size()) + "\r\n\r\n" + page;
        socket.write_some(boost::asio::buffer(response), ec);
        if (accepted) break;
        callback = Json();
    }
    check_cancel(cancel);
    if (callback.is_null()) throw std::runtime_error("ChatGPT sign-in timed out. Select Continue with ChatGPT to try again.");
    if (callback.contains("error")) throw std::runtime_error("ChatGPT sign-in was declined. You can reconnect and authorize plan usage when ready.");
    const auto client = callback.value("client_id", saved_client);
    if (client.empty() || client == "dynamic_agent_client" || (!saved_client.empty() && client != saved_client) || callback.value("code", "").empty())
        throw std::runtime_error("ChatGPT did not complete this app's registration.");
    // Keep the issued registration even if exchange fails. A subsequent fresh
    // authorization must reuse this client ID instead of registering duplicates.
    // It is not a signed-in identity until the ID token is verified below.
    if (saved_client.empty() && registration_issued) registration_issued({{"client_id", client}, {"subject", ""}, {"email", "Finish connecting ChatGPT"}});
    auto credentials = request(token_url, form_encode({{"grant_type", "authorization_code"}, {"client_id", client}, {"code", callback.at("code")},
        {"code_verifier", verifier}, {"redirect_uri", redirect}, {"resource", api}}), "", true, cancel);
    const auto jwks = request(std::string(issuer) + "/.well-known/jwks.json", "", "", false, cancel);
    const auto claims = verify_id_token(credentials.at("id_token"), jwks, client, nonce, epoch());
    if (!registration.value("subject", "").empty() && registration.at("subject") != claims.at("sub"))
        throw std::runtime_error("That sign-in belongs to another account. Choose Add account instead.");
    if (!has_scope(credentials, "chatgpt.tokens.use.direct") || !has_scope(credentials, "resource.invoke") || credentials.value("access_token", "").empty())
        throw std::runtime_error("Sign-in succeeded, but ChatGPT plan usage was not authorized. Reconnect and enable plan usage for this app.");
    credentials["expires_at"] = epoch() + credentials.value("expires_in", 3600);
    credentials["subject"] = claims.at("sub");
    { CredentialLock guard(m_credential_lock,cancel); m_callbacks.save_secret(client, credentials); }
    m_credentials[client]=credentials;
    return {{"client_id", client}, {"subject", claims.at("sub")}, {"email", claims.value("email", "ChatGPT account")}};
}
Json Client::credentials(const Json& registration, std::atomic<bool>& cancel)
{
    check_cancel(cancel);
    const auto client = registration.value("client_id", "");
    if (client.empty()) throw RequestError::sign_in("Connect your ChatGPT account first.");
    // Keep a valid session in memory across model/tool calls. Re-read the OS
    // store under the cross-process lock only when renewal is actually needed.
    const auto cached=m_credentials.find(client);
    if (cached!=m_credentials.end() && cached->second.value("subject", "")==registration.value("subject", "") &&
        !cached->second.value("access_token", "").empty() && cached->second.value("expires_at",int64_t(0))>epoch()+90)
        return cached->second;
    CredentialLock guard(m_credential_lock,cancel);
    auto saved = m_callbacks.load_secret(client);
    if (saved.value("access_token", "").empty() || saved.value("subject", "") != registration.value("subject", ""))
        throw RequestError::sign_in("Sign in to this ChatGPT account again.");
    if (saved.value("expires_at", int64_t(0)) > epoch()+90) { m_credentials[client]=saved; return saved; }
    if (saved.value("refresh_token", "").empty()) throw RequestError::sign_in("Your ChatGPT session expired. Sign in again.");
    auto renewed = request(token_url, form_encode({{"grant_type", "refresh_token"}, {"client_id", client},
        {"refresh_token", saved.at("refresh_token")}, {"resource", api}}), "", true, cancel);
    if (renewed.value("access_token", "").empty()) throw RequestError::sign_in("ChatGPT did not renew the session. Sign in again.");
    for (auto it = renewed.begin(); it != renewed.end(); ++it) saved[it.key()] = it.value();
    saved["expires_at"] = epoch() + renewed.value("expires_in", 3600);
    if (!has_scope(saved, "chatgpt.tokens.use.direct")) throw RequestError::sign_in("ChatGPT plan usage is no longer authorized. Reconnect your account.");
    m_callbacks.save_secret(client, saved);
    m_credentials[client]=saved;
    return saved;
}
Json Client::models(const Json& registration, std::atomic<bool>& cancel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto saved = credentials(registration, cancel);
    const auto result = request(std::string(api) + "/models", "", saved.at("access_token"), false, cancel);
    Json models = Json::array();
    for (const auto& model : result.at("models")) if (model.value("visibility", "") == "list")
        models.push_back({{"slug", model.at("slug")}, {"name", model.value("display_name", model.at("slug").get<std::string>())}});
    if (models.empty()) throw std::runtime_error("No available models were returned for this account.");
    return models;
}
void Client::authorize_credentials(const Json& registration,std::atomic<bool>& cancel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    check_cancel(cancel);
    if (!m_callbacks.authorize_secret) throw std::runtime_error("Keychain authorization is not needed on this platform.");
    const auto client=registration.value("client_id","");
    CredentialLock guard(m_credential_lock,cancel);
    auto saved=m_callbacks.authorize_secret(client);
    check_cancel(cancel);
    if (client.empty() || saved.value("access_token","").empty() || saved.value("subject","")!=registration.value("subject",""))
        throw RequestError::sign_in("No saved ChatGPT connection was found for this account. Sign in to ChatGPT again.");
    m_credentials[client]=std::move(saved);
}
Json Client::respond(const Json& registration, const Json& input, std::atomic<bool>& cancel, std::function<void(const std::string&)> on_text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto saved = credentials(registration, cancel);
    ResponseStream stream; stream.on_text = std::move(on_text);
    return request(std::string(api) + "/responses", input.dump(), saved.at("access_token"), false, cancel, &stream);
}
bool Client::sign_out(const Json& registration, std::atomic<bool>& cancel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CredentialLock guard(m_credential_lock,cancel);
    const auto client = registration.value("client_id", "");
    const auto cached=m_credentials.find(client);
    const auto saved = cached==m_credentials.end()?m_callbacks.load_secret(client):cached->second;
    bool revoked = saved.value("refresh_token", "").empty();
    if (!revoked) try {
        request(std::string(issuer) + "/api/accounts/oauth/revoke", form_encode({{"token", saved.at("refresh_token")},
            {"token_type_hint", "refresh_token"}, {"client_id", client}}), "", true, cancel, nullptr, true);
        revoked = true;
    } catch (...) { }
    if (!m_callbacks.delete_secret(client) && !saved.value("access_token", "").empty())
        throw std::runtime_error("ChatGPT credentials could not be removed from the OS credential store. Unlock it and sign out again.");
    m_credentials.erase(client);
    return revoked;
}
}}}
