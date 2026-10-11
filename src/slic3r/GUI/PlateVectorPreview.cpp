#include "PlateVectorPreview.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/nowide/fstream.hpp>
#include <filesystem>
#include <cctype>

namespace Slic3r { namespace GUI {
std::unique_ptr<PlateVectorPreview> load_plate_vector_preview(const std::string& job, int plate_index)
{
    try {
        const auto path = std::filesystem::u8path(job + ".preview.json");
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8 * 1024 * 1024) return nullptr;
        boost::nowide::ifstream stream(path.u8string());
        auto preview = std::make_unique<PlateVectorPreview>(PlateVectorPreview::parse(nlohmann::json::parse(stream)));
        if (plate_index >= 0 && preview->plate_index != plate_index) return nullptr;
        std::string filename = job, digest;
        if (!bbl_calc_md5(filename, digest)) return nullptr;
        std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(preview->source_md5.begin(), preview->source_md5.end(), preview->source_md5.begin(), [](unsigned char c) { return std::tolower(c); });
        if (digest != preview->source_md5) return nullptr;
        return preview;
    } catch (const std::exception&) {
        return nullptr;
    }
}
}}
