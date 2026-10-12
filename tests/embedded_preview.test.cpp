// Link PlateVectorPreview.cpp, miniz and OpenSSL. Arguments: valid job, stale job, missing-preview job.
#include "slic3r/GUI/PlateVectorPreview.hpp"
#include <cassert>
#include <iostream>
#ifdef _WIN32
#include <boost/nowide/convert.hpp>
int wmain(int argc, wchar_t** wide_argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(boost::nowide::narrow(wide_argv[i]));
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
#else
int main(int argc, char** argv)
{
#endif
    using Slic3r::GUI::load_plate_vector_preview;
    assert(argc == 4);
    const auto preview = load_plate_vector_preview(argv[1], 5);
    assert(preview && preview->hit_test({9, 2}) == 0);
    assert(!load_plate_vector_preview(argv[1], 1));
    assert(!load_plate_vector_preview(argv[2]));
    assert(!load_plate_vector_preview(argv[3]));
    std::cout << "Embedded native reader: Unicode path, fingerprint, plate identity and PNG fallbacks passed.\n";
}
