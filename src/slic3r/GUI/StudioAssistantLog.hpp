#ifndef slic3r_StudioAssistantLog_hpp_
#define slic3r_StudioAssistantLog_hpp_

#include "StudioAssistantClient.hpp"
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Assistant {

// Only conversation/session events enter this sink, never OAuth or HTTP traffic.
// Flush each event so a crash or failed turn does not lose the diagnostic trail.
class ConversationLog {
public:
    explicit ConversationLog(const std::filesystem::path& root) noexcept {
        try {
            std::filesystem::create_directories(root);
#ifndef _WIN32
            std::filesystem::permissions(root,std::filesystem::perms::owner_all);
#endif
            std::random_device random;
            std::ostringstream suffix;
            for (int i=0;i<4;++i) suffix<<std::hex<<std::setw(8)<<std::setfill('0')<<random();
            auto stamp=timestamp();
            for (auto& c:stamp) if (c==':') c='-';
            m_directory=root/(stamp+"-"+suffix.str());
            if (!std::filesystem::create_directory(m_directory)) throw std::runtime_error("Log directory exists");
#ifndef _WIN32
            std::filesystem::permissions(m_directory,std::filesystem::perms::owner_all);
#endif
            m_file=open_private(m_directory/"events.jsonl");
            if (!m_file) throw std::runtime_error("Cannot create log");
        } catch (...) { m_problem="Local conversation logging could not create its files. Check the log folder and available disk space."; }
    }
    ~ConversationLog() { if (m_file) std::fclose(m_file); }
    ConversationLog(const ConversationLog&)=delete;
    const std::filesystem::path& directory() const { return m_directory; }
    std::string problem() const { std::lock_guard<std::mutex> lock(m_mutex); return m_problem; }
    void append(const std::string& event,const Json& data=Json::object()) noexcept {
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_file || !m_problem.empty()) return;
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-m_started).count();
            Json record={{"schema_version",1},{"sequence",++m_sequence},{"time_utc",timestamp()},
                {"elapsed_ms",elapsed},{"event",event},{"data",clean(data)}};
            const auto line=record.dump(-1,' ',false,Json::error_handler_t::replace)+"\n";
            if (std::fwrite(line.data(),1,line.size(),m_file)!=line.size() || std::fflush(m_file)!=0)
                throw std::runtime_error("Cannot write log");
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_problem="Local conversation logging stopped because a file could not be written. Check available disk space and folder permissions.";
        }
    }
private:
    static std::FILE* open_private(const std::filesystem::path& path) {
        int descriptor=-1;
#ifdef _WIN32
        if (_wsopen_s(&descriptor,path.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY|_O_NOINHERIT,
                     _SH_DENYWR,_S_IREAD|_S_IWRITE)!=0) return nullptr;
        auto* file=_fdopen(descriptor,"wb");
        if (!file) _close(descriptor);
#else
        descriptor=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if (descriptor<0) return nullptr;
        auto* file=fdopen(descriptor,"wb");
        if (!file) ::close(descriptor);
#endif
        return file;
    }
    static std::string timestamp() {
        const auto now=std::chrono::system_clock::now();
        const auto seconds=std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc,&seconds);
#else
        gmtime_r(&seconds,&utc);
#endif
        const auto milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()%1000;
        std::ostringstream result;
        result<<std::put_time(&utc,"%Y-%m-%dT%H:%M:%S")<<'.'<<std::setw(3)<<std::setfill('0')<<milliseconds<<'Z';
        return result.str();
    }
    static bool secret_key(std::string key) {
        std::string normalized;
        for (unsigned char c:key) if (std::isalnum(c)) normalized+=char(std::tolower(c));
        for (const auto* secret:{"authorization","accesstoken","refreshtoken","idtoken","apikey","clientsecret",
                                "password","cookie","cookies","codeverifier","encryptedcontent"})
            if (normalized==secret) return true;
        return false;
    }
    static std::string scrub(std::string value) {
        // Avoid embedding images or obvious bearer/API credentials even in text.
        for (const auto& prefix:{std::string("data:image/"),std::string("Bearer "),std::string("bearer "),std::string("sk-")}) {
            size_t at=0;
            while ((at=value.find(prefix,at))!=std::string::npos) {
                auto end=value.find_first_of(" \t\r\n\"'<>)}]",at+prefix.size());
                if (end==std::string::npos) end=value.size();
                if (prefix=="sk-" && ((at && std::isalnum(static_cast<unsigned char>(value[at-1]))) || end-at<20)) { at+=3;continue; }
                value.replace(at,end-at,"[omitted]"); at+=9;
            }
        }
        return value;
    }
    Json attachment(const std::string& value) {
        const std::string prefix="data:image/png;base64,";
        if (value.rfind(prefix,0)!=0 || value.size()>16*1024*1024) return {{"omitted","unsupported image payload"}};
        std::string bytes;
        unsigned bits=0;int available=0;bool padded=false;
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i=prefix.size();i<value.size();++i) {
            if (value[i]=='=') { padded=true;continue; }
            const auto digit=alphabet.find(value[i]);
            if (padded || digit==std::string::npos) return {{"omitted","invalid image payload"}};
            bits=(bits<<6)|unsigned(digit);available+=6;
            if (available>=8) { available-=8;bytes+=char((bits>>available)&255); }
        }
        if (bytes.size()<8 || bytes.compare(0,8,"\x89PNG\r\n\x1a\n",8)!=0) return {{"omitted","invalid PNG payload"}};
        const auto name="image-"+std::to_string(++m_images)+".png";
        auto* file=open_private(m_directory/name);
        if (!file) throw std::runtime_error("Cannot create attachment");
        const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
        const auto closed=std::fclose(file);
        if (!written || closed!=0) throw std::runtime_error("Cannot write attachment");
        return {{"attachment",name},{"media_type","image/png"},{"bytes",bytes.size()}};
    }
    Json clean(const Json& value,const std::string& key={}) {
        if (secret_key(key)) return "[redacted]";
        if (value.is_object()) {
            Json result=Json::object();
            for (auto item=value.begin();item!=value.end();++item) result[item.key()]=clean(item.value(),item.key());
            return result;
        }
        if (value.is_array()) {
            Json result=Json::array();for (const auto& item:value) result.push_back(clean(item));return result;
        }
        if (value.is_string()) {
            const auto& text=value.get_ref<const std::string&>();
            if (key=="image_data" || key=="image_url") return attachment(text);
            if (key=="arguments") {
                const auto parsed=Json::parse(text,nullptr,false);
                if (!parsed.is_discarded()) return clean(parsed);
            }
            return scrub(text);
        }
        return value;
    }
    std::filesystem::path m_directory;
    std::FILE* m_file{nullptr};
    mutable std::mutex m_mutex;
    std::string m_problem;
    size_t m_sequence{0},m_images{0};
    std::chrono::steady_clock::time_point m_started{std::chrono::steady_clock::now()};
};
}}}
#endif
