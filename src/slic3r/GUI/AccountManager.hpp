#ifndef slic3r_AccountManager_hpp_
#define slic3r_AccountManager_hpp_

#include <string>
#include <vector>
#include <memory>
#include <wx/string.h>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

// Represents a single Bambu account
struct BambuAccount {
    std::string user_id;
    std::string user_name;
    std::string nickname;
    std::string avatar_url;
    std::string auth_token;  // Store auth token for switching
    bool is_active;          // Whether this is the currently active account
    
    BambuAccount() : is_active(false) {}
    
    json to_json() const {
        json j;
        j["user_id"] = user_id;
        j["user_name"] = user_name;
        j["nickname"] = nickname;
        j["avatar_url"] = avatar_url;
        j["auth_token"] = auth_token;
        j["is_active"] = is_active;
        return j;
    }
    
    static BambuAccount from_json(const json& j) {
        BambuAccount account;
        if (j.contains("user_id")) account.user_id = j["user_id"].get<std::string>();
        if (j.contains("user_name")) account.user_name = j["user_name"].get<std::string>();
        if (j.contains("nickname")) account.nickname = j["nickname"].get<std::string>();
        if (j.contains("avatar_url")) account.avatar_url = j["avatar_url"].get<std::string>();
        if (j.contains("auth_token")) account.auth_token = j["auth_token"].get<std::string>();
        if (j.contains("is_active")) account.is_active = j["is_active"].get<bool>();
        return account;
    }
};

// Manages multiple Bambu accounts
class AccountManager {
public:
    AccountManager();
    ~AccountManager();
    
    // Add a new account
    void add_account(const BambuAccount& account);
    
    // Remove an account by user_id
    bool remove_account(const std::string& user_id);
    
    // Switch to a different account
    bool switch_account(const std::string& user_id);
    
    // Get the currently active account
    BambuAccount* get_active_account();
    
    // Get all accounts
    const std::vector<BambuAccount>& get_all_accounts() const { return m_accounts; }
    
    // Check if a user is already added
    bool has_account(const std::string& user_id) const;
    
    // Save accounts to config
    void save_accounts();
    
    // Load accounts from config
    void load_accounts();
    
    // Get account by user_id
    BambuAccount* get_account(const std::string& user_id);
    
private:
    std::vector<BambuAccount> m_accounts;
    std::string m_config_file_path;
    
    void update_active_account(const std::string& user_id);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_AccountManager_hpp_
