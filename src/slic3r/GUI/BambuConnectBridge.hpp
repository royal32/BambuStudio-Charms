#pragma once
#include <string>

namespace Slic3r { namespace GUI { namespace BambuConnect {
// Blocking; run on a worker while the UI displays progress. No network listener.
// Returns JSON: status = ready, submitted, attention, or unavailable.
std::string direct_handoff(const std::string& request, const std::string& adapter_path);
// Session contains userId/token/countryCode. Credentials stay in memory and
// Connect's own encrypted store; callers must never log the session JSON.
std::string synchronize_account(const std::string& session, const std::string& adapter_path, bool show = false);

// Read only the active session from the supported arm64 network-plugin build.
// Returns empty for a different binary/layout or a changing account. Never log.
std::string studio_access_token(void* agent, void* identity_function, const std::string& user_id);
}}}
