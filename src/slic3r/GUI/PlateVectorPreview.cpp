#include "PlateVectorPreview.hpp"
#include <boost/nowide/cstdio.hpp>
#include <miniz/miniz.h>
#include <openssl/sha.h>
#include <array>
#include <map>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include "libslic3r/Utils.hpp"

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
std::map<std::string,mz_uint> member_map(Archive& archive)
{
    if (!archive.file || mz_zip_reader_get_num_files(&archive.zip)>10000) throw std::runtime_error("Invalid preview archive");
    std::map<std::string,mz_uint> members;
    for (mz_uint i=0;i<mz_zip_reader_get_num_files(&archive.zip);++i) {
        const auto length=mz_zip_reader_get_filename(&archive.zip,i,nullptr,0);
        if (!length || length>65536) throw std::runtime_error("Invalid member name");
        std::string name(length,'\0');mz_zip_reader_get_filename(&archive.zip,i,name.data(),length);name.resize(length-1);
        if (!members.emplace(name,i).second) throw std::runtime_error("Duplicate archive member");
    }
    return members;
}
std::string fingerprint(Archive& archive,const std::map<std::string,mz_uint>& members,int plate)
{
    std::array<std::string,3> names{{"Metadata/plate_"+std::to_string(plate)+".gcode","Metadata/project_settings.config","Metadata/slice_info.config"}};
    std::sort(names.begin(),names.end());SHA256_CTX hash;
    if (SHA256_Init(&hash)!=1) throw std::runtime_error("Cannot hash preview");
    for (const auto& name:names) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.zip,members.at(name),&stat) || stat.m_uncomp_size>max_member) throw std::runtime_error("Invalid print member");
        SHA256_Update(&hash,name.c_str(),name.size()+1);
        std::array<unsigned char,8> size{};
        for(size_t i=0;i<8;++i) size[7-i]=static_cast<unsigned char>(stat.m_uncomp_size>>(8*i));
        SHA256_Update(&hash,size.data(),size.size());
        if (!mz_zip_reader_extract_to_callback(&archive.zip,members.at(name),hash_bytes,&hash,0)) throw std::runtime_error("Cannot read print member");
    }
    unsigned char bytes[SHA256_DIGEST_LENGTH];SHA256_Final(bytes,&hash);
    std::string digest;constexpr char hex[]="0123456789abcdef";
    for (auto byte:bytes) {digest+=hex[byte>>4];digest+=hex[byte&15];}return digest;
}
std::string indexed_member(int plate) {return "Metadata/charm_library/plate_"+std::to_string(plate)+".preview.json";}
bool artwork_member(const std::string& name)
{
    return name==preview_member || (name.rfind("Metadata/charm_library/plate_",0)==0 && boost::ends_with(name,".preview.json"));
}
std::string read_member(Archive& archive,const std::map<std::string,mz_uint>& members,const std::string& name)
{
    mz_zip_archive_file_stat stat{};
    if(!mz_zip_reader_file_stat(&archive.zip,members.at(name),&stat) || stat.m_uncomp_size>max_json) throw std::runtime_error("Invalid preview metadata");
    std::string raw(size_t(stat.m_uncomp_size),'\0');
    if(!mz_zip_reader_extract_to_mem(&archive.zip,members.at(name),raw.data(),raw.size(),0)) throw std::runtime_error("Cannot read preview metadata");
    return raw;
}
void validate_sliced_colors(const std::string& xml,const nlohmann::json& preview)
{
    boost::property_tree::ptree document;std::istringstream input(xml);boost::property_tree::read_xml(input,document);
    std::map<int,std::string> colors;
    for(const auto& node:document.get_child("config")) if(node.first=="plate") {
        int index=-1;
        for(const auto& child:node.second) if(child.first=="metadata" && child.second.get<std::string>("<xmlattr>.key","")=="index") index=child.second.get<int>("<xmlattr>.value");
        if(index!=preview.at("plate_index").get<int>()) continue;
        for(const auto& child:node.second) if(child.first=="filament" && child.second.get<double>("<xmlattr>.used_g",0)>0)
            colors[child.second.get<int>("<xmlattr>.id")-1]=child.second.get<std::string>("<xmlattr>.color").substr(0,7);
    }
    auto folded=[](std::string color){std::transform(color.begin(),color.end(),color.begin(),[](unsigned char c){return char(std::toupper(c));});return color;};
    for(const auto& region:preview.at("regions"))
        if(!colors.count(region.at("filament")) || folded(colors.at(region.at("filament")))!=folded(region.at("color"))) throw std::runtime_error("Artwork does not match sliced filament slots");
}
}
std::unique_ptr<PlateVectorPreview> load_plate_vector_preview(const std::string& job, int plate_index)
{
    try {
        Archive archive(job);
        const auto members=member_map(archive);
        auto found=plate_index>0?members.find(indexed_member(plate_index)):members.end();
        if (found==members.end()) found=members.find(preview_member);
        if (found == members.end()) return nullptr;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.zip, found->second, &stat) || stat.m_uncomp_size > max_json) return nullptr;
        std::string raw(static_cast<size_t>(stat.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&archive.zip, found->second, raw.data(), raw.size(), 0)) return nullptr;
        auto preview = std::make_unique<PlateVectorPreview>(PlateVectorPreview::parse(nlohmann::json::parse(raw)));
        if (plate_index >= 0 && preview->plate_index != plate_index) return nullptr;
        if (fingerprint(archive,members,preview->plate_index)!=preview->print_sha256) return nullptr;
        return preview;
    } catch (const std::exception&) {
        return nullptr;
    }
}
bool embed_plate_vector_previews(const std::string& job,const std::vector<nlohmann::json>& previews)
{
    if (previews.empty()) return false;
    const auto temporary=boost::filesystem::path(job).parent_path()/boost::filesystem::unique_path(".vector-preview-%%%%-%%%%-%%%%.3mf");
    struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove(path,error);}} cleanup{temporary};
    try {
        std::map<std::string,std::string> additions;
        {
            Archive original(job);const auto members=member_map(original);
            size_t plates=0;
            for(const auto& member:members) if(member.first.rfind("Metadata/plate_",0)==0 && boost::ends_with(member.first,".gcode")) ++plates;
            for(auto data:previews) {
                const int plate=data.at("plate_index");data["print_sha256"]=fingerprint(original,members,plate);
                PlateVectorPreview::parse(data);
                validate_sliced_colors(read_member(original,members,"Metadata/slice_info.config"),data);
                auto raw=data.dump();if(raw.size()>max_json) throw std::runtime_error("Preview too large");
                if (!additions.emplace(plates==1?preview_member:indexed_member(plate),std::move(raw)).second) throw std::runtime_error("Duplicate preview plate");
            }
            mz_zip_archive_file_stat stat{};
            if(!mz_zip_reader_file_stat(&original.zip,members.at("[Content_Types].xml"),&stat) || stat.m_uncomp_size>max_json) throw std::runtime_error("Invalid content types");
            std::string xml(size_t(stat.m_uncomp_size),'\0');
            if(!mz_zip_reader_extract_to_mem(&original.zip,members.at("[Content_Types].xml"),xml.data(),xml.size(),0)) throw std::runtime_error("Cannot read content types");
            boost::property_tree::ptree tree;std::istringstream input(xml);boost::property_tree::read_xml(input,tree);
            auto root=std::find_if(tree.begin(),tree.end(),[](const auto& node){return node.first=="Types" || boost::ends_with(node.first,":Types");});
            if(root==tree.end()) throw std::runtime_error("Invalid content types root");
            auto& types=root->second;
            for(auto item=types.begin();item!=types.end();) {
                auto name=item->second.get<std::string>("<xmlattr>.PartName","");
                if(!name.empty() && artwork_member(name.substr(1))) item=types.erase(item);else ++item;
            }
            const auto colon=root->first.find(':');
            const std::string tag=colon==std::string::npos?"Override":root->first.substr(0,colon+1)+"Override";
            for(const auto& addition:additions) {
                boost::property_tree::ptree value;value.put("<xmlattr>.PartName","/"+addition.first);value.put("<xmlattr>.ContentType","application/json");types.add_child(tag,value);
            }
            std::ostringstream output;boost::property_tree::write_xml(output,tree);additions["[Content_Types].xml"]=output.str();
            struct Writer {
                FILE* file{nullptr};mz_zip_archive zip{};bool active{false};
                ~Writer(){if(active)mz_zip_writer_end(&zip);if(file)fclose(file);}
            } writer;
            writer.file=boost::nowide::fopen(temporary.string().c_str(),"wb");
            if(!writer.file || !(writer.active=mz_zip_writer_init_cfile(&writer.zip,writer.file,MZ_ZIP_FLAG_WRITE_ZIP64))) throw std::runtime_error("Cannot stage preview");
            for(const auto& member:members) {
                if(artwork_member(member.first) || member.first=="[Content_Types].xml") continue;
                if(!mz_zip_writer_add_from_zip_reader(&writer.zip,&original.zip,member.second)) throw std::runtime_error("Cannot copy print member");
            }
            for(const auto& addition:additions) if(!mz_zip_writer_add_mem(&writer.zip,addition.first.c_str(),addition.second.data(),addition.second.size(),MZ_DEFAULT_COMPRESSION)) throw std::runtime_error("Cannot add preview member");
            if(!mz_zip_writer_finalize_archive(&writer.zip)) throw std::runtime_error("Cannot finalize preview");
            mz_zip_writer_end(&writer.zip);writer.active=false;
            const auto closed=fclose(writer.file);writer.file=nullptr;if(closed) throw std::runtime_error("Cannot close preview");
        }
        for(const auto& data:previews) if(!load_plate_vector_preview(temporary.string(),data.at("plate_index"))) throw std::runtime_error("Preview verification failed");
#ifdef _WIN32
        if(Slic3r::rename_file(temporary.string(),job)) throw std::runtime_error("Cannot publish preview");
#else
        if(boost::nowide::rename(temporary.string().c_str(),job.c_str())) throw std::runtime_error("Cannot publish preview");
#endif
        return true;
    } catch(const std::exception&) {return false;}
}
}}
