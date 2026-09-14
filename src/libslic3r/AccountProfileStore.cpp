#include "AccountProfileStore.hpp"

#include "nlohmann/json.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace Slic3r {
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

constexpr int REGISTRY_VERSION = 1;

std::int64_t unix_time_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_profile_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

bool is_safe_profile_id(const std::string &id)
{
    static const std::regex safe_id("^[A-Za-z0-9-]{1,64}$");
    return std::regex_match(id, safe_id);
}

std::string normalized_name(std::string value, std::size_t fallback_index)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "Account " + std::to_string(fallback_index);
    value = value.substr(first, last - first + 1);
    if (value.size() > 80)
        value.resize(80);
    return value;
}

bool replace_file_atomically(const fs::path &source, const fs::path &target)
{
#ifdef _WIN32
    const std::wstring source_w = boost::nowide::widen(source.string());
    const std::wstring target_w = boost::nowide::widen(target.string());
    return MoveFileExW(source_w.c_str(), target_w.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(source.c_str(), target.c_str()) == 0;
#endif
}

bool copy_tree(const fs::path &source, const fs::path &target, std::string *error)
{
    try {
        if (!fs::exists(source))
            return true;

        fs::create_directories(target);
        for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
            // Keep framework symlink names intact; relative() resolves them to
            // their targets and would copy aliases over the real directories.
            const fs::path relative = it->path().lexically_relative(source);
            const fs::path dest     = target / relative;
            if (fs::is_directory(it->symlink_status())) {
                fs::create_directories(dest);
            } else if (fs::is_symlink(it->symlink_status())) {
                fs::create_directories(dest.parent_path());
                fs::create_symlink(fs::read_symlink(it->path()), dest);
            } else if (fs::is_regular_file(it->symlink_status())) {
                fs::create_directories(dest.parent_path());
                fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing);
            }
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace

AccountProfileStore::AccountProfileStore(fs::path default_data_dir)
    : m_default_data_dir(std::move(default_data_dir))
{
    m_registry_root = m_default_data_dir.parent_path() /
        (m_default_data_dir.filename().string() + "-Accounts");
    m_profiles_root = m_registry_root / "profiles";
    m_registry_path = m_registry_root / "registry.json";
}

bool AccountProfileStore::initialize(std::string *error)
{
    try {
        fs::create_directories(m_profiles_root);
#ifndef _WIN32
        ::chmod(m_registry_root.c_str(), S_IRWXU);
        ::chmod(m_profiles_root.c_str(), S_IRWXU);
#endif
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }

    const bool ok = fs::exists(m_registry_path) ? load(error) : create_initial_registry(error);
    m_available = ok;
    return ok;
}

bool AccountProfileStore::create_initial_registry(std::string *error)
{
    AccountProfile profile;
    profile.id           = new_profile_id();
    profile.display_name = "Current account";
    profile.last_used    = unix_time_now();
    profile.is_default   = true;

    m_profiles             = { std::move(profile) };
    m_active_profile_id    = m_profiles.front().id;
    m_pending_profile_id.clear();
    m_pending_login_profile_id.clear();
    return save(error);
}

bool AccountProfileStore::load(std::string *error)
{
    try {
        boost::nowide::ifstream stream(m_registry_path.string());
        if (!stream) {
            if (error)
                *error = "Unable to open account registry";
            return false;
        }

        json root;
        stream >> root;
        if (root.value("version", 0) != REGISTRY_VERSION)
            throw std::runtime_error("Unsupported account registry version");

        std::vector<AccountProfile> loaded;
        bool has_default = false;
        for (const json &item : root.at("profiles")) {
            AccountProfile profile;
            profile.id                   = item.at("id").get<std::string>();
            profile.display_name         = item.value("display_name", "Account");
            profile.user_id              = item.value("user_id", "");
            profile.user_name            = item.value("user_name", "");
            profile.avatar_url            = item.value("avatar_url", "");
            profile.last_used             = item.value("last_used", std::int64_t(0));
            profile.is_default            = item.value("is_default", false);
            profile.last_known_logged_in = item.value("last_known_logged_in", false);

            if (!is_safe_profile_id(profile.id))
                throw std::runtime_error("Invalid account profile identifier");
            if (std::any_of(loaded.begin(), loaded.end(), [&profile](const AccountProfile &other) {
                    return other.id == profile.id;
                }))
                throw std::runtime_error("Duplicate account profile identifier");
            if (profile.is_default) {
                if (has_default)
                    throw std::runtime_error("Multiple default account profiles");
                has_default = true;
            }
            loaded.emplace_back(std::move(profile));
        }

        if (loaded.empty() || !has_default)
            throw std::runtime_error("Account registry has no default profile");

        m_profiles                  = std::move(loaded);
        m_active_profile_id         = root.value("active_profile_id", "");
        m_pending_profile_id        = root.value("pending_profile_id", "");
        m_pending_login_profile_id  = root.value("pending_login_profile_id", "");
        if (!find_profile(m_active_profile_id))
            throw std::runtime_error("Account registry has an invalid active profile");
        if (!m_pending_profile_id.empty() && !find_profile(m_pending_profile_id))
            m_pending_profile_id.clear();
        if (!m_pending_login_profile_id.empty() && !find_profile(m_pending_login_profile_id))
            m_pending_login_profile_id.clear();
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

bool AccountProfileStore::save(std::string *error) const
{
    try {
        fs::create_directories(m_profiles_root);
        json root;
        root["version"]                  = REGISTRY_VERSION;
        root["active_profile_id"]        = m_active_profile_id;
        root["pending_profile_id"]       = m_pending_profile_id;
        root["pending_login_profile_id"] = m_pending_login_profile_id;
        root["profiles"]                 = json::array();
        for (const AccountProfile &profile : m_profiles) {
            root["profiles"].push_back({
                {"id", profile.id},
                {"display_name", profile.display_name},
                {"user_id", profile.user_id},
                {"user_name", profile.user_name},
                {"avatar_url", profile.avatar_url},
                {"last_used", profile.last_used},
                {"is_default", profile.is_default},
                {"last_known_logged_in", profile.last_known_logged_in}
            });
        }

        const fs::path temporary = m_registry_path.string() + ".tmp";
        {
            boost::nowide::ofstream stream(temporary.string(), std::ios::out | std::ios::trunc);
            if (!stream)
                throw std::runtime_error("Unable to write account registry");
            stream << root.dump(2) << '\n';
            stream.flush();
            if (!stream)
                throw std::runtime_error("Unable to flush account registry");
        }
#ifndef _WIN32
        ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
        if (!replace_file_atomically(temporary, m_registry_path)) {
            const int saved_errno = errno;
            fs::remove(temporary);
            throw std::runtime_error("Unable to replace account registry: " +
                                     std::string(std::strerror(saved_errno)));
        }
        return true;
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return false;
    }
}

fs::path AccountProfileStore::activate_startup_profile(bool *prompt_login, std::string *error)
{
    if (prompt_login)
        *prompt_login = false;
    if (!m_available)
        return m_default_data_dir;

    if (!m_pending_profile_id.empty()) {
        m_active_profile_id = m_pending_profile_id;
        m_pending_profile_id.clear();
    }

    AccountProfile *active = find_profile(m_active_profile_id);
    if (!active)
        return m_default_data_dir;

    if (prompt_login && m_pending_login_profile_id == active->id)
        *prompt_login = true;
    if (m_pending_login_profile_id == active->id)
        m_pending_login_profile_id.clear();

    active->last_used = unix_time_now();
    if (!save(error))
        return m_default_data_dir;

    const fs::path selected = profile_data_dir(*active);
    try {
        fs::create_directories(selected);
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return m_default_data_dir;
    }
    return selected;
}

const AccountProfile* AccountProfileStore::active_profile() const
{
    return find_profile(m_active_profile_id);
}

fs::path AccountProfileStore::profile_data_dir(const AccountProfile &profile) const
{
    return profile.is_default ? m_default_data_dir : (m_profiles_root / profile.id);
}

std::optional<std::string> AccountProfileStore::add_profile(const std::string &display_name,
                                                             const fs::path &source_data_dir,
                                                             std::string *error)
{
    if (!m_available) {
        if (error)
            *error = "Account profiles are unavailable";
        return std::nullopt;
    }

    AccountProfile profile;
    profile.id           = new_profile_id();
    profile.display_name = normalized_name(display_name, m_profiles.size() + 1);
    profile.last_used    = unix_time_now();
    const fs::path target = profile_data_dir(profile);

    try {
        fs::create_directories(target / "log");
#ifndef _WIN32
        ::chmod(target.c_str(), S_IRWXU);
#endif
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }

    if (!copy_plugins(source_data_dir, target, error)) {
        boost::system::error_code ignored;
        fs::remove_all(target, ignored);
        return std::nullopt;
    }

    m_profiles.emplace_back(std::move(profile));
    if (!save(error)) {
        const std::string failed_id = m_profiles.back().id;
        m_profiles.pop_back();
        boost::system::error_code ignored;
        fs::remove_all(m_profiles_root / failed_id, ignored);
        return std::nullopt;
    }
    return m_profiles.back().id;
}

bool AccountProfileStore::copy_plugins(const fs::path &source_data_dir,
                                       const fs::path &target_data_dir,
                                       std::string *error) const
{
    if (!copy_tree(source_data_dir / "plugins", target_data_dir / "plugins", error))
        return false;
    // Reuse this installation's basic setup, but never the network-engine
    // session, printer access codes, recent projects, or account-specific presets.
    try {
        boost::nowide::ifstream input((source_data_dir / "BambuStudio.conf").string());
        if (!input) return true;
        json source;
        input >> source;
        json target = json::object();
        for (const char* key : {"header", "firstguide", "models", "filaments", "print", "nozzle_volume_types"})
            if (source.contains(key)) target[key] = source.at(key);
        if (source.contains("app") && source.at("app").is_object()) {
            for (const char* key : {"language", "region", "installed_networking", "single_instance",
                                   "dark_color_mode", "sync_user_preset", "sync_system_preset"})
                if (source.at("app").contains(key)) target["app"][key] = source.at("app").at(key);
        }
        boost::nowide::ofstream output((target_data_dir / "BambuStudio.conf").string());
        output << target.dump(2);
        output.close();
        if (!output) throw std::runtime_error("Unable to initialize account preferences");
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool AccountProfileStore::request_switch(const std::string &profile_id, bool prompt_login,
                                         std::string *error)
{
    if (!find_profile(profile_id)) {
        if (error)
            *error = "Unknown account profile";
        return false;
    }
    const std::string previous_pending       = m_pending_profile_id;
    const std::string previous_pending_login = m_pending_login_profile_id;
    m_pending_profile_id       = profile_id;
    m_pending_login_profile_id = prompt_login ? profile_id : "";
    if (save(error))
        return true;
    m_pending_profile_id       = previous_pending;
    m_pending_login_profile_id = previous_pending_login;
    return false;
}

bool AccountProfileStore::cancel_pending_switch(std::string *error)
{
    const std::string previous_pending       = m_pending_profile_id;
    const std::string previous_pending_login = m_pending_login_profile_id;
    m_pending_profile_id.clear();
    m_pending_login_profile_id.clear();
    if (save(error))
        return true;
    m_pending_profile_id       = previous_pending;
    m_pending_login_profile_id = previous_pending_login;
    return false;
}

bool AccountProfileStore::update_active_identity(const std::string &user_id,
                                                 const std::string &user_name,
                                                 const std::string &avatar_url,
                                                 bool logged_in,
                                                 std::string *error)
{
    AccountProfile *profile = find_profile(m_active_profile_id);
    if (!profile)
        return false;
    const std::string next_display_name = user_name.empty() ? profile->display_name : user_name;
    const bool changed = profile->user_id != user_id ||
                         profile->user_name != user_name ||
                         profile->avatar_url != avatar_url ||
                         profile->last_known_logged_in != logged_in ||
                         profile->display_name != next_display_name;
    if (!changed)
        return true;
    profile->user_id              = user_id;
    profile->user_name            = user_name;
    profile->avatar_url           = avatar_url;
    profile->last_known_logged_in = logged_in;
    profile->display_name         = next_display_name;
    return save(error);
}

bool AccountProfileStore::mark_active_signed_out(std::string *error)
{
    AccountProfile *profile = find_profile(m_active_profile_id);
    if (!profile)
        return false;
    if (!profile->last_known_logged_in)
        return true;
    profile->last_known_logged_in = false;
    return save(error);
}

AccountProfile* AccountProfileStore::find_profile(const std::string &profile_id)
{
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(), [&profile_id](const AccountProfile &profile) {
        return profile.id == profile_id;
    });
    return it == m_profiles.end() ? nullptr : &*it;
}

const AccountProfile* AccountProfileStore::find_profile(const std::string &profile_id) const
{
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(), [&profile_id](const AccountProfile &profile) {
        return profile.id == profile_id;
    });
    return it == m_profiles.end() ? nullptr : &*it;
}

} // namespace Slic3r
