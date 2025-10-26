#include "catch_main.hpp"
#include "../src/slic3r/GUI/AccountManager.hpp"
#include <boost/filesystem.hpp>

using namespace Slic3r::GUI;

TEST_CASE("AccountManager basic operations", "[account_manager]") {
    // Create a temporary directory for testing
    boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path() / "bambu_test";
    boost::filesystem::create_directories(temp_dir);
    
    // Note: This is a basic structural test. Full testing would require mocking the data_dir() function
    
    SECTION("BambuAccount JSON serialization") {
        BambuAccount account;
        account.user_id = "user123";
        account.user_name = "testuser";
        account.nickname = "Test User";
        account.avatar_url = "https://example.com/avatar.jpg";
        account.auth_token = "token123";
        account.is_active = true;
        
        json j = account.to_json();
        
        REQUIRE(j["user_id"] == "user123");
        REQUIRE(j["user_name"] == "testuser");
        REQUIRE(j["nickname"] == "Test User");
        REQUIRE(j["avatar_url"] == "https://example.com/avatar.jpg");
        REQUIRE(j["auth_token"] == "token123");
        REQUIRE(j["is_active"] == true);
        
        BambuAccount deserialized = BambuAccount::from_json(j);
        REQUIRE(deserialized.user_id == "user123");
        REQUIRE(deserialized.user_name == "testuser");
        REQUIRE(deserialized.nickname == "Test User");
        REQUIRE(deserialized.avatar_url == "https://example.com/avatar.jpg");
        REQUIRE(deserialized.auth_token == "token123");
        REQUIRE(deserialized.is_active == true);
    }
    
    // Clean up
    boost::filesystem::remove_all(temp_dir);
}
