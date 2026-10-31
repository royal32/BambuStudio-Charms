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
#include <map>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <vector>
#include <cctype>
#include <cstdio>
#include <memory>
#include <functional>
#ifndef _WIN32
#include <unistd.h>
#include <cerrno>
#endif
#include "PlateJobName.hpp"

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
    if (!fs::is_regular_file(path)) throw std::runtime_error("A shared library record is not a regular file.");
    const auto size = fs::file_size(path);
    if (size > 65536) throw std::runtime_error("A shared library record is too large.");
#ifdef _WIN32
    auto* raw = _wfopen(path.c_str(), L"rb");
#else
    auto* raw = std::fopen(path.c_str(), "rb");
#endif
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(raw, &std::fclose);
    if (!file) throw std::runtime_error("A shared library record could not be read.");
    // Bound metadata reads and parse in memory. The caller's deadline handles
    // File Provider delays without keeping the library interface busy forever.
    std::string bytes(size, '\0');
#ifdef _WIN32
    std::setvbuf(file.get(), nullptr, _IONBF, 0);
    if (size && std::fread(bytes.data(), 1, size, file.get()) != size)
        throw std::runtime_error("A shared library record changed or could not be read.");
#else
    size_t offset = 0;
    while (offset < size) {
        const auto count = ::pread(::fileno(file.get()), bytes.data() + offset, size - offset, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("A shared library record changed or could not be read.");
        offset += size_t(count);
    }
#endif
    auto record = Json::parse(bytes);
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

inline std::string plate_source_key(const std::string& plate)
{
    uint64_t value = 14695981039346656037ull;
    for (unsigned char c : plate) { value ^= c; value *= 1099511628211ull; }
    std::ostringstream text; text << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

inline std::vector<std::string> index_project(const fs::path& root, const Json& project)
{
    std::vector<std::string> entries;
    const auto id = project.at("id").get<std::string>();
    if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
        throw std::runtime_error("Invalid source project ID.");
    for (const auto& plate : project.at("plates")) {
        const auto name = plate.at("id").get<std::string>();
        const auto relative = ".charm-library/plate-sources/" + plate_source_key(name) + "/" + id + ".json";
        write(root, relative,
            Json({{"version", 1}, {"plate", name}, {"project_id", id}}).dump());
        entries.push_back(relative);
    }
    return entries;
}

inline std::vector<Json> projects_for(const fs::path& root, const std::string& plate,
    const std::function<bool()>& cancelled = [] { return false; })
{
    std::vector<Json> projects;
    const bool indexed = fs::exists(child(root, ".charm-library/plate-sources"));
    const auto directory = child(root, indexed ? ".charm-library/plate-sources/" + plate_source_key(plate) : ".charm-library/projects");
    if (!fs::exists(directory)) return projects;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (cancelled()) return {};
        if (item.path().extension() != ".json") continue;
        auto project = read(child(root, item.path().lexically_relative(root).generic_u8string()));
        if (indexed) {
            if (project.at("plate").get<std::string>() != plate) continue; // Routing keys are not identities.
            const auto id = project.at("project_id").get<std::string>();
            if (cancelled()) return {};
            const auto record = child(root, ".charm-library/projects/" + id + ".json");
            if (!fs::exists(record)) continue;
            project = read(record);
            if (project.at("id").get<std::string>() != id) throw std::runtime_error("Invalid source project index.");
        }
        for (const auto& p : project.at("plates")) {
            if (p.at("id").get<std::string>() == plate) { projects.push_back(project); break; }
        }
    }
    return projects;
}

inline bool is_source_project(const fs::path& path)
{
    auto name = path.filename().u8string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.size() > 4 && name.substr(name.size() - 4) == ".3mf" &&
        (name.size() < 10 || name.substr(name.size() - 10) != ".gcode.3mf");
}

// Availability checks can touch a disconnected drive: call this on a worker.
// Source paths may live outside the library. Relative paths travel with it;
// machine-local overrides let another computer locate its own source copy.
inline Json source_projects(const fs::path& root, const std::string& plate, const Json& overrides = Json::object(),
    const std::function<bool()>& cancelled = [] { return false; })
{
    Json sources = Json::array();
    auto projects = projects_for(root, plate, cancelled);
    if (cancelled()) return sources;
    if (projects.empty()) projects.push_back({{"id", "plate:" + plate}, {"name", plate},
        {"plates", Json::array({{{"id", plate}, {"index", 0}}})}});
    for (const auto& project : projects) {
        if (cancelled()) return {};
        const auto id = project.at("id").get<std::string>();
        auto source = project.value("source", std::string());
        const auto registered_source = source;
        if (overrides.contains(id)) {
            const auto& location = overrides.at(id);
            if (location.is_object() && location.value("source", std::string()) == registered_source)
                source = location.value("path", source);
            else if (location.is_string() && !project.value("editable_source", false))
                source = location.get<std::string>();
        }
        fs::path path;
        bool available = false;
        if (!source.empty()) {
            path = fs::u8path(source);
            if (!path.is_absolute()) path = child(root, source);
            std::error_code error;
            available = is_source_project(path) && fs::is_regular_file(path, error);
        }
        for (const auto& p : project.at("plates")) if (p.at("id").get<std::string>() == plate) {
            sources.push_back({{"id", id}, {"name", project.at("name")}, {"path", path.u8string()},
                {"registered_source", registered_source}, {"plate_index", p.at("index")}, {"available", available}});
            break;
        }
    }
    return sources;
}

// Native exports use the same plate identities as deletion and source lookup.
// Keep existing filenames (including aliases); allocate new names without
// taking over another project's file, even on case-insensitive volumes.
inline std::string folded(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

inline std::string short_stem(const std::string& name)
{
    auto stem = plate_filename_component(name);
    while (stem.size() > 210) {
        size_t last = stem.size() - 1;
        while ((static_cast<unsigned char>(stem[last]) & 0xc0) == 0x80) --last;
        stem.resize(last);
    }
    return stem;
}

inline Json project_for_source(const fs::path& root, const fs::path& source, const std::string& preferred = {})
{
    const auto directory = child(root, ".charm-library/projects");
    if (!fs::exists(directory)) {
        if (!preferred.empty()) throw std::runtime_error("The library project registration changed. Refresh and try again.");
        return Json();
    }
    Json match;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json") continue;
        auto project = read(child(root, item.path().lexically_relative(root).generic_u8string()));
        if (!preferred.empty()) {
            if (project.at("id").get<std::string>() == preferred) return project;
            continue;
        }
        auto path = fs::u8path(project.value("source", std::string()));
        if (path.empty()) continue;
        if (!path.is_absolute()) path = child(root, path.generic_u8string());
        if (fs::weakly_canonical(path) == fs::weakly_canonical(source)) {
            if (!match.is_null()) throw std::runtime_error("More than one library project uses this source. Regenerate from its library item instead.");
            match = std::move(project);
        }
    }
    if (!preferred.empty()) throw std::runtime_error("The library project registration changed. Refresh and try again.");
    return match;
}

struct PlateExport { int index; std::string name; std::string relative; };
struct PublicationPlan { Json project; std::vector<PlateExport> files; size_t excluded{0}; };

inline bool plate_excluded(const std::vector<Json>& records, const Json& project, int index, const std::string& filename)
{
    for (const auto& record : records) {
        if (folded(record.at("plate").get<std::string>()) == folded(filename)) return true;
        for (const auto& target : record.at("targets"))
            if (target.at("project_id") == project.at("id") && target.at("plate_index") == index) return true;
    }
    return false;
}

inline PublicationPlan plan_publication(const fs::path& root, Json project, const std::string& source,
                                         const std::string& name, const std::vector<PlateExport>& plates)
{
    if (project.is_null()) project = {{"version", 1}, {"id", "studio-" + unique_id()}, {"name", name}, {"plates", Json::array()}};
    const auto records = exclusions(root);
    const auto id = project.at("id").get<std::string>();
    // IDs become filenames; never accept a path from shared metadata here.
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::runtime_error("Invalid library project identity.");
    std::map<std::string, std::string> owners;
    const auto directory = child(root, ".charm-library/projects");
    if (fs::exists(directory)) for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json") continue;
        const auto other = read(child(root, item.path().lexically_relative(root).generic_u8string()));
        for (const auto& p : other.at("plates")) {
            const auto filename = p.at("id").get<std::string>(); child(root, filename);
            auto& owner = owners[folded(filename)];
            const auto other_id = other.at("id").get<std::string>();
            owner = owner.empty() || owner == other_id ? other_id : "<shared>";
        }
    }
    std::set<std::string> taken;
    for (const auto& item : fs::directory_iterator(root)) taken.insert(folded(item.path().filename().u8string()));
    PublicationPlan plan; plan.project = project;
    for (const auto& plate : plates) {
        if (plate.index < 1) throw std::runtime_error("Invalid library plate index.");
        bool known = false;
        for (const auto& old : project.at("plates")) if (old.at("index") == plate.index) {
            known = true;
            const auto filename = old.at("id").get<std::string>();
            const auto path = child(root, filename);
            if (plate_excluded(records, project, plate.index, filename)) { ++plan.excluded; continue; }
            if (owners[folded(filename)] != id) throw std::runtime_error("This sliced file belongs to more than one project. Give the projects separate library filenames first.");
            if (fs::exists(path) && !fs::is_regular_file(path)) throw std::runtime_error("A sliced file destination is not a regular file.");
            plan.files.push_back({plate.index, plate.name, filename});
        }
        if (known) continue;
        auto stem = plate_filename_component(plate.name);
        if (stem.empty()) stem = "Plate " + std::to_string(plate.index);
        auto filename = stem + ".gcode.3mf";
        if (plate_excluded(records, project, plate.index, filename)) { ++plan.excluded; continue; }
        for (int n = 2; taken.count(folded(filename)) || owners.count(folded(filename)) || fs::exists(child(root, filename)); ++n)
            filename = short_stem(stem) + " (" + std::to_string(n) + ").gcode.3mf";
        child(root, filename);
        taken.insert(folded(filename)); owners[folded(filename)] = id;
        plan.files.push_back({plate.index, plate.name, filename});
        plan.project["plates"].push_back({{"id", filename}, {"index", plate.index}, {"name", plate.name}});
    }
    plan.project["source"] = source;
    plan.project["editable_source"] = true;
    plan.project["native_export"] = true;
    return plan;
}

