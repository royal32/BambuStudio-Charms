#pragma once

#include <algorithm>
#include <string>

namespace Slic3r { namespace GUI {

// A portable filename component, leaving room for .gcode.3mf without splitting
// a UTF-8 character. Keep normal names intact (including spaces and accents).
inline std::string plate_filename_component(std::string name)
{
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch) {
        return ch < 32 || std::string("<>[]:/\\|?*\"").find(ch) != std::string::npos;
    }), name.end());
    if (name.size() > 240) {
        size_t end = 240;
        while ((static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name.resize(end);
    }
    const auto start = name.find_first_not_of(" ");
    if (start == std::string::npos) return {};
    name.erase(0, start);
    const auto end = name.find_last_not_of(" .");
    if (end == std::string::npos) return {};
    name.resize(end + 1);
    return name;
}

inline std::string plate_job_name(std::string project, const std::string& plate,
                                 int plate_index, int plate_count, bool whole_project = false)
{
    const std::string custom = plate_filename_component(plate);
    if (!custom.empty() && (!whole_project || plate_count == 1)) return custom;
    project = plate_filename_component(project);
    if (project.empty()) project = "Untitled";
    if (!whole_project && plate_count > 1) {
        const std::string number = std::to_string(plate_index + 1);
        const std::string suffix = " " + (number.size() < 2 ? "0" + number : number);
        // Reserve the suffix even for a very long project name.
        while (project.size() + suffix.size() > 240) {
            size_t last = project.size() - 1;
            while ((static_cast<unsigned char>(project[last]) & 0xc0) == 0x80) --last;
            project.resize(last);
        }
        project += suffix;
    }
    return project;
}

}} // namespace Slic3r::GUI
