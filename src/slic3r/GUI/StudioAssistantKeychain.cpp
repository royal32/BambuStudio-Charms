#include "StudioAssistantKeychain.hpp"
#ifdef __APPLE__
#include <stdexcept>
#include <utility>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <optional>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <mach-o/dyld.h>
extern char** environ;

namespace Slic3r { namespace GUI { namespace Assistant {
namespace {
bool in_credential_child=false;
constexpr size_t message_limit=256*1024;
constexpr const char* child_argument="--internal-charms-keychain";
std::string executable_path();
template<class T> struct Owned {
    T value;
    explicit Owned(T v):value(v) {}
    ~Owned() { if (value) CFRelease(value); }
    Owned(const Owned&)=delete;
    Owned(Owned&& other):value(std::exchange(other.value,nullptr)) {}
    operator T() const { return value; }
};
Owned<CFMutableDictionaryRef> query(const std::string& service, bool interaction, SecKeychainRef keychain, bool adding=false) {
    Owned<CFMutableDictionaryRef> q(CFDictionaryCreateMutable(nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    Owned<CFStringRef> name(CFStringCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(service.data()),service.size(),kCFStringEncodingUTF8,false));
    CFDictionarySetValue(q,kSecClass,kSecClassGenericPassword);
    CFDictionarySetValue(q,kSecAttrService,name);
    CFDictionarySetValue(q,kSecAttrAccount,CFSTR("ChatGPT"));
    // Same file-based items created by wxSecretStore. The legacy shim ignores
    // this flag on some macOS releases. The isolated child ALSO disables legacy
    // interaction process-wide, without changing Studio/network SDK behavior.
    CFDictionarySetValue(q,kSecUseAuthenticationUI,interaction?kSecUseAuthenticationUIAllow:kSecUseAuthenticationUIFail);
    if (keychain) {
        if (adding) CFDictionarySetValue(q,kSecUseKeychain,keychain);
        else {
            const void* refs[]={keychain};
            Owned<CFArrayRef> search(CFArrayCreate(nullptr,refs,1,&kCFTypeArrayCallBacks));
            CFDictionarySetValue(q,kSecMatchSearchList,search);
        }
    }
    return q;
}
void check(OSStatus status) {
    if (status==errSecSuccess) return;
    if (status==errSecInteractionNotAllowed || status==errSecAuthFailed || status==errSecInteractionRequired)
        throw std::runtime_error("ChatGPT needs Keychain authorization. Choose ⋯ → Authorize Keychain access when you are ready. Background requests will not open password prompts.");
    throw std::runtime_error("The ChatGPT connection could not access Keychain ("+std::to_string(status)+"). Your saved sign-in stays protected.");
}
// Check file-Keychain lock state using metadata, without requesting its secret.
// Caller recognition is left to Security.framework's code-requirement checks.
void preflight(CFDictionaryRef original) {
    Owned<CFMutableDictionaryRef> q(CFDictionaryCreateMutableCopy(nullptr,0,original));
    CFDictionarySetValue(q,kSecReturnRef,kCFBooleanTrue);
    CFDictionarySetValue(q,kSecMatchLimit,kSecMatchLimitOne);
    CFTypeRef raw=nullptr; const auto status=SecItemCopyMatching(q,&raw);
    Owned<CFTypeRef> item(raw);
    if (status==errSecItemNotFound) return;
    check(status);
    SecKeychainRef raw_chain=nullptr;
    const auto reference=reinterpret_cast<SecKeychainItemRef>(const_cast<void*>(raw));
    check(SecKeychainItemCopyKeychain(reference,&raw_chain));
    Owned<SecKeychainRef> chain(raw_chain);
    SecKeychainStatus state=0; check(SecKeychainGetStatus(chain,&state));
    if (!(state&kSecUnlockStateStatus)) check(errSecInteractionNotAllowed);
}
bool same_signed_parent() {
    const pid_t pid=getppid();
    Owned<CFNumberRef> number(CFNumberCreate(nullptr,kCFNumberIntType,&pid));
    const void* keys[]={kSecGuestAttributePid}; const void* values[]={number.value};
    Owned<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    SecCodeRef raw_parent=nullptr,raw_self=nullptr;
    if (SecCodeCopyGuestWithAttributes(nullptr,attributes,kSecCSDefaultFlags,&raw_parent)!=errSecSuccess) return false;
    Owned<SecCodeRef> parent(raw_parent);
    if (SecCodeCopySelf(kSecCSDefaultFlags,&raw_self)!=errSecSuccess) return false;
    Owned<SecCodeRef> self(raw_self);
    if (SecCodeCheckValidity(parent,kSecCSDefaultFlags,nullptr)!=errSecSuccess || SecCodeCheckValidity(self,kSecCSDefaultFlags,nullptr)!=errSecSuccess) return false;
    CFDictionaryRef raw_a=nullptr,raw_b=nullptr;
    if (SecCodeCopySigningInformation(parent,kSecCSSigningInformation,&raw_a)!=errSecSuccess) return false;
    Owned<CFDictionaryRef> a(raw_a);
    if (SecCodeCopySigningInformation(self,kSecCSSigningInformation,&raw_b)!=errSecSuccess) return false;
    Owned<CFDictionaryRef> b(raw_b);
    const auto x=CFDictionaryGetValue(a,kSecCodeInfoUnique),y=CFDictionaryGetValue(b,kSecCodeInfoUnique);
    return x && y && CFEqual(x,y); // Exact executing code, not a forgeable path/name.
}
std::string executable_path() {
    uint32_t size=0; _NSGetExecutablePath(nullptr,&size);
    std::string value(size,'\0');
    if (_NSGetExecutablePath(value.data(),&size)!=0) throw std::runtime_error("Studio's credential helper could not be located.");
    value.resize(strlen(value.c_str())); return value;
}
Json credential_process(const Json& input) {
    const auto packet=input.dump()+"\n";
    if (packet.size()>message_limit) throw std::runtime_error("The saved ChatGPT connection is too large.");
    struct Pipe {
        int fd[2]{-1,-1};
        Pipe() { if (pipe(fd)!=0) throw std::runtime_error("Studio's private credential pipe could not be opened."); for (int n:fd) fcntl(n,F_SETFD,FD_CLOEXEC); }
        void close(int i) { if (fd[i]>=0) { ::close(fd[i]); fd[i]=-1; } }
        ~Pipe() { close(0); close(1); }
    } to_child,from_child;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,to_child.fd[0],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,from_child.fd[1],3);
    for (int fd:{to_child.fd[0],to_child.fd[1],from_child.fd[0],from_child.fd[1]})
        if (fd!=STDIN_FILENO && fd!=3) posix_spawn_file_actions_addclose(&actions,fd);
    posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    auto path=executable_path();
    char* argv[]={path.data(),const_cast<char*>(child_argument),nullptr};
    pid_t pid=-1;
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT);
    const int spawned=posix_spawn(&pid,path.c_str(),&actions,&attributes,argv,environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned!=0) throw std::runtime_error("Studio's private credential helper could not start.");
    struct Child {
        pid_t pid;
        ~Child() {
            if (pid<=0) return;
            pid_t state;
            do { state=waitpid(pid,nullptr,WNOHANG); } while (state<0 && errno==EINTR);
            if (state==0) { kill(pid,SIGKILL); while (waitpid(pid,nullptr,0)<0 && errno==EINTR) {} }
        }
    } child{pid};
    to_child.close(0); from_child.close(1);
    fcntl(to_child.fd[1],F_SETNOSIGPIPE,1);
    fcntl(to_child.fd[1],F_SETFL,fcntl(to_child.fd[1],F_GETFL)|O_NONBLOCK);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(input.value("interaction",false)?180:10);
    size_t written=0;
    while (written<packet.size()) {
        if (std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("Studio's credential helper did not accept its request in time.");
        const auto count=write(to_child.fd[1],packet.data()+written,packet.size()-written);
        if (count<0 && errno==EINTR) continue;
        if (count<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) { pollfd writable{to_child.fd[1],POLLOUT,0}; poll(&writable,1,100); continue; }
        if (count<=0) throw std::runtime_error("Studio's private credential helper rejected the request.");
        written+=count;
    }
    to_child.close(1);
    std::string response;
    for (;;) {
        if (std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("Keychain did not respond. No further authorization requests will be opened automatically.");
        pollfd descriptor{from_child.fd[0],POLLIN|POLLHUP,0};
        const auto ready=poll(&descriptor,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0) throw std::runtime_error("Studio's private credential pipe failed.");
        if (!ready) continue;
        char buffer[4096]; const auto count=read(from_child.fd[0],buffer,sizeof(buffer));
        if (count<0 && errno==EINTR) continue;
        if (count<0) throw std::runtime_error("Studio's private credential pipe failed.");
        if (!count) break;
        response.append(buffer,count);
        if (response.size()>message_limit) throw std::runtime_error("The saved ChatGPT connection is too large.");
    }
    int status=0;
    for (;;) {
        const auto waited=waitpid(pid,&status,WNOHANG);
        if (waited==pid) break;
        // A host/framework SIGCHLD handler may already have reaped the child.
        // The authenticated pipe response is still authoritative in that case.
        if (waited<0 && errno==ECHILD) break;
        if (waited<0 && errno!=EINTR) throw std::runtime_error("Studio's credential helper could not be reaped.");
        if (std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("Studio's credential helper did not finish in time.");
        poll(nullptr,0,10);
    }
    child.pid=-1;
    auto result=Json::parse(response,nullptr,false);
    if (!result.is_object()) throw std::runtime_error("Studio's private credential helper did not finish.");
    if (result.contains("error")) throw std::runtime_error(result.at("error").get<std::string>());
    return result.at("result");
}
}
namespace {
std::string stable_service(const std::string& logical){return logical+"/signed-v2";}
std::optional<Json> read_secret(const std::string& service,bool interaction,SecKeychainRef keychain) {
    auto q=query(service,interaction,keychain);
    if (!interaction) preflight(q);
    CFDictionarySetValue(q,kSecReturnData,kCFBooleanTrue);
    CFDictionarySetValue(q,kSecMatchLimit,kSecMatchLimitOne);
    CFTypeRef result=nullptr;
    const auto status=SecItemCopyMatching(q,&result);
    Owned<CFTypeRef> guard(result);
    if (status==errSecItemNotFound) return std::nullopt;
    check(status);
    if (!result || CFGetTypeID(result)!=CFDataGetTypeID()) throw std::runtime_error("The saved ChatGPT connection could not be read.");
    const auto bytes=static_cast<CFDataRef>(result);
    auto value=Json::parse(std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)),CFDataGetLength(bytes)),nullptr,false);
    if (!value.is_object()) throw std::runtime_error("The saved ChatGPT connection could not be read. Sign in again.");
    return value;
}
void add_signed_secret(const std::string& service,const Json& value,SecKeychainRef keychain) {
    const auto text=value.dump();Owned<CFDataRef> bytes(CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(text.data()),text.size()));
    // Updating a legacy record's data does not replace its pre-signing ACL.
    // A fresh item captures this signed code's persistent designated requirement.
    SecTrustedApplicationRef raw_app=nullptr;check(SecTrustedApplicationCreateFromPath(nullptr,&raw_app));Owned<SecTrustedApplicationRef> app(raw_app);
    const void* refs[]={app.value};Owned<CFArrayRef> trusted(CFArrayCreate(nullptr,refs,1,&kCFTypeArrayCallBacks));
    SecAccessRef raw_access=nullptr;check(SecAccessCreate(CFSTR("Charms Studio Assistant"),trusted,&raw_access));Owned<SecAccessRef> access(raw_access);
    auto add=query(service,false,keychain,true);CFDictionarySetValue(add,kSecValueData,bytes);CFDictionarySetValue(add,kSecAttrAccess,access);
    const auto status=SecItemAdd(add,nullptr);
    if(status==errSecDuplicateItem){if(!read_secret(service,false,keychain))throw std::runtime_error("The saved ChatGPT connection changed while upgrading its storage.");}
    else check(status);
}
}
Json keychain_load(const std::string& service,bool interaction,SecKeychainRef keychain) {
    if (!keychain && !in_credential_child) return credential_process({{"operation","load"},{"service",service},{"interaction",interaction}});
    if(auto current=read_secret(stable_service(service),interaction,keychain))return *current;
    auto legacy=read_secret(service,interaction,keychain);if(!legacy)return Json::object();
    // Migrate only after an authorized read. Keep the original encrypted item
    // intact until its replacement is created and read back; never relax an ACL.
    add_signed_secret(stable_service(service),*legacy,keychain);
    const auto migrated=read_secret(stable_service(service),false,keychain);
    if(!migrated)throw std::runtime_error("The ChatGPT connection's signed storage could not be verified. Its original saved sign-in is retained.");
    return *migrated;
}
void keychain_save(const std::string& service,const Json& value,SecKeychainRef keychain) {
    if (!keychain && !in_credential_child) { credential_process({{"operation","save"},{"service",service},{"data",value}}); return; }
    // Require read authorization before changing an existing item, even if its
    // legacy write ACL happens to be less restrictive than the read ACL.
    keychain_load(service,false,keychain);
    const auto text=value.dump();
    Owned<CFDataRef> bytes(CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(text.data()),text.size()));
    auto q=query(stable_service(service),false,keychain);
    preflight(q);
    const void* keys[]={kSecValueData}; const void* values[]={bytes.value};
    Owned<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    auto status=SecItemUpdate(q,attributes);
    if (status==errSecItemNotFound) {
        add_signed_secret(stable_service(service),value,keychain);return;
    }
    check(status);
}
bool keychain_delete(const std::string& service,SecKeychainRef keychain) {
    if (!keychain && !in_credential_child) return credential_process({{"operation","delete"},{"service",service}}).get<bool>();
    keychain_load(service,false,keychain);
    for(const auto& name:{stable_service(service),service}) {
        auto q=query(name,false,keychain);const auto status=SecItemDelete(q);
        if(status!=errSecItemNotFound)check(status);
    }
    return true;
}
int keychain_child_main() {
    // No Studio, network plugin, logging, or GUI is initialized in this process.
    // Only its exact signed parent can use the two inherited anonymous pipes.
    struct stat input{},output{};
    if (fstat(STDIN_FILENO,&input)!=0 || fstat(3,&output)!=0 || !S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode) || !same_signed_parent()) return 77;
    Json response;
    try {
        std::string packet; char ch;
        while (packet.size()<=message_limit && read(STDIN_FILENO,&ch,1)==1 && ch!='\n') packet+=ch;
        if (packet.size()>message_limit) throw std::runtime_error("Studio's credential request is too large.");
        const auto request=Json::parse(packet);
        const auto service=request.at("service").get<std::string>();
        const std::string prefix="Charms Studio Assistant/";
        const auto tail=service.substr(std::min(service.size(),prefix.size()));
        if (service.rfind(prefix,0)!=0 || tail.size()!=87 || tail[43]!='/' || tail.substr(0,43).find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")!=std::string::npos || tail.substr(44).find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")!=std::string::npos)
            throw std::runtime_error("Studio's credential service is invalid.");
        const bool interaction=request.value("interaction",false);
        check(SecKeychainSetUserInteractionAllowed(interaction));
        in_credential_child=true;
        const auto operation=request.at("operation").get<std::string>();
        if (operation=="load") response["result"]=keychain_load(service,interaction);
        else if (operation=="save" && !interaction) { keychain_save(service,request.at("data")); response["result"]=true; }
        else if (operation=="delete" && !interaction) response["result"]=keychain_delete(service);
        else throw std::runtime_error("Studio's credential operation is invalid.");
    } catch (const Json::exception&) { response={{"error","Studio's credential request was invalid."}}; }
    catch (const std::exception& error) { response={{"error",error.what()}}; }
    const auto text=response.dump();
    size_t sent=0;
    while (sent<text.size()) { const auto count=write(3,text.data()+sent,text.size()-sent); if (count<0 && errno==EINTR) continue; if (count<=0) return 1; sent+=count; }
    return 0;
}
}}}
#endif
