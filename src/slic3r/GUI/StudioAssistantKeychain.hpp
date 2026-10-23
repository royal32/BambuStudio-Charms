#ifndef slic3r_StudioAssistantKeychain_hpp_
#define slic3r_StudioAssistantKeychain_hpp_
#ifdef __APPLE__
#include "StudioAssistantClient.hpp"
#include <Security/Security.h>

namespace Slic3r { namespace GUI { namespace Assistant {
// Normal requests must never ask the OS to display authorization UI. Only the
// explicit Authorize Keychain action may call load with interaction=true.
Json keychain_load(const std::string& service, bool interaction = false, SecKeychainRef keychain = nullptr);
void keychain_save(const std::string& service, const Json& data, SecKeychainRef keychain = nullptr);
bool keychain_delete(const std::string& service, SecKeychainRef keychain = nullptr);
// Private same-executable subprocess, entered before Studio/SDK initialization.
int keychain_child_main();
}}}
#endif
#endif
