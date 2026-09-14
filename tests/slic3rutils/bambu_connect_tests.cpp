#include <catch2/catch.hpp>
#include "slic3r/GUI/BambuConnect.hpp"

using namespace Slic3r::GUI::BambuConnect;

TEST_CASE("Bambu Connect matches the documented import URL", "[bambu_connect]")
{
    REQUIRE(import_url("/tmp/cube.gcode.3mf", "Cube") ==
            "bambu-connect://import-file?path=%2Ftmp%2Fcube.gcode.3mf&name=Cube&version=1.0.0");
}

TEST_CASE("Bambu Connect keeps filenames out of URL syntax", "[bambu_connect]")
{
    REQUIRE(import_url("/tmp/Mom's charms #1 & 2%.gcode.3mf", "A&B?x=1#two") ==
            "bambu-connect://import-file?path=%2Ftmp%2FMom%27s%20charms%20%231%20%26%202%25.gcode.3mf"
            "&name=A%26B%3Fx%3D1%23two&version=1.0.0");
    REQUIRE(encode_component(u8"café 猫") == "caf%C3%A9%20%E7%8C%AB");
    REQUIRE(encode_component("C:\\Prints\\one.gcode.3mf") == "C%3A%5CPrints%5Cone.gcode.3mf");
    REQUIRE(encode_component("").empty());
}

TEST_CASE("Connect 2.5 beta's extra decode cannot split query values", "[bambu_connect]")
{
    REQUIRE(import_url("/tmp/A&B+50%.gcode.3mf", u8"café & #1", true) ==
            "bambu-connect://import-file?path=%252Ftmp%252FA%2526B%252B50%2525.gcode.3mf"
            "&name=caf%25C3%25A9%2520%2526%2520%25231&version=1.0.0");
}
