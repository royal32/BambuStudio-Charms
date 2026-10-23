#include "slic3r/GUI/StudioAssistantModel.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Format/STL.hpp"
#include <cassert>
#include <iostream>
#include <boost/filesystem.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI::Assistant;
template<class F> void rejects(F&& fn) { bool rejected=false; try { fn(); } catch (...) { rejected=true; } assert(rejected); }
int main(int argc,char** argv)
{
    set_resources_dir(std::string(PROJECT_RESOURCES));
    boost::filesystem::create_directories(PROJECT_TEST_TMP);
    set_temporary_dir(PROJECT_TEST_TMP); set_data_dir(PROJECT_TEST_TMP);
    Model model;
    auto* object=model.add_object("Two color charm","",make_cube(10,10,2));
    object->add_instance(); object->config.set("extruder",1);
    auto* volume=object->volumes.front(); volume->config.set("extruder",2);
    TriangleSelector paint(volume->mesh()); paint.set_facet(0,EnforcerBlockerType(1)); paint.set_facet(1,EnforcerBlockerType(2));
    volume->mmu_segmentation_facets.set(paint);
    TriangleSelector supports(volume->mesh()); supports.set_facet(3,EnforcerBlockerType::ENFORCER); volume->supported_facets.set(supports);
    const auto original_supports=volume->supported_facets.get_data();
    auto* hole=object->add_volume(make_cube(1,1,4),ModelVolumeType::NEGATIVE_VOLUME);
    DynamicPrintConfig source,target;
    source.set_key_value("filament_colour",new ConfigOptionStrings({"#FF0000","#FFFF00"}));
    source.set_key_value("filament_type",new ConfigOptionStrings({"PLA","PLA"}));
    target.set_key_value("filament_colour",new ConfigOptionStrings({"#ffff00","#ff0000"}));
    target.set_key_value("filament_type",new ConfigOptionStrings({"PLA","PLA"}));
    const auto mapping=source_filament_mapping(*object,source,target,2);
    assert(mapping.at(1)==2 && mapping.at(2)==1);
    apply_source_filament_mapping(*object,mapping);
    assert(object->config.extruder()==2); assert(volume->config.extruder()==1);
    TriangleSelector check(volume->mesh()); check.deserialize(volume->mmu_segmentation_facets.get_data(),false);
    assert(int(check.get_triangles().at(0).get_state())==2); assert(int(check.get_triangles().at(1).get_state())==1);
    assert(volume->supported_facets.get_data()==original_supports); assert(hole->is_negative_volume());
    const auto original_guid=volume->ensure_part_guid();
    auto* clone=copy_source_object(model,*object);
    assert(clone->id()!=object->id()); assert(clone->volumes.size()==object->volumes.size());
    assert(clone->volumes.at(0)->supported_facets.get_data()==original_supports); assert(clone->volumes.at(1)->is_negative_volume());
    assert(clone->volumes.at(0)->part_guid()!=original_guid); assert(volume->part_guid()==original_guid);
    const auto parts=part_targets(*object,{std::to_string(volume->id().id)});
    rejects([&] { validate_part_filament(parts,1,2,false); });
    rejects([&] { validate_part_filament(parts,3,2,true); });
    rejects([&] { part_targets(*object,{std::to_string(hole->id().id)}); });
    rejects([&] { part_targets(*object,{std::to_string(clone->volumes[0]->id().id)}); });
    rejects([&] { part_targets(*object,{std::to_string(volume->id().id),std::to_string(volume->id().id)}); });
    assert(!volume->mmu_segmentation_facets.empty()); assert(volume->config.extruder()==1);
    validate_part_filament(parts,2,2,true); assign_part_filament(parts,2,true);
    assert(volume->config.extruder()==2); assert(volume->mmu_segmentation_facets.empty());
    assert(volume->supported_facets.get_data()==original_supports); assert(object->config.extruder()==2);
    assert(!clone->volumes[0]->mmu_segmentation_facets.empty());
    volume->config.set("extruder",1); volume->mmu_segmentation_facets.set(check);
    target.set_key_value("filament_type",new ConfigOptionStrings({"PETG","PETG"}));
    rejects([&] { source_filament_mapping(*object,source,target,2); });
    assert(source_filament_mapping(*object,source,target,2,std::vector<int>{2,1}).at(1)==2);
    rejects([&] { source_filament_mapping(*object,source,target,2,std::vector<int>{3,1}); });
    rejects([&] { source_filament_mapping(*object,source,target,2,std::vector<int>{1}); });
    std::cout << "Painted-color swaps, material mismatch refusal, explicit mapping, support masks, negative volumes, and clone identities passed.\n";
    // Exercise the real slicer warning lifecycle used by inspect_project:
    // an unsupported overhang, followed by native support generation/reslicing.
    Model overhang;
    auto* unsupported=overhang.add_object("Unsupported overhang","",make_cube(6,6,6));
    auto cap=make_cube(24,24,2); cap.translate(Vec3f(-9,-9,6)); unsupported->add_volume(std::move(cap));
    unsupported->add_instance(Vec3d(80,80,-0.4),Vec3d::Ones(),Vec3d::Zero(),Vec3d::Ones());
    const auto fixture=(boost::filesystem::path(PROJECT_TEST_TMP)/"Unsupported overhang.stl").string();
    assert(store_stl(fixture.c_str(), &overhang, true));
    auto process=DynamicPrintConfig::full_print_config();
    process.set_key_value("enable_support",new ConfigOptionBool(false));
    process.set_key_value("enable_prime_tower",new ConfigOptionBool(false));
    Print print;
    print.apply(overhang,process); print.process();
    auto needs_support=[&] {
        for (const auto* item:print.objects()) for (int step=0;step<posCount;++step)
            for (const auto& warning:item->step_state_with_warnings(PrintObjectStep(step)).warnings)
                if (warning.current && warning.message_id==PrintStateBase::SlicingNeedSupportOn) return true;
        return false;
    };
    assert(needs_support());
    DynamicPrintConfig object_support;
    object_support.set_key_value("enable_support",new ConfigOptionBool(true));
    object_support.set_key_value("support_on_build_plate_only",new ConfigOptionBool(false));
    validate_object_settings(object_support);
    // These global-only options can be stored in ModelConfig, but the slicer
    // does not consume them as object overrides. Refuse misleading success.
    for (const auto* key : {"enable_prime_tower", "print_sequence"}) {
        DynamicPrintConfig invalid;
        invalid.set_key_value(key, process.option(key)->clone());
        rejects([&] { validate_object_settings(invalid); });
    }
    const auto support_pose=unsupported->instances.front()->get_transformation().get_matrix();
    unsupported->config.apply(object_support);
    assert(unsupported->instances.front()->get_transformation().get_matrix().matrix().isApprox(support_pose.matrix()));
    print.apply(overhang,process); print.process();
    assert(!needs_support());
    bool generated=false; for (const auto* item:print.objects()) generated|=!item->support_layers().empty(); assert(generated);
    assert(!process.opt_bool("enable_support"));
    std::cout << "Object-only setting validation and native support override passed; global support remains disabled and object placement is unchanged.\n";
    std::cout << "Real slicer: unsupported-overhang warning detected; object support generation and reslice produced support layers and refreshed diagnostics.\n";
    for (int i=1;i<argc;++i) {
        DynamicPrintConfig config; auto project=read_source_model(argv[i],config);
        if (boost::filesystem::path(argv[i]).extension()==".stl") {
            assert(project.objects.size()==1); auto* item=project.objects.front(); assert(item->volumes.size()==1);
            auto* mesh=item->volumes.front(); const auto before=item->raw_mesh_bounding_box();
            assert(mesh->is_splittable()); const auto count=mesh->split(4,1.0f); assert(count>1);
            item->invalidate_bounding_box(); const auto after=item->raw_mesh_bounding_box();
            assert((before.min-after.min).norm()<1e-4 && (before.max-after.max).norm()<1e-4);
            size_t largest=0; double largest_volume=-1;
            for (size_t j=0;j<item->volumes.size();++j) {
                const auto* part=item->volumes[j]; const auto box=part->mesh().transformed_bounding_box(part->get_matrix());
                const auto volume=std::abs(double(its_volume(part->mesh().its)));
                if (volume>largest_volume) { largest_volume=volume; largest=j; }
                std::cout<<"STL part "<<j+1<<": "<<part->name<<", size "<<box.size().transpose()<<", volume "<<volume<<" mm3\n";
            }
            for (size_t j=0;j<item->volumes.size();++j) {
                auto target_parts=part_targets(*item,{std::to_string(item->volumes[j]->id().id)});
                validate_part_filament(target_parts,j==largest?2:1,4,false); assign_part_filament(target_parts,j==largest?2:1,false);
            }
            for (size_t j=0;j<item->volumes.size();++j) assert(item->volumes[j]->config.extruder()==(j==largest?2:1));
            std::cout<<"STL disconnected-shell split preserved placement; "<<count<<" parts assigned with largest part in white slot 2 and inner pieces in red slot 1; passed.\n";
            continue;
        }
        assert(!project.objects.empty()); assert(config.has("filament_colour")); assert(config.has("printer_model"));
        size_t volumes=0,painted=0,negatives=0;
        const auto* colors=config.option<ConfigOptionStrings>("filament_colour"); assert(colors && !colors->values.empty());
        for (const auto* item:project.objects) {
            const auto identity=source_filament_mapping(*item,config,config,colors->values.size());
            assert(!identity.empty());
            for (const auto* part:item->volumes) { ++volumes; painted+=!part->mmu_segmentation_facets.empty(); negatives+=part->is_negative_volume(); }
        }
        std::cout << "Source fixture " << i << ": " << project.objects.size() << " objects, " << volumes << " volumes, " << painted << " painted, " << negatives << " negative; geometry/config/material mapping passed.\n";
    }
}
