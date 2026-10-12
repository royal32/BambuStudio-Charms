#include "PlateVectorPreview.hpp"
#include <boost/nowide/cstdio.hpp>
#include <miniz/miniz.h>
#include <openssl/sha.h>
#include <array>
#include <map>

namespace Slic3r { namespace GUI {
namespace {
constexpr mz_uint64 max_member = 512ull * 1024 * 1024;
constexpr mz_uint64 max_json = 8ull * 1024 * 1024;
constexpr const char* preview_member = "Metadata/charm_library/preview.json";
struct Archive {
    FILE* file{nullptr};
    mz_zip_archive zip{};
    explicit Archive(const std::string& path) : file(boost::nowide::fopen(path.c_str(), "rb")) {
        if (file && !mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
            fclose(file);
            file = nullptr;
        }
    }
    ~Archive() { if (file) { mz_zip_reader_end(&zip); fclose(file); } }
};
size_t hash_bytes(void* context, mz_uint64, const void* data, size_t size)
{
    return SHA256_Update(static_cast<SHA256_CTX*>(context), data, size) == 1 ? size : 0;
}
}
std::unique_ptr<PlateVectorPreview> load_plate_vector_preview(const std::string& job, int plate_index)
{
    try {
        Archive archive(job);
        if (!archive.file || mz_zip_reader_get_num_files(&archive.zip) > 10000) return nullptr;
        std::map<std::string, mz_uint> members;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.zip); ++i) {
            const auto length = mz_zip_reader_get_filename(&archive.zip, i, nullptr, 0);
            if (!length || length > 65536) return nullptr;
            std::string name(length, '\0');
            mz_zip_reader_get_filename(&archive.zip, i, name.data(), length);
            name.resize(length - 1);
            if (!members.emplace(name, i).second) return nullptr;
        }
        const auto found = members.find(preview_member);
        if (found == members.end()) return nullptr;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.zip, found->second, &stat) || stat.m_uncomp_size > max_json) return nullptr;
        std::string raw(static_cast<size_t>(stat.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&archive.zip, found->second, raw.data(), raw.size(), 0)) return nullptr;
        auto preview = std::make_unique<PlateVectorPreview>(PlateVectorPreview::parse(nlohmann::json::parse(raw)));
        if (plate_index >= 0 && preview->plate_index != plate_index) return nullptr;
        std::array<std::string, 3> required{{"Metadata/plate_" + std::to_string(preview->plate_index) + ".gcode",
            "Metadata/project_settings.config", "Metadata/slice_info.config"}};
        std::sort(required.begin(), required.end());
        SHA256_CTX hash;
        if (SHA256_Init(&hash) != 1) return nullptr;
        for (const auto& name : required) {
            const auto member = members.find(name);
            if (member == members.end() || !mz_zip_reader_file_stat(&archive.zip, member->second, &stat) || stat.m_uncomp_size > max_member) return nullptr;
            SHA256_Update(&hash, name.c_str(), name.size() + 1);
            std::array<unsigned char, 8> length{};
            for (size_t i = 0; i < length.size(); ++i) length[7 - i] = static_cast<unsigned char>(stat.m_uncomp_size >> (8 * i));
            SHA256_Update(&hash, length.data(), length.size());
            if (!mz_zip_reader_extract_to_callback(&archive.zip, member->second, hash_bytes, &hash, 0)) return nullptr;
        }
        unsigned char bytes[SHA256_DIGEST_LENGTH];
        SHA256_Final(bytes, &hash);
        std::string digest;
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char byte : bytes) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
        if (digest != preview->print_sha256) return nullptr;
        return preview;
    } catch (const std::exception&) {
        return nullptr;
    }
}
}}
