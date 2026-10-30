#ifndef CHARMS_ASSISTANT_TEST_PATHS_HPP
#define CHARMS_ASSISTANT_TEST_PATHS_HPP
#include <cstdlib>
#include <filesystem>
#include <string>

inline std::string assistant_test_tmp()
{
    const auto* value=std::getenv("CHARMS_ASSISTANT_TEST_TMP");
    return value && *value ? value : PROJECT_TEST_TMP;
}
inline void configure_assistant_test_paths()
{
    const auto* resources=std::getenv("CHARMS_ASSISTANT_TEST_RESOURCES");
    const auto temporary=assistant_test_tmp();
    std::filesystem::create_directories(temporary);
    Slic3r::set_resources_dir(resources && *resources ? resources : PROJECT_RESOURCES);
    Slic3r::set_data_dir(temporary);Slic3r::set_temporary_dir(temporary);
}
#endif
