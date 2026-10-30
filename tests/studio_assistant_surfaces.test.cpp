#include "slic3r/GUI/StudioAssistantSurfaces.hpp"
#include "slic3r/GUI/StudioAssistantSurfaceView.hpp"
#include "slic3r/GUI/StudioAssistantSlicing.hpp"
#include "slic3r/GUI/StudioAssistantMeshEdits.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_test_paths.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
using namespace Slic3r;
using namespace Slic3r::GUI::Assistant;
template<class F> void rejects(F&& fn){bool rejected=false;try{fn();}catch(...){rejected=true;}assert(rejected);}
double painted_area(const ModelVolume& part,const FacetsAnnotation& annotation,EnforcerBlockerType state){
    const auto mesh=annotation.get_facets_strict(part,state);double area=0;
    for(const auto& f:mesh.indices)area+=(mesh.vertices[f[1]]-mesh.vertices[f[0]]).cross(mesh.vertices[f[2]]-mesh.vertices[f[0]]).norm()/2;
    return area;
}
int main(){
    configure_assistant_test_paths();
    Model model;auto* object=model.add_object("Measured box","",TriangleMesh(its_make_cube(20,20,3)));auto* instance=object->add_instance();instance->set_offset(Vec3d(60,70,6));
    auto* part=object->volumes.front();const Transform3d world=instance->get_matrix()*part->get_matrix();const auto patches=surface_patches(*part,world);assert(patches.size()==6);
    size_t bottom=0,top=0;double area=0;
    for(size_t i=0;i<patches.size();++i){area+=patches[i].area;if(patches[i].normal.z()<-.99)bottom=i;if(patches[i].normal.z()>.99)top=i;}
    assert(std::abs(area-1040)<.001);assert(std::abs(patches[bottom].area-400)<.001);assert(std::abs(patches[bottom].center.z()-part->mesh().transformed_bounding_box(world).min.z())<.001);
    std::map<int,int> labels;for(size_t i=0;i<patches.size();++i)for(int f:patches[i].faces)labels[f]=int(i);
    auto map_top=render_surface_map({{part,world,labels}},"top");assert(map_top.visible_pixels[int(top)]>500000);assert(map_top.visible_pixels[int(bottom)]==0);
    auto map_bottom=render_surface_map({{part,world,labels}},"bottom");assert(map_bottom.visible_pixels[int(bottom)]>500000);assert(map_bottom.visible_pixels[int(top)]==0);
    const auto label_anchor=map_bottom.anchors.at(int(bottom));assert(map_bottom.labels[label_anchor.second*768+label_anchor.first]==int(bottom));
    auto piece_map=render_surface_map({{part,world,{},9}},"top");assert(piece_map.visible_pixels[9]>500000);const auto piece_anchor=piece_map.anchors.at(9);assert(piece_map.labels[piece_anchor.second*768+piece_anchor.first]==9);
    Transform3d raised=world;raised.translation().z()+=5;auto occluded=render_surface_map({{part,world,{},9},{part,raised,{},7}},"top");assert(occluded.visible_pixels[9]==0 && occluded.visible_pixels[7]>500000);
    // Simulate a corrupt imported coordinate without invoking unrelated native
    // repair/hull code. Both readers must reject it before raster integer casts.
    auto& raw_vertex=const_cast<TriangleMesh&>(part->mesh()).its.vertices.front();const Vec3f saved_vertex=raw_vertex;raw_vertex.x()=std::numeric_limits<float>::quiet_NaN();
    rejects([&]{surface_patches(*part,world);});rejects([&]{render_surface_map({{part,world,{},9}},"top");});raw_vertex=saved_vertex;
    auto paint=prepared_surface_paint(*part,part->supported_facets,patches[bottom].faces,1);part->supported_facets.assign(std::move(paint.supported));
    assert(std::abs(painted_area(*part,part->supported_facets,EnforcerBlockerType::ENFORCER)-400)<.001);
    const auto support=part->supported_facets.get_data();
    auto color=prepared_surface_paint(*part,part->mmu_segmentation_facets,patches[top].faces,2);part->mmu_segmentation_facets.assign(std::move(color.supported));
    assert(part->supported_facets.get_data()==support);assert(std::abs(painted_area(*part,part->mmu_segmentation_facets,EnforcerBlockerType::Extruder2)-400)<.001);
    auto brush=prepared_surface_brush(*part,part->seam_facets,world,patches[bottom].faces,1,patches[bottom].center,1.5);
    assert(!brush.limited);assert(brush.touched>0);part->seam_facets.assign(std::move(brush.paint.supported));
    const double brush_area=painted_area(*part,part->seam_facets,EnforcerBlockerType::ENFORCER);assert(brush_area>7 && brush_area<8.2);
    assert(part->supported_facets.get_data()==support);
    const auto color_before=part->mmu_segmentation_facets.get_data();
    part->seam_facets.reset();assert(part->mmu_segmentation_facets.get_data()==color_before);
    rejects([&]{prepared_surface_brush(*part,part->supported_facets,world,patches[bottom].faces,1,Vec3d(0,0,0),1);});
    rejects([&]{prepared_surface_paint(*part,part->supported_facets,{-1},1);});
    rejects([&]{surface_patches(*part,world,0);});
    // Physical areas/normals remain meaningful under mirrored, nonuniform transforms.
    Transform3d scaled=instance->get_matrix();scaled.linear()=Vec3d(-2,3,1).asDiagonal();
    auto physical=surface_patches(*part,scaled);bool found=false;
    for(const auto& patch:physical)if(patch.normal.z()<-.99){assert(std::abs(patch.area-2400)<.001);found=true;}assert(found);
    // Slicing consumes the same native support annotations as manual painting.
    // A narrow pillar anchors the cap to the bed while its broad underside
    // overhangs. A floating object alone is auto-dropped by Print::apply.
    auto pillar=prepare_primitive(*object,std::to_string(instance->id().id),"box",false,Vec3d(60,70,3),Vec3d(2,2,6),Vec3d::UnitZ());
    auto* anchor=object->add_volume(std::move(pillar.mesh),pillar.type);anchor->set_transformation(Geometry::Transformation(pillar.transform));object->invalidate_bounding_box();
    DynamicPrintConfig config=DynamicPrintConfig::full_print_config();config.set_key_value("enable_support",new ConfigOptionBool(true));
    config.set_key_value("support_type",new ConfigOptionEnum<SupportType>(stNormal));
    config.set_key_value("support_on_build_plate_only",new ConfigOptionBool(true));
    config.set_key_value("layer_height",new ConfigOptionFloat(.2));config.set_key_value("initial_layer_print_height",new ConfigOptionFloat(.2));
    config.set_key_value("nozzle_diameter",new ConfigOptionFloats({.4}));
    Print print;print.set_status_silent();print.apply(model,config);print.process();
    assert(!print.objects().empty());assert(!print.objects().front()->support_layers().empty());
    const auto scene=collect_slice_scene(print,1);assert(!scene.solids.empty() && !scene.support_paths.empty());
    const auto physical_center=scene.solids.front().contour.bounding_box().center();assert(std::abs(unscale<double>(physical_center.x())-60)<.01 && std::abs(unscale<double>(physical_center.y())-70)<.01);
    assert(!scene.model_paths.empty() && !scene.print_heights.empty());
    auto limited=collect_slice_scene(print,1,{},0);assert(limited.paths_truncated && !limited.solids.empty());
    assert(!collect_slice_scene(print,1,"missing-object").bounds.defined);rejects([&]{collect_slice_scene(print,-1);});
    // A watertight unpainted STL for the separate live assistant trial. STL
    // carries no process settings or annotations, so opening it uses the app's
    // established profiles and begins with genuinely missing support paint.
    auto joined=prepare_mesh_boolean(*object,std::to_string(instance->id().id),*part,{anchor},"union");
    part->commit_mesh_keep_paint(std::move(joined));object->delete_volume(1);object->invalidate_bounding_box();
    std::filesystem::create_directories(assistant_test_tmp());assert(store_stl((std::string(assistant_test_tmp())+"/Assistant Overhang.stl").c_str(),&model,true));
    std::cout<<"Assistant surface identification, native painting, clipped brush, transformed geometry, and real support slicing passed.\n";
}
