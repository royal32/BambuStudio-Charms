#ifndef slic3r_AccountProfileStore_hpp_
#define slic3r_AccountProfileStore_hpp_

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

struct AccountProfile
{
    std::string id;
    std::string display_name;
    std::string user_id;
    std::string user_name;
    std::string avatar_url;
    std::int64_t last_used { 0 };
    bool is_default { false };
    bool last_known_logged_in { false };
};

// Stores only account/profile metadata. Authentication remains inside each
// complete Bambu Studio data directory and is owned by BambuNetworkAgent.
class AccountProfileStore
{
public:
    explicit AccountProfileStore(boost::filesystem::path default_data_dir);

    bool initialize(std::string *error = nullptr);
    bool available() const { return m_available; }

    // Promotes a pending switch, returns the selected data directory and reports
    // whether the new profile should open the normal sign-in dialog.
    boost::filesystem::path activate_startup_profile(bool *prompt_login = nullptr,
                                                     std::string *error = nullptr);

    const std::vector<AccountProfile>& profiles() const { return m_profiles; }
    const AccountProfile* active_profile() const;
    boost::filesystem::path profile_data_dir(const AccountProfile &profile) const;

    std::optional<std::string> add_profile(const std::string &display_name,
                                           const boost::filesystem::path &source_data_dir,
                                           std::string *error = nullptr);
    bool request_switch(const std::string &profile_id, bool prompt_login,
                        std::string *error = nullptr);
    bool cancel_pending_switch(std::string *error = nullptr);

    bool update_active_identity(const std::string &user_id,
                                const std::string &user_name,
                                const std::string &avatar_url,
                                bool logged_in,
                                std::string *error = nullptr);
    bool mark_active_signed_out(std::string *error = nullptr);

    const boost::filesystem::path& registry_root() const { return m_registry_root; }

private:
    AccountProfile* find_profile(const std::string &profile_id);
    const AccountProfile* find_profile(const std::string &profile_id) const;
    bool load(std::string *error);
    bool save(std::string *error = nullptr) const;
    bool create_initial_registry(std::string *error);
    bool copy_plugins(const boost::filesystem::path &source_data_dir,
                      const boost::filesystem::path &target_data_dir,
                      std::string *error) const;

    boost::filesystem::path m_default_data_dir;
    boost::filesystem::path m_registry_root;
    boost::filesystem::path m_profiles_root;
    boost::filesystem::path m_registry_path;
    std::vector<AccountProfile> m_profiles;
    std::string m_active_profile_id;
    std::string m_pending_profile_id;
    std::string m_pending_login_profile_id;
    bool m_available { false };
};

} // namespace Slic3r

#endif
