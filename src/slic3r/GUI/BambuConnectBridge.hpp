#pragma once
#include <string>

namespace Slic3r { namespace GUI { namespace BambuConnect {
// Blocking; run on a worker while the UI displays progress. No network listener.
// Returns JSON: status = ready, submitted, attention, or unavailable.
std::string direct_handoff(const std::string& request, const std::string& adapter_path);
}}}
