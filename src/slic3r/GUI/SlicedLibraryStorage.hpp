#ifndef slic3r_SlicedLibraryStorage_hpp_
#define slic3r_SlicedLibraryStorage_hpp_

// Folder-relative records travel with an iCloud library. Each user action gets
// its own immutable file so two Macs never rewrite a shared notes/exclusion list.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <vector>
#include <cctype>

namespace Slic3r { namespace GUI { namespace LibraryStorage {
namespace fs = std::filesystem;
using Json = nlohmann::json;

inline fs::path child(const fs::path& root, const std::string& relative)
{
    const auto name = fs::u8path(relative);
    if (name.empty() || name.is_absolute()) throw std::runtime_error("Invalid library path.");
    auto result = root;
    for (const auto& component : name) {
        if (component == ".." || component == ".") throw std::runtime_error("Invalid library path.");
        result /= component;
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(result, error)))
            throw std::runtime_error("Library actions cannot follow symbolic links.");
    }
    return result;
}

inline std::string unique_id()
{
    std::random_device random;
    std::ostringstream stream;
    for (int i = 0; i < 4; ++i) stream << std::hex << std::setw(8) << std::setfill('0') << random();
    return stream.str();
}

inline std::string timestamp()
{
    const auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

inline void write(const fs::path& root, const std::string& relative, const std::string& text)
{
    auto destination = child(root, relative);
    fs::create_directories(destination.parent_path());
    destination = child(root, relative);
    const auto temporary = child(root, relative + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(text.data(), text.size());
        stream.close();
    }
    fs::rename(temporary, destination);
}

inline Json read(const fs::path& path)
{
    if (fs::file_size(path) > 65536) throw std::runtime_error("A shared library record is too large.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("A shared library record could not be read.");
    auto record = Json::parse(stream);
    if (!record.is_object() || record.value("version", 0) != 1)
        throw std::runtime_error("Unsupported shared library record. Update Studio and the library tool.");
    return record;
}

inline std::vector<Json> exclusions(const fs::path& root)
{
    std::vector<Json> removed;
    std::set<std::string> restored;
    const auto directory = child(root, ".charm-library/events");
    if (!fs::exists(directory)) return removed;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json") continue;
        const auto path = child(root, item.path().lexically_relative(root).generic_u8string());
        auto record = read(path);
        const auto kind = record.at("kind").get<std::string>();
        if (kind == "exclude") {
            const auto plate = record.at("plate").get<std::string>();
            child(root, plate);
            std::string lower = plate;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.size() <= 10 || lower.substr(lower.size() - 10) != ".gcode.3mf" || !record.at("targets").is_array())
                throw std::runtime_error("Invalid shared plate exclusion.");
            (void) record.at("id").get<std::string>();
            for (const auto& target : record.at("targets")) {
                (void) target.at("project_id").get<std::string>();
                if (target.at("plate_index").get<int>() < 1) throw std::runtime_error("Invalid plate index.");
                (void) target.at("source_sha256").get<std::string>();
            }
            removed.push_back(std::move(record));
        } else if (kind == "restore") {
            for (const auto& id : record.at("restores").get<std::vector<std::string>>()) restored.insert(id);
        } else throw std::runtime_error("Unsupported shared library action.");
    }
    removed.erase(std::remove_if(removed.begin(), removed.end(), [&](const auto& r) {
        return restored.count(r.at("id").template get<std::string>()) != 0;
    }), removed.end());
    return removed;
}

inline std::vector<Json> projects_for(const fs::path& root, const std::string& plate)
{
    std::vector<Json> projects;
    const auto directory = child(root, ".charm-library/projects");
    if (!fs::exists(directory)) return projects;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json") continue;
        auto project = read(child(root, item.path().lexically_relative(root).generic_u8string()));
        for (const auto& p : project.at("plates")) {
            if (p.at("id").get<std::string>() == plate) { projects.push_back(project); break; }
        }
    }
    return projects;
}

inline std::string leave_feedback(const fs::path& root, const std::string& plate,
                                  const std::string& name, const std::string& note)
{
    child(root, plate);
    if (note.empty() || note.size() > 32000) throw std::runtime_error("Enter a note of at most 8,000 characters.");
    const auto projects = projects_for(root, plate);
    const auto date = timestamp();
    std::string filename_date = date;
    std::replace(filename_date.begin(), filename_date.end(), ':', '-');
    const auto relative = "Library Feedback/" + filename_date + "-" + unique_id() + ".md";
    std::string text = "# Library feedback: " + name + "\n\nDate (UTC): " + date + "\n\nPlate: " + plate + "\n";
    for (const auto& project : projects)
        text += "\nProject: " + project.at("name").get<std::string>() + " [" + project.at("id").get<std::string>() + "]\n";
    text += "\n" + note + "\n";
    write(root, relative, text);
    return relative;
}

inline void remove_plate(const fs::path& root, const std::string& plate, const std::string& name,
                         const std::string& expected_stamp)
{
    const auto job = child(root, plate);
    if (!fs::is_regular_file(job)) throw std::runtime_error("This plate is no longer available. Refresh and try again.");
    const auto stamp = std::to_string(fs::file_size(job)) + ":" + std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(fs::last_write_time(job).time_since_epoch()).count());
    if (expected_stamp != stamp && expected_stamp.compare(0, stamp.size() + 8, stamp + ":vector:") != 0)
        throw std::runtime_error("This plate changed since the last refresh. Refresh before deleting it.");
    // Validate every companion before recording the action. The original source
    // project is never moved or edited.
    for (const auto& suffix : {"", ".preview.json", ".preview.svg"}) child(root, plate + suffix);
    const auto id = unique_id();
    const auto removed = ".charm-library/removed/" + id;
    Json targets = Json::array();
    for (const auto& project : projects_for(root, plate)) {
        for (const auto& p : project.at("plates")) if (p.at("id").get<std::string>() == plate)
            targets.push_back({{"project_id", project.at("id")}, {"plate_index", p.at("index")},
                               {"source_sha256", project.at("source_sha256")}});
    }
    const Json record = {{"version", 1}, {"kind", "exclude"}, {"id", id}, {"created_at", timestamp()},
        {"plate", plate}, {"name", name}, {"targets", targets}, {"removed_dir", removed}};
    // Exclude first: a refresh must never recreate a plate whose move failed or
    // whose removal is still syncing from the other Mac.
    write(root, ".charm-library/events/" + id + ".json", record.dump(2) + "\n");
    for (const auto& suffix : {"", ".preview.json", ".preview.svg"}) {
        const auto source = child(root, plate + suffix);
        if (!fs::exists(source)) continue;
        const auto target = child(root, removed + "/" + plate + suffix);
        fs::create_directories(target.parent_path());
        fs::rename(source, target);
    }
}

}}} // namespace Slic3r::GUI::LibraryStorage
#endif
