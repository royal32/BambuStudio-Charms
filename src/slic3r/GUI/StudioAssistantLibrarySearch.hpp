#ifndef slic3r_StudioAssistantLibrarySearch_hpp_
#define slic3r_StudioAssistantLibrarySearch_hpp_

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Assistant {
// Filename-only search: retain a bounded, deterministic prefix while counting
// all matches. No archive contents or filesystem access are needed here.
class LibrarySearch {
public:
    explicit LibrarySearch(const std::string& query) : m_query(fold(query)) {}
    void consider(const std::string& filename) {
        const auto folded=fold(filename);
        if (folded.size()<10 || folded.compare(folded.size()-10,10,".gcode.3mf")!=0 || folded.find(m_query)==std::string::npos) return;
        ++m_total_matches;
        m_matches.emplace(folded.substr(0,folded.size()-10),filename);
        if (m_matches.size()>40) m_matches.erase(std::prev(m_matches.end()));
    }
    std::vector<std::string> filenames() const {
        std::vector<std::string> result;
        for (const auto& match:m_matches) result.push_back(match.second);
        return result;
    }
    size_t total_matches() const { return m_total_matches; }
    bool truncated() const { return m_total_matches>m_matches.size(); }
private:
    static std::string fold(std::string value) {
        std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }
    std::string m_query;
    std::set<std::pair<std::string,std::string>> m_matches;
    size_t m_total_matches{0};
};
}}}
#endif
