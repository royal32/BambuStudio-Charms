#include "slic3r/GUI/StudioAssistantGeometry.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/Format/STL.hpp"
#include <boost/filesystem.hpp>
#include <cassert>
#include <iostream>
#include <cstdlib>

using namespace Slic3r;
using namespace Slic3r::GUI::Assistant;
template<class F> void rejects(F&& fn) { bool rejected=false;try {fn();} catch (...) {rejected=true;} assert(rejected); }
double area_mm2(const ExPolygons& section) { double area=0;for(const auto& p:section)area+=p.area();return area*SCALING_FACTOR*SCALING_FACTOR; }
ModelVolume* add(ModelObject& object,PreparedPrimitive&& prepared) {
    auto* volume=object.add_volume(std::move(prepared.mesh),prepared.type);
    volume->set_transformation(Geometry::Transformation(prepared.transform));object.invalidate_bounding_box();return volume;
}
int main(int argc,char** argv) {
    const std::string temporary=std::getenv("CHARMS_GEOMETRY_TEST_TMP")?std::getenv("CHARMS_GEOMETRY_TEST_TMP"):PROJECT_TEST_TMP;
    set_resources_dir(PROJECT_RESOURCES);set_temporary_dir(temporary);set_data_dir(temporary);
    boost::filesystem::create_directories(temporary);
    auto xy=[](double x,double y){return Point(scale_(x),scale_(y));};
    ExPolygon outline(Polygon({xy(0,0),xy(20,0),xy(20,30),xy(14,30),xy(14,36),xy(6,36),xy(6,30),xy(0,30)}));
    outline.holes.push_back(Polygon({xy(8,31),xy(8,34),xy(12,34),xy(12,31)}));
    Model model;auto* object=model.add_object("Earring with a loop","",TriangleMesh(slices_to_mesh({{outline}},0,3,3)));
    auto* instance=object->add_instance();instance->set_offset(Vec3d(60,60,0));
    const auto instance_id=std::to_string(instance->id().id);
    auto* part=object->volumes.front();part->config.set("extruder",1);
    auto part_id=std::to_string(part->id().id);const auto object_id=object->id();
    assert(store_stl((temporary+"/Loop earring before.stl").c_str(),&model,true));
    // The old loop opening disappears with the upper tab; a new 2 mm hole is
    // drilled through the retained body, without moving it on the plate.
    const auto transform=instance->get_matrix();
    auto cut=prepare_plane_cut(*object,instance_id,{part_id},Vec3d(70,90,0),Vec3d::UnitY(),false);
    assert(std::abs(cut.removed_mm3-108)<.01);cut.apply(*object);
    assert(object->id()==object_id && std::to_string(part->id().id)!=part_id);
    part_id=std::to_string(part->id().id);
    assert(instance->get_matrix().isApprox(transform));
    assert(std::abs(part->mesh().transformed_bounding_box(part->get_matrix()).max.y()-30)<1e-4);
    add(*object,prepare_primitive(*object,instance_id,"cylinder",true,Vec3d(70,87,1.5),Vec3d(2,2,5),Vec3d::UnitZ()));
    for(double depth:{.2,1.5,2.8}) {
        const auto section=model_section(*object,instance_id,Vec3d(0,0,depth),Vec3d::UnitZ());
        assert(section.size()==1 && section.front().holes.size()==1);
        assert(std::abs(area_mm2(section)-(600-PI))<.005);
        const auto bounds=section.front().holes.front().bounding_box();
        assert(std::abs(unscale<double>(bounds.size().x())-2)<.001);
        assert(std::abs(unscale<double>(bounds.size().y())-2)<.001);
    }
    Print print;auto config=DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower",new ConfigOptionBool(false));
    print.apply(model,config);print.process();
    assert(!print.objects().empty());
    const auto* layer=print.objects().front()->layers().at(3);
    assert(layer->lslices.size()==1 && layer->lslices.front().holes.size()==1);
    std::cout<<"Capped loop trim and exact 2 mm through-hole at three depths, plus actual sliced hole passed.\n";

    // A reference instance may be rotated, mirrored and nonuniformly scaled;
    // physical diameter and depth must still be exact, in any drilling axis.
    Model transformed;auto* rotated=transformed.add_object("Transformed earring","",make_cube(20,30,3));
    auto* reference=rotated->add_instance(Vec3d(80,80,0),Vec3d(1.5,.75,2),Vec3d(.3,.2,.7),Vec3d(-1,1,1));
    auto* v=rotated->volumes.front();v->set_offset(Vec3d(2,-3,1));
    const auto ref_id=std::to_string(reference->id().id);
    const auto world=reference->get_matrix()*v->get_matrix();
    const Vec3d local_center=v->mesh().bounding_box().center();
    const Vec3d center=world*local_center;
    const Vec3d axis=(world.linear()*Vec3d::UnitZ()).normalized();
    auto primitive=prepare_primitive(*rotated,ref_id,"cylinder",true,center,Vec3d(2.4,2.4,30),axis);
    auto* negative=add(*rotated,std::move(primitive));
    const auto frame=geometry_frame(center,axis);
    auto mesh=negative->mesh();mesh.transform(frame.inverse()*reference->get_matrix()*negative->get_matrix(),true);
    assert((mesh.bounding_box().size()-Vec3d(2.4,2.4,30)).norm()<1e-4);
    const auto section=model_section(*rotated,ref_id,center,axis);
    assert(section.size()==1 && section.front().holes.size()==1);
    const auto saved_matrix=reference->get_matrix(),saved_part=v->get_matrix();
    const auto before_box=v->mesh().transformed_bounding_box(world);
    auto trim=prepare_plane_cut(*rotated,ref_id,{std::to_string(v->id().id)},world*(local_center+Vec3d(0,10,0)),world.linear().inverse().transpose()*Vec3d::UnitY(),false);
    trim.apply(*rotated);
    assert(reference->get_matrix().isApprox(saved_matrix) && v->get_matrix().isApprox(saved_part));
    assert(v->mesh().transformed_bounding_box(world).size().norm()<before_box.size().norm());
    std::cout<<"Rotated/mirrored/scaled instance and part coordinates, angled cut and physical hole dimensions passed.\n";

    // Part removal includes negatives and preserves unrelated meshes/paint.
    auto* spare=add(*object,prepare_primitive(*object,instance_id,"box",false,Vec3d(70,70,2),Vec3d(3,4,2),Vec3d::UnitZ()));
    const auto spare_id=std::to_string(spare->id().id);const auto base_ptr=part->mesh_ptr();
    const auto remove=removable_parts(*object,{spare_id});for(auto i:remove)object->delete_volume(i);
    assert(part->mesh_ptr()==base_ptr && part->config.extruder()==1);
    const auto n=object->volumes.size();
    rejects([&]{removable_parts(*object,{part_id});});
    rejects([&]{removable_parts(*object,{"missing"});});
    rejects([&]{removable_parts(*object,{part_id,part_id});});
    rejects([&]{prepare_plane_cut(*object,instance_id,{part_id},Vec3d(0,0,0),Vec3d::Zero(),true);});
    rejects([&]{prepare_plane_cut(*object,instance_id,{part_id},Vec3d(0,200,0),Vec3d::UnitY(),false);});
    rejects([&]{prepare_plane_cut(*object,instance_id,{part_id},Vec3d(0,200,0),Vec3d::UnitY(),true);});
    rejects([&]{prepare_primitive(*object,instance_id,"cylinder",true,Vec3d(70,87,1.5),Vec3d(2,3,5),Vec3d::UnitZ());});
    rejects([&]{prepare_primitive(*object,instance_id,"box",true,Vec3d(400,400,0),Vec3d(3,3,5),Vec3d::UnitZ());});
    rejects([&]{prepare_primitive(*object,"stale","sphere",false,Vec3d::Zero(),Vec3d(3,3,3),Vec3d::UnitZ());});
    assert(object->volumes.size()==n && part->mesh_ptr()==base_ptr);
    std::cout<<"Printable primitive addition, precise part deletion, unchanged unrelated parts and no-mutation validation failures passed.\n";
    Model painted;auto* painted_object=painted.add_object("Painted part","",make_cube(10,10,4));
    auto* painted_instance=painted_object->add_instance();auto* painted_part=painted_object->volumes.front();
    TriangleSelector paint(painted_part->mesh());
    for(size_t f=0;f<painted_part->mesh().facets_count();++f)paint.set_facet(f,EnforcerBlockerType::ENFORCER);
    painted_part->mmu_segmentation_facets.set(paint);painted_part->supported_facets.set(paint);
    auto* untouched=painted_object->add_volume(make_cube(1,1,1));untouched->mmu_segmentation_facets.set(paint);
    const auto untouched_mesh=untouched->mesh_ptr();const auto untouched_paint=untouched->mmu_segmentation_facets.get_data();
    auto painted_cut=prepare_plane_cut(*painted_object,std::to_string(painted_instance->id().id),{std::to_string(painted_part->id().id)},Vec3d(0,7,0),Vec3d::UnitY(),false);
    painted_cut.apply(*painted_object);
    assert(!painted_part->mmu_segmentation_facets.empty() && !painted_part->supported_facets.empty());
    assert(untouched->mesh_ptr()==untouched_mesh && untouched->mmu_segmentation_facets.get_data()==untouched_paint);
    std::cout<<"Cut-surface paint reprojection and unchanged unrelated mesh/paint passed.\n";
    Model separate;auto* assembly=separate.add_object("Separate loop","",make_cube(20,20,2));
    auto* assembly_instance=assembly->add_instance();const auto assembly_id=std::to_string(assembly_instance->id().id);
    auto* base=assembly->volumes.front();const auto base_mesh=base->mesh_ptr();
    auto* loop=add(*assembly,prepare_primitive(*assembly,assembly_id,"box",false,Vec3d(10,23,1),Vec3d(4,6,2),Vec3d::UnitZ()));
    auto separate_cut=prepare_plane_cut(*assembly,assembly_id,{std::to_string(base->id().id),std::to_string(loop->id().id)},Vec3d(0,20,0),Vec3d::UnitY(),false);
    separate_cut.apply(*assembly);assert(assembly->volumes.size()==1 && base->mesh_ptr()==base_mesh);
    auto positive=prepare_plane_cut(*assembly,assembly_id,{std::to_string(base->id().id)},Vec3d(0,5,0),Vec3d::UnitY(),true);
    positive.apply(*assembly);
    // Native removal folds the last part's transform into its instances.
    // Physical placement is preserved even though object-space bounds change.
    assert(std::abs(base->mesh().transformed_bounding_box(geometry_instance(*assembly,assembly_id)*base->get_matrix()).min.y()-5)<.001);
    auto* ball=add(*assembly,prepare_primitive(*assembly,assembly_id,"sphere",false,Vec3d(10,12,1),Vec3d(3,3,3),Vec3d::UnitZ()));
    auto* subtract=add(*assembly,prepare_primitive(*assembly,assembly_id,"box",true,Vec3d(10,12,1),Vec3d(1,1,4),Vec3d::UnitZ()));
    for(auto index:removable_parts(*assembly,{std::to_string(ball->id().id),std::to_string(subtract->id().id)}))assembly->delete_volume(index);
    assert(assembly->volumes.size()==1 && assembly->volumes.front()==base);
    std::cout<<"Whole-part plane removal, positive-side retention, sphere/box additions and negative-part deletion passed.\n";
    for(int f=1;f<argc;++f) {
        DynamicPrintConfig saved_config;auto saved=read_source_model(argv[f],saved_config);
        assert(saved.objects.size()==1);auto* earring=saved.objects.front();
        assert(earring->volumes.size()==2 && earring->volumes.back()->is_negative_volume());
        const auto saved_id=std::to_string(earring->instances.front()->id().id);
        const auto bounds=earring->bounding_box();
        assert((bounds.size()-Vec3d(20,30,3)).norm()<.001);
        for(double z:{bounds.min.z()+.1,bounds.center().z(),bounds.max.z()-.1}) {
            const auto section=model_section(*earring,saved_id,Vec3d(0,0,z),Vec3d::UnitZ());
            assert(section.size()==1 && section.front().holes.size()==1);
            assert(std::abs(area_mm2(section)-(600-PI))<.005);
            const auto hole=section.front().holes.front().bounding_box();
            assert((Vec2d(unscale<double>(hole.size().x()),unscale<double>(hole.size().y()))-Vec2d(2,2)).norm()<.001);
            assert(std::abs(unscale<double>(hole.center().y())-(bounds.max.y()-3))<.001);
            assert(std::abs(unscale<double>(hole.center().x())-bounds.center().x())<.001);
        }
        std::cout<<"Saved live assistant project: correct body, centered 2 mm through-hole 3 mm below edge, and negative-volume persistence passed.\n";
    }
}
