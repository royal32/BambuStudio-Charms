#pragma once

#include <string>

namespace Slic3r { namespace GUI { namespace BambuConnect {

// Encode UTF-8 bytes as URL component data, never as query-string syntax.
inline std::string encode_component(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

// Public protocol: https://wiki.bambulab.com/en/software/bambu-connect
// Import only. Connect owns printer selection, AMS mapping and final submission.
inline std::string import_url(const std::string& absolute_path, const std::string& name,
                              bool double_decode_workaround = false)
{
    auto path_component = encode_component(absolute_path);
    auto name_component = encode_component(name);
    // Connect 2.5 beta's public URL receiver runs decodeURIComponent on the whole
    // query before URLSearchParams decodes its values. Escape once more for that
    // receiver, or '&' splits names/paths and '+' becomes a space. Keep the
    // documented encoding available for versions that fix the receiver.
    if (double_decode_workaround) {
        path_component = encode_component(path_component);
        name_component = encode_component(name_component);
    }
    return "bambu-connect://import-file?path=" + path_component +
           "&name=" + name_component + "&version=1.0.0";
}

}}} // namespace Slic3r::GUI::BambuConnect
