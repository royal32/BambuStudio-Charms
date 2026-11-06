#include "slic3r/GUI/PlateVectorGeometry.hpp"
#include "slic3r/GUI/StudioAssistantModel.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_test_paths.hpp"
#include <boost/nowide/cstdio.hpp>
#include <miniz/miniz.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Json=nlohmann::json;
namespace test_fs=std::filesystem;
using Members=std::map<std::string,std::string>;
void write_zip(const test_fs::path& path,const Members& members) {
    FILE* file=boost::nowide::fopen(path.u8string().c_str(),"wb");assert(file);
    mz_zip_archive zip{};assert(mz_zip_writer_init_cfile(&zip,file,MZ_ZIP_FLAG_WRITE_ZIP64));
    for(const auto& member:members) assert(mz_zip_writer_add_mem(&zip,member.first.c_str(),member.second.data(),member.second.size(),MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));mz_zip_writer_end(&zip);assert(!fclose(file));
}
Members read_zip(const test_fs::path& path) {
    FILE* file=boost::nowide::fopen(path.u8string().c_str(),"rb");assert(file);
    mz_zip_archive zip{};assert(mz_zip_reader_init_cfile(&zip,file,0,0));Members members;
    for(mz_uint i=0;i<mz_zip_reader_get_num_files(&zip);++i) {
        mz_zip_archive_file_stat stat{};assert(mz_zip_reader_file_stat(&zip,i,&stat));
        std::string raw(size_t(stat.m_uncomp_size),'\0');assert(mz_zip_reader_extract_to_mem(&zip,i,raw.data(),raw.size(),0));
        assert(members.emplace(stat.m_filename,std::move(raw)).second);
    }
    mz_zip_reader_end(&zip);fclose(file);return members;
}
Members job_members(int plates=1) {
    Members members={{"[Content_Types].xml","<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"gcode\" ContentType=\"text/x.gcode\"/></Types>"},
        {"Metadata/project_settings.config","{\"filament_colour\":[\"#FFFFFF\",\"#FFFFFF\"]}"},
        {"Metadata/plate_1.png","unchanged PNG bytes"}};
    std::string slice="<config>";
    for(int p=1;p<=plates;++p) {
        members["Metadata/plate_"+std::to_string(p)+".gcode"]="; plate "+std::to_string(p)+"\nG1 X1 Y2\n";
        slice+="<plate><metadata key=\"index\" value=\""+std::to_string(p)+"\"/><filament id=\"1\" color=\"#FFFFFF\" used_g=\"1\"/><filament id=\"2\" color=\"#FFFFFF\" used_g=\"1\"/></plate>";
    }
    members["Metadata/slice_info.config"]=slice+"</config>";return members;
}
void print_unchanged(const Members& before,const Members& after) {
    for(const auto& member:before) if(member.first!="[Content_Types].xml" && member.first.rfind("Metadata/charm_library/",0)!=0) assert(after.at(member.first)==member.second);
}
int main(int argc,char** argv) {
    configure_assistant_test_paths();
    const test_fs::path root=test_fs::u8path(assistant_test_tmp());
    Model model;auto* object=model.add_object("Charm","",make_cube(20,20,2));object->add_instance();
    object->add_instance(Vec3d(30,0,0),Vec3d(2,1,1),Vec3d::Zero(),Vec3d::Ones());
    auto* colored=object->add_volume(make_cube(10,20,2));colored->config.set("extruder",2);
    auto cut=make_cube(2,2,4);cut.translate(Vec3f(9,4,-1));object->add_volume(std::move(cut),ModelVolumeType::NEGATIVE_VOLUME);
    DynamicPrintConfig config;config.set_key_value("filament_colour",new ConfigOptionStrings({"#FFFFFF","#FFFFFF"}));
    auto geometry=generate_plate_vector_geometry(model,config,{{0,0}},1,{0,1});assert(!geometry.empty());
    auto parsed=PlateVectorPreview::parse(geometry);
    assert(std::abs(parsed.width-20)<1e-4 && std::abs(parsed.height-20)<1e-4);
    assert(parsed.hit_test({17,17})==1 && parsed.hit_test({5,5})==0);
    assert(parsed.hit_test({9.5,15})==-1 && parsed.hit_test({10.5,15})==-1);
    const auto second=generate_plate_vector_geometry(model,config,{{0,1}},2,{0,1});assert(std::abs(second.at("width").get<double>()-40)<1e-4);
    std::rotate(object->volumes.begin(),object->volumes.end()-1,object->volumes.end());
    auto reordered=PlateVectorPreview::parse(generate_plate_vector_geometry(model,config,{{0,0}},1,{0,1}));
    assert(reordered.hit_test({9.5,15})==-1 && reordered.hit_test({10.5,15})==-1 && reordered.hit_test({17,17})==1);
    TriangleSelector paint(colored->mesh());paint.set_facet(0,EnforcerBlockerType(1));colored->mmu_segmentation_facets.set(paint);
    assert(generate_plate_vector_geometry(model,config,{{0,0}},1,{0,1}).empty());colored->mmu_segmentation_facets.reset();
    auto* sculpture=model.add_object("Tall sculpture","",make_cube(10,10,40));sculpture->add_instance();
    assert(generate_plate_vector_geometry(model,config,{{1,0}},1,{0}).empty());
    std::cout<<"Native transformed plate selection, equal-RGB independent slots, negative holes/order and painted/3D fallback passed.\n";

    auto job=root/"single.gcode.3mf";const auto original=job_members();write_zip(job,original);
    assert(embed_plate_vector_previews(job.u8string(),{geometry}));auto first=load_plate_vector_preview(job.u8string(),1);assert(first && first->hit_test({17,17})==1);
    print_unchanged(original,read_zip(job));const auto sha=first->print_sha256;
    auto changed=job_members();changed["Metadata/plate_1.gcode"]+="; fresh re-export\n";write_zip(job,changed);
    assert(embed_plate_vector_previews(job.u8string(),{geometry}));assert(load_plate_vector_preview(job.u8string())->print_sha256!=sha);print_unchanged(changed,read_zip(job));
    auto mismatch=geometry;mismatch["regions"][0]["color"]="#FF0000";
    const auto snapshot=read_zip(job);assert(!embed_plate_vector_previews(job.u8string(),{mismatch}));assert(read_zip(job)==snapshot);
    auto stale=snapshot;stale["Metadata/plate_1.gcode"]+="; changed without regenerating\n";write_zip(job,stale);assert(!load_plate_vector_preview(job.u8string()));
    auto multi=root/"multi.gcode.3mf";const auto originals=job_members(2);write_zip(multi,originals);
    assert(embed_plate_vector_previews(multi.u8string(),{geometry,second}));
    assert(load_plate_vector_preview(multi.u8string(),1)->width<21 && load_plate_vector_preview(multi.u8string(),2)->width>39);
    assert(!load_plate_vector_preview(multi.u8string()));print_unchanged(originals,read_zip(multi));
    assert(embed_plate_vector_previews(multi.u8string(),{geometry,second}));
    const auto portable=root/"portable.gcode.3mf";test_fs::copy_file(multi,portable,test_fs::copy_options::overwrite_existing);
    assert(load_plate_vector_preview(portable.u8string(),2));
    std::cout<<"Atomic per-plate/multiplate packaging, regeneration, portability, stale/mismatched rejection and unchanged print members passed.\n";

    if(argc==3) {
        DynamicPrintConfig source;PlateDataPtrs plates;std::vector<Preset*> presets;
        Model actual=Model::read_from_file(argv[1],&source,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig|LoadStrategy::AddDefaultInstances|LoadStrategy::Silence,&plates,&presets);
        assert(!plates.empty());const PlateData* plate=nullptr;for(const auto* p:plates)if(p->plate_index==0)plate=p;assert(plate);
        // Import metadata stores source object IDs/identify IDs. The GUI later
        // builds its zero-based object/instance set; reproduce that correspondence.
        std::vector<std::pair<int,int>> selected;
        for(size_t o=0;o<actual.objects.size();++o) for(size_t i=0;i<actual.objects[o]->instances.size();++i)
            for(const auto& entry:plate->obj_inst_map) if(entry.second.first==int(i) && size_t(entry.second.second)==actual.objects[o]->instances[i]->loaded_id)
                selected.emplace_back(int(o),int(i));
        assert(!selected.empty());
        auto preview=generate_plate_vector_geometry(actual,source,selected,1,{0,1,2,3});assert(!preview.empty());
        const auto output=root/"Namaste.gcode.3mf";test_fs::copy_file(test_fs::u8path(argv[2]),output,test_fs::copy_options::overwrite_existing);
        const auto before=read_zip(output);assert(embed_plate_vector_previews(output.u8string(),{preview}));print_unchanged(before,read_zip(output));
        const auto view=load_plate_vector_preview(output.u8string(),1);assert(view && view->regions.size()==3);
        assert(std::abs(view->width-93.04349)<.01 && std::abs(view->height-70.74078)<.01);
        std::ofstream raw(root/"Namaste.preview.json");raw<<preview.dump(2);
        for(auto* p:plates)delete p;for(auto* p:presets)delete p;
        std::cout<<"Actual Namaste project/plate export: three independent slots, expected 93.04349 x 70.74078 mm bounds and unchanged G-code/settings passed.\n";
    }
}
