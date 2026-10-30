// Executed only against a disposable keychain and synthetic credentials.
#include "slic3r/GUI/StudioAssistantKeychain.hpp"
#include <iostream>
#include <string>
#include <signal.h>
using namespace Slic3r::GUI::Assistant;
#ifndef KEYCHAIN_TEST_GENERATION
#define KEYCHAIN_TEST_GENERATION 1
#endif
int main(int argc,char** argv) {
    if (argc==2 && std::string(argv[1])=="--internal-charms-keychain") return keychain_child_main();
    if(argc==2 && std::string(argv[1])=="--ipc-cancel-check") {
        Boolean before=false,after=false;if(SecKeychainGetUserInteractionAllowed(&before)!=errSecSuccess)return 68;
        // Cancel before spawning, before writing, and while awaiting a reply.
        // Invalid service names can never reach a real credential or OS prompt.
        for(int threshold:{1,3,4}){int checks=0;bool canceled=false;try{keychain_load("not-an-assistant-service",true,nullptr,[&]{return ++checks>=threshold;});}catch(const std::exception& error){canceled=std::string(error.what())=="Keychain authorization canceled.";}if(!canceled)return 72;}
        if(SecKeychainGetUserInteractionAllowed(&after)!=errSecSuccess || before!=after)return 71;
        return 0;
    }
    if (argc==2 && (std::string(argv[1])=="--ipc-check" || std::string(argv[1])=="--ipc-check-reaped")) {
        if (std::string(argv[1])=="--ipc-check-reaped") signal(SIGCHLD,SIG_IGN);
        Boolean before=false,after=false;
        if (SecKeychainGetUserInteractionAllowed(&before)!=errSecSuccess) return 68;
        try { keychain_load("not-an-assistant-service"); }
        catch (const std::exception& error) {
            if (SecKeychainGetUserInteractionAllowed(&after)!=errSecSuccess || before!=after) return 71;
            if (std::string(error.what())!="Studio's credential service is invalid.") std::cerr<<"IPC probe: "<<error.what()<<'\n';
            return std::string(error.what())=="Studio's credential service is invalid."?0:69;
        }
        return 70;
    }
    // Same isolated-process legacy policy as the production credential child.
    if (SecKeychainSetUserInteractionAllowed(false)!=errSecSuccess) return 68;
    if (argc!=4) return 64;
    const std::string operation=argv[1],path=argv[2],service=argv[3];
    if (service.rfind("charms-keychain-test/",0)!=0 || path.find("keychain-native-checks")==std::string::npos) return 64;
    SecKeychainRef keychain=nullptr;
    constexpr const char* password="synthetic-test-keychain-only";
    auto status=(operation=="create" || operation=="create_legacy")?SecKeychainCreate(path.c_str(),strlen(password),password,false,nullptr,&keychain):SecKeychainOpen(path.c_str(),&keychain);
    if (status!=errSecSuccess) return 65;
    try {
        if (operation=="create") keychain_save(service,{{"access_token","synthetic-test-value"},{"generation",KEYCHAIN_TEST_GENERATION}},keychain);
        else if(operation=="create_legacy") {
            const auto value=Json({{"access_token","synthetic-test-value"},{"generation",0}}).dump();
            status=SecKeychainAddGenericPassword(keychain,UInt32(service.size()),service.c_str(),7,"ChatGPT",UInt32(value.size()),value.data(),nullptr);
        }
        else if(operation=="versions") {
            for(const auto& name:{service,service+"/signed-v2"})if(SecKeychainFindGenericPassword(keychain,UInt32(name.size()),name.c_str(),7,"ChatGPT",nullptr,nullptr,nullptr)!=errSecSuccess)return 72;
        }
        else if(operation=="change_legacy") {
            SecKeychainItemRef item=nullptr;status=SecKeychainFindGenericPassword(keychain,UInt32(service.size()),service.c_str(),7,"ChatGPT",nullptr,nullptr,&item);
            if(status==errSecSuccess){const auto value=Json({{"access_token","stale-legacy-test-value"}}).dump();status=SecKeychainItemModifyAttributesAndData(item,nullptr,UInt32(value.size()),value.data());CFRelease(item);}
        }
        else if (operation=="read") {
            const auto value=keychain_load(service,false,keychain);
            if (value.value("access_token","")!="synthetic-test-value") return 66;
        } else if (operation=="update") keychain_save(service,{{"access_token","synthetic-test-value"},{"generation",KEYCHAIN_TEST_GENERATION}},keychain);
        else if (operation=="lock") status=SecKeychainLock(keychain);
        else if (operation=="unlock") status=SecKeychainUnlock(keychain,strlen(password),password,true);
        else if (operation=="delete") { keychain_delete(service,keychain);for(const auto& name:{service,service+"/signed-v2"})if(SecKeychainFindGenericPassword(keychain,UInt32(name.size()),name.c_str(),7,"ChatGPT",nullptr,nullptr,nullptr)!=errSecItemNotFound)return 73;status=SecKeychainDelete(keychain); }
        else return 64;
        CFRelease(keychain);
        return status==errSecSuccess?0:67;
    } catch (const std::exception& error) {
        CFRelease(keychain);
        // Error text contains no credential values, account names or keychain contents.
        std::cerr<<error.what()<<'\n'; return 2;
    }
}
