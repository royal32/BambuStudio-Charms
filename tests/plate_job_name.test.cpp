// Run: c++ -std=c++17 tests/plate_job_name.test.cpp -o /tmp/plate-job-name-test && /tmp/plate-job-name-test
#include "../src/slic3r/GUI/PlateJobName.hpp"
#include <cassert>
#include <iostream>

using Slic3r::GUI::plate_job_name;

int main()
{
    assert(plate_job_name("Wanda", "", 0, 1) == "Wanda");
    assert(plate_job_name("Wanda", "", 0, 5) == "Wanda 01");
    assert(plate_job_name("Wanda", "", 4, 5) == "Wanda 05");
    assert(plate_job_name("Wanda", "", 99, 100) == "Wanda 100");
    assert(plate_job_name("Wanda", "Red glasses", 0, 1) == "Red glasses");
    assert(plate_job_name("Wanda", "Red glasses", 4, 5) == "Red glasses");
    assert(plate_job_name("Wanda", "Untitled custom", 4, 5) == "Untitled custom");
    // Whole-project archives keep their project name; a single-plate archive
    // still uses its manual plate name, even through the Export all path.
    assert(plate_job_name("Wanda", "Red glasses", 4, 5, true) == "Wanda");
    assert(plate_job_name("Wanda", "Red glasses", 0, 1, true) == "Red glasses");
    // Sparse imported archives must not append their original index again.
    assert(plate_job_name("Wanda 05", "", 4, 1) == "Wanda 05");
    assert(plate_job_name("Wanda", u8"Mamá’s glasses", 0, 2) == u8"Mamá’s glasses");
    assert(plate_job_name("Wanda", " ../Red: glasses?\n ", 0, 2) == "..Red glasses");
    assert(plate_job_name("", "..", 0, 1) == "Untitled");
    const std::string long_name(250, 'a');
    assert(plate_job_name("Wanda", long_name, 0, 2) == std::string(240, 'a'));
    assert(plate_job_name(long_name, "", 0, 2) == std::string(237, 'a') + " 01");
    const std::string multibyte = std::string(239, 'a') + u8"é";
    assert(plate_job_name("Wanda", multibyte, 0, 2) == std::string(239, 'a'));
    std::cout << "Plate job naming checks passed\n";
}
