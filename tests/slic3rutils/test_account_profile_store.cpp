#include <catch_main.hpp>

#include "libslic3r/AccountProfileStore.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;

namespace {

struct TemporaryAccountRoot
{
    TemporaryAccountRoot()
        : root(fs::temp_directory_path() / fs::unique_path("bambu-account-profiles-%%%%-%%%%"))
        , default_data_dir(root / "BambuStudio")
    {
        fs::create_directories(default_data_dir / "plugins");
    }

    ~TemporaryAccountRoot()
    {
        boost::system::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path root;
    fs::path default_data_dir;
};

} // namespace

TEST_CASE("Account profiles adopt the existing data directory in place", "[AccountProfiles]")
{
    TemporaryAccountRoot temp;
    Slic3r::AccountProfileStore store(temp.default_data_dir);
    std::string error;

    REQUIRE(store.initialize(&error));
    bool prompt_login = true;
    REQUIRE(store.activate_startup_profile(&prompt_login, &error) == temp.default_data_dir);
    REQUIRE_FALSE(prompt_login);
    REQUIRE(store.profiles().size() == 1);
    REQUIRE(store.active_profile() != nullptr);
    REQUIRE(store.active_profile()->is_default);
}

TEST_CASE("A pending account switch selects an isolated profile and preserves plugins", "[AccountProfiles]")
{
    TemporaryAccountRoot temp;
    {
        boost::nowide::ofstream plugin((temp.default_data_dir / "plugins" / "network-plugin.test").string());
        plugin << "plugin";
    }

    Slic3r::AccountProfileStore store(temp.default_data_dir);
    std::string error;
    REQUIRE(store.initialize(&error));
    auto profile_id = store.add_profile("Work", temp.default_data_dir, &error);
    REQUIRE(profile_id.has_value());
    REQUIRE(store.request_switch(*profile_id, true, &error));

    Slic3r::AccountProfileStore restarted(temp.default_data_dir);
    REQUIRE(restarted.initialize(&error));
    bool prompt_login = false;
    const fs::path selected = restarted.activate_startup_profile(&prompt_login, &error);
    REQUIRE(prompt_login);
    REQUIRE(selected != temp.default_data_dir);
    REQUIRE(fs::exists(selected / "plugins" / "network-plugin.test"));
    REQUIRE(restarted.active_profile() != nullptr);
    REQUIRE(restarted.active_profile()->display_name == "Work");

    REQUIRE(restarted.update_active_identity("user-id", "Bambu User", "https://example.com/avatar.png", true, &error));
    Slic3r::AccountProfileStore reloaded(temp.default_data_dir);
    REQUIRE(reloaded.initialize(&error));
    REQUIRE(reloaded.active_profile()->user_id == "user-id");
    REQUIRE(reloaded.active_profile()->display_name == "Bambu User");
}

TEST_CASE("A canceled account switch does not change the startup profile", "[AccountProfiles]")
{
    TemporaryAccountRoot temp;
    Slic3r::AccountProfileStore store(temp.default_data_dir);
    std::string error;
    REQUIRE(store.initialize(&error));
    auto profile_id = store.add_profile("Canceled", temp.default_data_dir, &error);
    REQUIRE(profile_id.has_value());
    REQUIRE(store.request_switch(*profile_id, true, &error));
    REQUIRE(store.cancel_pending_switch(&error));

    Slic3r::AccountProfileStore restarted(temp.default_data_dir);
    REQUIRE(restarted.initialize(&error));
    bool prompt_login = true;
    REQUIRE(restarted.activate_startup_profile(&prompt_login, &error) == temp.default_data_dir);
    REQUIRE_FALSE(prompt_login);
}

TEST_CASE("New accounts reuse setup without inheriting sessions or printer access", "[AccountProfiles]")
{
    TemporaryAccountRoot temp;
    const nlohmann::json settings = {
        {"firstguide", {{"finish", "1"}}}, {"models", {"A1 mini"}},
        {"app", {{"region", "North America"}, {"language", "en_GB"}, {"secret_api_key", "not-to-copy"}}},
        {"access_code", {{"printer-one", "private"}}}, {"recent_projects", {{"001", "private.3mf"}}}
    };
    { boost::nowide::ofstream out((temp.default_data_dir / "BambuStudio.conf").string()); out << settings; }
    { boost::nowide::ofstream out((temp.default_data_dir / "BambuNetworkEngine.conf").string()); out << "session"; }
    Slic3r::AccountProfileStore store(temp.default_data_dir);
    std::string error;
    REQUIRE(store.initialize(&error));
    auto id = store.add_profile("Second account", temp.default_data_dir, &error);
    REQUIRE(id);
    REQUIRE(store.request_switch(*id, true, &error));
    REQUIRE(store.has_pending_switch());
    const auto folder = store.activate_startup_profile(nullptr, &error);
    REQUIRE_FALSE(store.has_pending_switch());
    boost::nowide::ifstream input((folder / "BambuStudio.conf").string());
    nlohmann::json copied; input >> copied;
    REQUIRE(copied["app"]["region"] == "North America");
    REQUIRE(copied["firstguide"]["finish"] == "1");
    REQUIRE_FALSE(copied.contains("access_code"));
    REQUIRE_FALSE(copied.contains("recent_projects"));
    REQUIRE_FALSE(copied["app"].contains("secret_api_key"));
    REQUIRE_FALSE(fs::exists(folder / "BambuNetworkEngine.conf"));
}

#ifndef _WIN32
TEST_CASE("Account plugin copies preserve macOS framework symlinks", "[AccountProfiles]")
{
    TemporaryAccountRoot temp;
    const auto framework = temp.default_data_dir / "plugins" / "Example.framework";
    fs::create_directories(framework / "Versions/A/Resources");
    { boost::nowide::ofstream out((framework / "Versions/A/Resources/info").string()); out << "framework"; }
    fs::create_symlink("A", framework / "Versions/Current");
    fs::create_symlink("Versions/Current/Resources", framework / "Resources");
    Slic3r::AccountProfileStore store(temp.default_data_dir);
    std::string error;
    REQUIRE(store.initialize(&error));
    const auto id = store.add_profile("Second", temp.default_data_dir, &error);
    INFO(error);
    REQUIRE(id);
    REQUIRE(store.request_switch(*id, true, &error));
    const auto copied = store.activate_startup_profile(nullptr, &error) / "plugins/Example.framework";
    REQUIRE(fs::read_symlink(copied / "Resources") == "Versions/Current/Resources");
    REQUIRE(fs::read_symlink(copied / "Versions/Current") == "A");
    REQUIRE(fs::is_regular_file(copied / "Resources/info"));
}
#endif
