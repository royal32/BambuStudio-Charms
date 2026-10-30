// Opt-in developer check. Reads the existing app registration and uses the
// normal noninteractive Keychain/HTTPS client. Never prints credential data.
#include "slic3r/GUI/StudioAssistantClient.hpp"
#include "slic3r/GUI/StudioAssistantKeychain.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_workbench_eval.hpp"
#include <iostream>
#include <cstring>
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
#endif
using namespace Slic3r;using namespace Slic3r::GUI::Assistant;
int main(int argc,char** argv){
#ifdef __APPLE__
    if(argc==2 && std::strcmp(argv[1],"--internal-charms-keychain")==0)return keychain_child_main();
    try {
        const bool evaluate=(argc==6 || argc==7) && std::strcmp(argv[2],"--evaluate")==0;
        const std::string effort=evaluate&&argc==7?argv[6]:"";
        if(!effort.empty()&&effort!="medium"&&effort!="high"&&effort!="max")throw std::runtime_error("Use an explicit medium, high, or max benchmark effort.");
        if(argc!=2 && !evaluate && !(argc==3 && (std::strcmp(argv[2],"--inspect-keychain-access")==0 || std::strcmp(argv[2],"--inspect-signed-storage")==0)))throw std::runtime_error("Usage: assistant-client-check PROFILE_DIRECTORY [--inspect-keychain-access|--inspect-signed-storage|--evaluate CASE OUTPUT_DIRECTORY MODEL]");
        set_data_dir(argv[1]);AppConfig config;const auto error=config.load();if(!error.empty())throw std::runtime_error("Could not load the selected Studio profile.");
        const auto registrations=Json::parse(config.get("studio_assistant_accounts"));Json account;
        for(const auto& item:registrations)if(item.value("client_id","")==config.get("studio_assistant_active"))account=item;
        if(!account.is_object())throw std::runtime_error("No active ChatGPT registration in this profile.");
        const auto host=config.get("studio_assistant_host");const auto prefix="Charms Studio Assistant/"+base64url(sha256(host))+"/";
        if(argc==3) {
            SecKeychainSetUserInteractionAllowed(false);const auto service=prefix+base64url(sha256(account.at("client_id")))+(std::strcmp(argv[2],"--inspect-signed-storage")==0?"/signed-v2":"");
            auto name=CFStringCreateWithCString(nullptr,service.c_str(),kCFStringEncodingUTF8);
            const void* keys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecReturnRef,kSecUseAuthenticationUI};
            const void* values[]={kSecClassGenericPassword,name,CFSTR("ChatGPT"),kCFBooleanTrue,kSecUseAuthenticationUIFail};
            auto query=CFDictionaryCreate(nullptr,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            CFTypeRef raw=nullptr;const auto status=SecItemCopyMatching(query,&raw);CFRelease(query);CFRelease(name);Json result={{"metadata_status",status}};
            if(status==errSecSuccess && raw){SecKeychainRef chain=nullptr;const auto item=reinterpret_cast<SecKeychainItemRef>(const_cast<void*>(raw));const auto chain_status=SecKeychainItemCopyKeychain(item,&chain);result["owner_status"]=chain_status;
                if(chain_status==errSecSuccess && chain){char path[4096];UInt32 length=sizeof(path);if(SecKeychainGetPath(chain,&length,path)==errSecSuccess)result["keychain_path"]=std::string(path,length);SecKeychainStatus flags=0;result["lock_state_status"]=SecKeychainGetStatus(chain,&flags);result["unlocked"]=bool(flags&kSecUnlockStateStatus);CFRelease(chain);}
                SecAccessRef access=nullptr;result["acl_status"]=SecKeychainItemCopyAccess(item,&access);if(access){CFArrayRef acls=nullptr;if(SecAccessCopyACLList(access,&acls)==errSecSuccess && acls){Json checks=Json::array();for(CFIndex i=0;i<CFArrayGetCount(acls);++i){auto acl=reinterpret_cast<SecACLRef>(const_cast<void*>(CFArrayGetValueAtIndex(acls,i)));CFArrayRef apps=nullptr;CFStringRef description=nullptr;CSSM_ACL_KEYCHAIN_PROMPT_SELECTOR prompt{};if(SecACLCopySimpleContents(acl,&apps,&description,&prompt)==errSecSuccess && apps){for(CFIndex j=0;j<CFArrayGetCount(apps);++j){auto app=reinterpret_cast<SecTrustedApplicationRef>(const_cast<void*>(CFArrayGetValueAtIndex(apps,j)));CFDataRef opaque=nullptr;if(SecTrustedApplicationCopyData(app,&opaque)==errSecSuccess && opaque){const auto* data=CFDataGetBytePtr(opaque);const size_t size=CFDataGetLength(opaque);const auto end=std::find(data,data+size,0);std::string path(reinterpret_cast<const char*>(data),size_t(end-data));checks.push_back({{"data_bytes",size},{"path",path.size()<4096 && !path.empty() && path[0]=='/'?path:"(opaque code identity)"}});CFRelease(opaque);}}CFRelease(apps);}if(description)CFRelease(description);}result["trusted_app_checks"]=checks;CFRelease(acls);}CFRelease(access);}CFRelease(raw);}
            std::cout<<result.dump()<<"\n";return 0;
        }
        // A pre-migration GUI build writes the legacy record, while this build
        // writes signed-v2. Do not rotate a real account's tokens alongside an
        // older running app. Metadata-only diagnostics above are harmless.
        const int count=proc_listallpids(nullptr,0);if(count<=0)throw std::runtime_error("Could not verify that Studio is closed before this developer account check.");
        std::vector<pid_t> pids(size_t(count)+32);const int listed=proc_listallpids(pids.data(),int(pids.size()*sizeof(pid_t)));
        if(listed<=0)throw std::runtime_error("Could not verify Studio's process state; no account check was attempted.");
        const std::string suffix="/Contents/MacOS/BambuStudio";
        for(int i=0;i<std::min<int>(listed,pids.size());++i)if(pids[i]>0 && pids[i]!=getpid()){char path[PROC_PIDPATHINFO_MAXSIZE]{};if(proc_pidpath(pids[i],path,sizeof(path))>0){const std::string executable(path);if(executable.size()>=suffix.size() && executable.compare(executable.size()-suffix.size(),suffix.size(),suffix)==0)throw std::runtime_error("Quit Studio before this standalone developer account check. This avoids mixing old/new credential stores or interfering with its active session.");}}
        Callbacks callbacks;callbacks.load_secret=[prefix](const std::string& id){return keychain_load(prefix+base64url(sha256(id)));};
        callbacks.save_secret=[prefix](const std::string& id,const Json& data){keychain_save(prefix+base64url(sha256(id)),data);};
        callbacks.delete_secret=[](const std::string&){return false;};
        Client client(host,callbacks,std::string(argv[1])+"/assistant/credentials.lock");std::atomic<bool> canceled{false};const auto result=client.models(account,canceled);
        if(evaluate){bool available=false;for(const auto& item:result)available|=item.at("slug")==argv[5];if(!available)throw std::runtime_error("Requested benchmark model is unavailable; no model was substituted.");const auto report=CharmsWorkbenchEval::run(client,account,argv[5],argv[3],std::filesystem::absolute(argv[4]),effort);std::cout<<report.dump()<<"\n";return report.at("passed").get<bool>()?0:3;}
        Json names=Json::array();for(const auto& model:result)names.push_back(model.at("slug"));
        std::cout<<Json({{"connection","passed"},{"models",names}}).dump()<<"\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
#else
    std::cerr<<"This local developer check uses the macOS credential backend.\n";return 2;
#endif
}
