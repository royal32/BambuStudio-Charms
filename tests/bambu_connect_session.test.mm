// Standalone macOS bridge checks; see BAMBU_CONNECT.md for the compile command.
#include <catch_main.hpp>
#include "slic3r/GUI/BambuConnectBridge.mm"

using namespace Slic3r::GUI::BambuConnect;

TEST_CASE("Connect session reader handles short and long libc++ strings", "[ConnectSession]")
{
    for (const std::string value : {std::string(), std::string("account-id"), std::string(256, 't')})
        REQUIRE(read_session_string(reinterpret_cast<uintptr_t>(&value)) == value);
}

TEST_CASE("Connect session reader rejects invalid or oversized memory", "[ConnectSession]")
{
    REQUIRE(read_session_string(1).empty());
    std::array<unsigned char, 24> invalid{};
    invalid[23] = 23; // A short string can hold at most 22 characters.
    REQUIRE(read_session_string(reinterpret_cast<uintptr_t>(invalid.data())).empty());
    invalid[23] = 0x80;
    size_t length = 8193;
    std::memcpy(invalid.data() + 8, &length, sizeof(length));
    REQUIRE(read_session_string(reinterpret_cast<uintptr_t>(invalid.data())).empty());
    uintptr_t pointer = 1;
    length = 32;
    std::memcpy(invalid.data(), &pointer, sizeof(pointer));
    std::memcpy(invalid.data() + 8, &length, sizeof(length));
    REQUIRE(read_session_string(reinterpret_cast<uintptr_t>(invalid.data())).empty());
}

TEST_CASE("Connect session adapter refuses an unknown plugin binary", "[ConnectSession]")
{
    REQUIRE_FALSE(supported_session_layout(nullptr));
    REQUIRE_FALSE(supported_session_layout(reinterpret_cast<void*>(&read_own_memory)));
    REQUIRE(studio_access_token(reinterpret_cast<void*>(1), reinterpret_cast<void*>(&read_own_memory), "account-id").empty());
}
