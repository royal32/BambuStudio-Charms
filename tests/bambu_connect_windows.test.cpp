// Standalone native Windows checks; no printer or Connect account required.
#include "catch_main.hpp"
#include "slic3r/GUI/BambuConnectBridgeWin.cpp"

using namespace Slic3r::GUI::BambuConnect;

TEST_CASE("Connect Windows pipes are private and only child handles inherit", "[ConnectWindows]")
{
    for (bool writing : {false, true}) {
        Handle parent, child;
        make_pipe(parent, child, writing);
        DWORD flags = 0;
        REQUIRE(GetHandleInformation(parent.value, &flags));
        REQUIRE((flags & HANDLE_FLAG_INHERIT) == 0);
        REQUIRE(GetHandleInformation(child.value, &flags));
        REQUIRE((flags & HANDLE_FLAG_INHERIT) != 0);
        const char text[] = u8"café 猫 & #1\0second";
        const std::string payload(text, sizeof(text) - 1);
        DWORD count = 0;
        if (writing) {
            REQUIRE(transfer_pipe(parent.value, const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), true,
                Transport::Clock::now() + std::chrono::seconds(1)) == payload.size());
            char buffer[64];
            REQUIRE(ReadFile(child.value, buffer, sizeof(buffer), &count, nullptr));
            REQUIRE(std::string(buffer, count) == payload);
        } else {
            REQUIRE(WriteFile(child.value, payload.data(), static_cast<DWORD>(payload.size()), &count, nullptr));
            char buffer[64];
            count = transfer_pipe(parent.value, buffer, sizeof(buffer), false, Transport::Clock::now() + std::chrono::seconds(1));
            REQUIRE(std::string(buffer, count) == payload);
        }
    }
}

TEST_CASE("Connect Windows pipe timeout cancels pending I/O", "[ConnectWindows]")
{
    Handle parent, child;
    make_pipe(parent, child, false);
    const auto started = Transport::Clock::now();
    char bytes[32];
    REQUIRE_THROWS_WITH(transfer_pipe(parent.value, bytes, sizeof(bytes), false, started + std::chrono::milliseconds(100)),
        "Connect did not respond in time.");
    REQUIRE(Transport::Clock::now() - started < std::chrono::seconds(2));
    // The cancelled stack OVERLAPPED must be fully retired before another read.
    DWORD count = 0;
    REQUIRE(WriteFile(child.value, "ok", 2, &count, nullptr));
    REQUIRE(transfer_pipe(parent.value, bytes, sizeof(bytes), false, Transport::Clock::now() + std::chrono::seconds(1)) == 2);
    REQUIRE(std::string(bytes, 2) == "ok");
    child.reset();
    REQUIRE_THROWS_WITH(transfer_pipe(parent.value, bytes, sizeof(bytes), false, Transport::Clock::now() + std::chrono::seconds(1)),
        "Connect closed its pipe.");
}

TEST_CASE("Connect Windows session reader fails closed for unverified layouts", "[ConnectWindows]")
{
    REQUIRE(studio_access_token(nullptr, nullptr, "account").empty());
    REQUIRE(studio_access_token(reinterpret_cast<void*>(1), reinterpret_cast<void*>(1), "account").empty());
}
