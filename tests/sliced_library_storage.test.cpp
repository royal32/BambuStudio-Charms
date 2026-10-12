// Standalone filesystem/protocol tests. Never touches the real print library.
#include "slic3r/GUI/SlicedLibraryStorage.hpp"
#include <cassert>
#include <iostream>

using namespace Slic3r::GUI::LibraryStorage;

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / ("sliced-library-test-" + unique_id());
    fs::create_directories(root);
    try {
        const std::string plate = "Mamá Bee SB.gcode.3mf";
        write(root, plate, "test gcode archive");
        write(root, "Bee.gcode.3mf", "sibling");
        Json project = {{"version", 1}, {"id", "bee-project"}, {"name", "Bee"}, {"source_sha256", "source-hash"},
            {"plates", Json::array({{{"id", plate}, {"index", 2}}, {{"id", "Bee.gcode.3mf"}, {"index", 1}}})}};
        write(root, ".charm-library/projects/bee-project.json", project.dump());
        // Older libraries still offer Locate, then remember that computer's copy.
        auto sources = source_projects(root, plate);
        assert(sources.size() == 1 && !sources[0]["available"].get<bool>());
        assert(sources[0]["plate_index"] == 2);
        write(root, "Sources/Mamá Bee.3mf", "editable project");
        project["source"] = "Sources/Mamá Bee.3mf";
        fs::remove(root / ".charm-library/projects/bee-project.json");
        write(root, ".charm-library/projects/bee-project.json", project.dump());
        sources = source_projects(root, plate);
        assert(sources[0]["available"].get<bool>());
        assert(sources[0]["path"] == (root / "Sources/Mamá Bee.3mf").u8string());
        const Json overrides = {{"bee-project", (root / "Bee.gcode.3mf").u8string()}};
        assert(!source_projects(root, plate, overrides)[0]["available"].get<bool>());
        write(root, "Moved Bee.3MF", "moved project");
        const Json moved = {{"bee-project", (root / "Moved Bee.3MF").u8string()}};
        assert(source_projects(root, plate, moved)[0]["available"].get<bool>());
        auto untracked = source_projects(root, "untracked.gcode.3mf");
        assert(untracked[0]["plate_index"] == 0 && !untracked[0]["available"].get<bool>());
        assert(source_projects(root, "untracked.gcode.3mf", {{"plate:untracked.gcode.3mf", (root / "Moved Bee.3MF").u8string()}})[0]["available"].get<bool>());
        project["source"] = "../escape.3mf";
        fs::remove(root / ".charm-library/projects/bee-project.json");
        write(root, ".charm-library/projects/bee-project.json", project.dump());
        bool source_traversal = false;
        try { source_projects(root, plate); } catch (const std::exception&) { source_traversal = true; }
        assert(source_traversal);
        project["source"] = (root / "Sources/Mamá Bee.3mf").u8string();
        fs::remove(root / ".charm-library/projects/bee-project.json");
        write(root, ".charm-library/projects/bee-project.json", project.dump());
        assert(source_projects(root, plate)[0]["available"].get<bool>());
        // A newly published canonical source invalidates old machine-local links.
        project["editable_source"] = true;
        fs::remove(root / ".charm-library/projects/bee-project.json");
        write(root, ".charm-library/projects/bee-project.json", project.dump());
        assert(source_projects(root, plate, moved)[0]["path"] == project["source"]);
        Json obsolete = {{"bee-project", {{"source", "old-source.3mf"}, {"path", (root / "Moved Bee.3MF").u8string()}}}};
        assert(source_projects(root, plate, obsolete)[0]["path"] == project["source"]);
        Json located = {{"bee-project", {{"source", project["source"]}, {"path", (root / "Moved Bee.3MF").u8string()}}}};
        assert(source_projects(root, plate, located)[0]["path"] == (root / "Moved Bee.3MF").u8string());
        const auto note = leave_feedback(root, plate, "Mamá Bee SB", "The hole needs to be larger.\nPlease keep the wings.");
        std::ifstream feedback(child(root, note));
        const std::string text((std::istreambuf_iterator<char>(feedback)), {});
        assert(text.find("The hole needs to be larger.") != std::string::npos);
        assert(text.find("bee-project") != std::string::npos);
        assert(fs::exists(root / plate));
        const auto job = root / plate;
        const auto stamp = std::to_string(fs::file_size(job)) + ":" + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(fs::last_write_time(job).time_since_epoch()).count());
        bool stale = false;
        try { remove_plate(root, plate, "Bee SB", "stale"); } catch (const std::exception&) { stale = true; }
        assert(stale && exclusions(root).empty());
        remove_plate(root, plate, "Bee SB", stamp);
        const auto records = exclusions(root);
        assert(records.size() == 1);
        assert(records[0]["targets"].size() == 1);
        assert(records[0]["targets"][0]["plate_index"] == 2);
        assert(!fs::exists(root / plate));
        assert(fs::exists(root / "Bee.gcode.3mf"));
        assert(fs::exists(root / "Sources/Mamá Bee.3mf"));
        assert(fs::exists(child(root, records[0]["removed_dir"].get<std::string>() + "/" + plate)));
        // Restoration references observed deletion IDs, independent of file dates.
        write(root, ".charm-library/events/restore.json", Json({{"version", 1}, {"kind", "restore"},
            {"id", "restore"}, {"restores", Json::array({records[0]["id"]})}}).dump());
        assert(exclusions(root).empty());
        bool traversal = false;
        try { child(root, "../outside"); } catch (const std::exception&) { traversal = true; }
        assert(traversal);
        fs::create_directory_symlink(fs::temp_directory_path(), root / "linked");
        bool link = false;
        try { leave_feedback(root, "linked/outside.gcode.3mf", "Outside", "note"); } catch (const std::exception&) { link = true; }
        assert(link);
        // Optional handoff verifies that Python understands native-written files.
        if (argc > 1) {
            fs::remove(root / ".charm-library/events/restore.json");
            fs::rename(root, fs::u8path(argv[1]));
        } else fs::remove_all(root);
        std::cout << "Sliced library source links, deletion, feedback and shared records passed.\n";
    } catch (...) { fs::remove_all(root); throw; }
}
