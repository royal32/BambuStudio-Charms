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
        // Native publication is repeatable and never steals another project's
        // filename. Existing aliases and per-plate exclusions keep their IDs.
        const auto native_root = root / "native";
        fs::create_directory(native_root);
        write(native_root, "Bee.gcode.3mf", "unowned job");
        auto initial = plan_publication(native_root, Json(), "Projects/Bee.3mf", "Bee",
            {{1, "Bee", {}}, {2, "Bee", {}}});
        assert(initial.files.size() == 2);
        assert(initial.files[0].relative == "Bee (2).gcode.3mf");
        assert(initial.files[1].relative == "Bee (3).gcode.3mf");
        initial.project["source_sha256"] = "native-hash";
        const auto native_id = initial.project.at("id").get<std::string>();
        initial.project["plates"].push_back({{"id", "Alias.gcode.3mf"}, {"index", 1}});
        write(native_root, ".charm-library/projects/" + native_id + ".json", initial.project.dump());
        write(native_root, "Projects/Bee.3mf", "editable source");
        assert(project_for_source(native_root, native_root / "Projects/Bee.3mf")["id"] == native_id);
        assert(project_for_source(native_root, native_root / "moved.3mf", native_id)["id"] == native_id);
        bool missing_registration = false;
        try { project_for_source(native_root, native_root / "Projects/Bee.3mf", "removed-project-id"); }
        catch (const std::exception&) { missing_registration = true; }
        assert(missing_registration);
        auto repeated = plan_publication(native_root, initial.project, "Projects/Bee.3mf", "Bee",
            {{1, "Renamed bee", {}}, {2, "Bee", {}}});
        assert(repeated.project["id"] == native_id && repeated.files.size() == 3);
        assert(repeated.files[0].relative == initial.files[0].relative);
        assert(repeated.files[1].relative == "Alias.gcode.3mf");
        assert(repeated.project["plates"].size() == 3);
        write(native_root, ".charm-library/events/exclude-native.json", Json({{"version", 1}, {"kind", "exclude"},
            {"id", "exclude-native"}, {"plate", initial.files[1].relative},
            {"targets", Json::array({{{"project_id", native_id}, {"plate_index", 2}, {"source_sha256", "native-hash"}}})}}).dump());
        auto excluded = plan_publication(native_root, initial.project, "Projects/Bee.3mf", "Bee",
            {{1, "Bee", {}}, {2, "Changed name", {}}});
        assert(excluded.files.size() == 2 && excluded.excluded == 1);
        // Selecting one plate retains the other plate's source registration.
        auto single = plan_publication(native_root, initial.project, "Projects/Bee.3mf", "Bee", {{1, "Bee", {}}});
        assert(single.files.size() == 2 && single.project["plates"].size() == 3);
        auto other = initial.project; other["id"] = "other-owner";
        write(native_root, ".charm-library/projects/other-owner.json", other.dump());
        bool shared_conflict = false;
        try { plan_publication(native_root, initial.project, "Projects/Bee.3mf", "Bee", {{1, "Bee", {}}}); }
        catch (const std::exception&) { shared_conflict = true; }
        assert(shared_conflict);
        const auto staging = native_root / ".charm-library/staging/test";
        fs::create_directories(staging);
        write(native_root, "existing.gcode.3mf", "previous slice");
        write(native_root, "project.json", "previous registration");
        write(native_root, ".charm-library/staging/test/slice", "new slice");
        write(native_root, ".charm-library/staging/test/new", "new file");
        write(native_root, ".charm-library/staging/test/record", "new registration");
        int replacements = 0;
        bool rolled_back = false;
        try {
            publish_staged(native_root, staging, {{staging / "slice", "existing.gcode.3mf"},
                {staging / "new", "new.gcode.3mf"}, {staging / "record", "project.json"}},
                [&](const fs::path& from, const fs::path& to) {
                    if (++replacements == 3) return std::make_error_code(std::errc::no_space_on_device);
                    std::error_code error; fs::rename(from, to, error); return error;
                });
        } catch (const std::exception&) { rolled_back = true; }
        assert(rolled_back && !fs::exists(native_root / "new.gcode.3mf"));
        auto contents = [](const fs::path& path) { std::ifstream in(path); return std::string(std::istreambuf_iterator<char>(in), {}); };
        assert(contents(native_root / "existing.gcode.3mf") == "previous slice");
        assert(contents(native_root / "project.json") == "previous registration");
        // The caller owns staging cleanup; failure never deletes recovery data.
        assert(contents(staging / "record") == "new registration");
        const auto successful = native_root / ".charm-library/staging/success";
        fs::create_directories(successful);
        write(native_root, ".charm-library/staging/success/slice", "replacement slice");
        write(native_root, ".charm-library/staging/success/record", "replacement registration");
        publish_staged(native_root, successful, {{successful / "slice", "existing.gcode.3mf"},
            {successful / "record", "project.json"}}, [](const fs::path& from, const fs::path& to) {
                std::error_code error; fs::rename(from, to, error); return error;
            });
        assert(contents(native_root / "existing.gcode.3mf") == "replacement slice");
        assert(contents(native_root / "project.json") == "replacement registration");
        assert(contents(successful / "backup-0") == "previous slice");
        // Collision suffixes never split a UTF-8 codepoint in a long name.
        std::string accented;
        for (int i = 0; i < 120; ++i) accented += "é";
        const auto unicode_plan = plan_publication(native_root, Json(), "Projects/Unicode.3mf", "Unicode",
            {{1, accented, {}}, {2, accented, {}}});
        assert(unicode_plan.files.size() == 2 && unicode_plan.files[1].relative.size() < 250);
        assert(!Json(unicode_plan.files[1].relative).dump().empty());
        fs::remove_all(native_root);
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