class PublicationFailure : public std::runtime_error {
public:
    bool keep_backups;
    PublicationFailure(const std::string& message, bool keep) : std::runtime_error(message), keep_backups(keep) {}
};

template<class Rename>
void publish_staged(const fs::path& root, const fs::path& staging,
                    const std::vector<std::pair<fs::path, std::string>>& files, Rename rename)
{
    struct Replacement { fs::path target, backup; bool existed; };
    std::vector<Replacement> replaced;
    try {
        for (const auto& file : files) {
            const auto target = child(root, file.second);
            fs::create_directories(target.parent_path());
            const auto backup = staging / ("backup-" + std::to_string(replaced.size()));
            const bool existed = fs::exists(target);
            if (existed) fs::copy_file(target, backup);
            const auto error = rename(file.first, target);
            if (error) throw std::runtime_error("A library file could not be replaced: " + error.message());
            replaced.push_back({target, backup, existed});
        }
    } catch (...) {
        bool recovered = true;
        for (auto it = replaced.rbegin(); it != replaced.rend(); ++it) {
            if (it->existed) recovered = !rename(it->backup, it->target) && recovered;
            else { std::error_code error; fs::remove(it->target, error); recovered = !error && recovered; }
        }
        if (!recovered) throw PublicationFailure("Some files could not be restored. Recovery copies remain at: " + staging.u8string(), true);
        throw;
    }
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
    if (expected_stamp != stamp)
        throw std::runtime_error("This plate changed since the last refresh. Refresh before deleting it.");
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
    {
        const auto target = child(root, removed + "/" + plate);
        fs::create_directories(target.parent_path());
        fs::rename(job, target);
    }
}

}}} // namespace Slic3r::GUI::LibraryStorage
#endif
