#include "AccountManager.hpp"
#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <fstream>

namespace Slic3r {
namespace GUI {

AccountManager::AccountManager()
{
    // Initialize config file path in user data directory
    boost::filesystem::path data_dir = boost::filesystem::path(data_dir());
    m_config_file_path = (data_dir / "bambu_accounts.json").string();
}

AccountManager::~AccountManager()
{
}

void AccountManager::add_account(const BambuAccount& account)
{
    // Check if account already exists
    for (auto& existing_account : m_accounts) {
        if (existing_account.user_id == account.user_id) {
            // Update existing account
            existing_account = account;
            save_accounts();
            return;
        }
    }
    
    // Add new account
    BambuAccount new_account = account;
    
    // If this is the first account, make it active
    if (m_accounts.empty()) {
        new_account.is_active = true;
    } else {
        new_account.is_active = false;
    }
    
    m_accounts.push_back(new_account);
    save_accounts();
}

bool AccountManager::remove_account(const std::string& user_id)
{
    auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
        [&user_id](const BambuAccount& acc) { return acc.user_id == user_id; });
    
    if (it != m_accounts.end()) {
        bool was_active = it->is_active;
        m_accounts.erase(it);
        
        // If we removed the active account, activate the first remaining account
        if (was_active && !m_accounts.empty()) {
            m_accounts[0].is_active = true;
        }
        
        save_accounts();
        return true;
    }
    
    return false;
}

bool AccountManager::switch_account(const std::string& user_id)
{
    bool found = false;
    
    // Deactivate all accounts and activate the selected one
    for (auto& account : m_accounts) {
        if (account.user_id == user_id) {
            account.is_active = true;
            found = true;
        } else {
            account.is_active = false;
        }
    }
    
    if (found) {
        save_accounts();
    }
    
    return found;
}

BambuAccount* AccountManager::get_active_account()
{
    for (auto& account : m_accounts) {
        if (account.is_active) {
            return &account;
        }
    }
    return nullptr;
}

bool AccountManager::has_account(const std::string& user_id) const
{
    return std::find_if(m_accounts.begin(), m_accounts.end(),
        [&user_id](const BambuAccount& acc) { return acc.user_id == user_id; }) != m_accounts.end();
}

BambuAccount* AccountManager::get_account(const std::string& user_id)
{
    auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
        [&user_id](const BambuAccount& acc) { return acc.user_id == user_id; });
    
    if (it != m_accounts.end()) {
        return &(*it);
    }
    return nullptr;
}

void AccountManager::save_accounts()
{
    try {
        json j = json::array();
        for (const auto& account : m_accounts) {
            j.push_back(account.to_json());
        }
        
        std::ofstream file(m_config_file_path);
        if (file.is_open()) {
            file << j.dump(4);
            file.close();
            BOOST_LOG_TRIVIAL(info) << "Saved " << m_accounts.size() << " accounts to " << m_config_file_path;
        } else {
            BOOST_LOG_TRIVIAL(error) << "Failed to open accounts file for writing: " << m_config_file_path;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error saving accounts: " << e.what();
    }
}

void AccountManager::load_accounts()
{
    try {
        if (!boost::filesystem::exists(m_config_file_path)) {
            BOOST_LOG_TRIVIAL(info) << "Accounts file does not exist: " << m_config_file_path;
            return;
        }
        
        std::ifstream file(m_config_file_path);
        if (!file.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "Failed to open accounts file for reading: " << m_config_file_path;
            return;
        }
        
        json j;
        file >> j;
        file.close();
        
        m_accounts.clear();
        if (j.is_array()) {
            for (const auto& item : j) {
                m_accounts.push_back(BambuAccount::from_json(item));
            }
            BOOST_LOG_TRIVIAL(info) << "Loaded " << m_accounts.size() << " accounts from " << m_config_file_path;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Error loading accounts: " << e.what();
    }
}

void AccountManager::update_active_account(const std::string& user_id)
{
    for (auto& account : m_accounts) {
        account.is_active = (account.user_id == user_id);
    }
}

} // namespace GUI
} // namespace Slic3r
