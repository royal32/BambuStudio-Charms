#include "slic3r/GUI/StudioAssistantMeshEdits.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_test_paths.hpp"
#include <cassert>
#include <iostream>
using namespace Slic3r;using namespace Slic3r::GUI::Assistant;
template<class F>void rejects(F&& fn){bool rejected=false;try{fn();}catch(...){rejected=true;}assert(rejected);}
ModelVolume* append(ModelObject& object,PreparedPrimitive&& p){auto* part=object.add_volume(std::move(p.mesh),p.type);part->set_transformation(Geometry::Transformation(p.transform));object.invalidate_bounding_box();return part;}
int main(){
    configure_assistant_test_paths();
    Model model;auto* object=model.add_object("Boolean body","",make_cube(20,20,3));auto* instance=object->add_instance();instance->set_offset(Vec3d(60,60,3));
    const auto iid=std::to_string(instance->id().id);auto* primary=object->volumes.front();
    const auto world=instance->get_matrix()*primary->get_matrix();const auto bounds=primary->mesh().transformed_bounding_box(world);
    auto* drill=append(*object,prepare_primitive(*object,iid,"cylinder",true,bounds.center(),Vec3d(2,2,8),Vec3d::UnitZ()));
    const double before=std::abs(its_volume(primary->mesh().its));const auto original_id=primary->id();
    std::cerr<<"Round hole in an untransformed body\n";
    auto result=prepare_mesh_boolean(*object,iid,*primary,{drill},"subtract");
    assert(primary->id()==original_id && primary->mesh().facets_count()==12 && object->volumes.size()==2);
    std::cerr<<"Boolean mesh volumes before="<<before<<" after="<<std::abs(its_volume(result.mesh.its))<<" drill="<<its_volume(drill->mesh().its)<<"\n";
    assert(std::abs(std::abs(its_volume(result.mesh.its))-(before-PI*3))<.08);
    primary->commit_mesh_keep_paint(std::move(result));primary->set_new_unique_id();object->delete_volume(1);object->invalidate_bounding_box();
    assert(primary->mesh().transformed_bounding_box(world).size().isApprox(bounds.size(),1e-6));
    auto section=model_section(*object,iid,bounds.center(),Vec3d::UnitZ());assert(section.size()==1 && section.front().holes.size()==1);
    const auto hole=section.front().holes.front().bounding_box();assert(std::abs(unscale<double>(hole.size().x())-2)<.02);
    // Operands that miss cannot masquerade as a successful cut.
    auto* miss=append(*object,prepare_primitive(*object,iid,"box",false,bounds.center()+Vec3d(100,0,0),Vec3d(2,2,5),Vec3d::UnitZ()));
    rejects([&]{prepare_mesh_boolean(*object,iid,*primary,{miss},"subtract");});
    rejects([&]{prepare_mesh_boolean(*object,iid,*primary,{miss},"intersect");});
    rejects([&]{prepare_mesh_boolean(*object,iid,*primary,{miss,miss},"union");});
    rejects([&]{prepare_mesh_boolean(*object,iid,*primary,{miss},"union",[]{return true;});});
    object->delete_volume(1);
    // A drill remains physically round even with rotated, mirrored and
    // nonuniform instance transforms; its construction is in plate mm.
    Model rotated;auto* transformed=rotated.add_object("Transformed body","",make_cube(20,20,3));auto* pose=transformed->add_instance();
    pose->set_offset(Vec3d(70,70,15));pose->set_rotation(Vec3d(.25,.15,.3));pose->set_scaling_factor(Vec3d(1.2,.7,1.4));pose->set_mirror(Vec3d(-1,1,1));
    auto* body=transformed->volumes.front();const auto pose_id=std::to_string(pose->id().id);const auto center=body->mesh().transformed_bounding_box(pose->get_matrix()*body->get_matrix()).center();
    auto* cutter=append(*transformed,prepare_primitive(*transformed,pose_id,"cylinder",true,center,Vec3d(2,2,30),Vec3d::UnitZ()));
    std::cerr<<"Round hole under rotated, mirrored, nonuniform transforms\n";
    auto physical=prepare_mesh_boolean(*transformed,pose_id,*body,{cutter},"subtract");body->commit_mesh_keep_paint(std::move(physical));transformed->delete_volume(1);transformed->invalidate_bounding_box();
    const auto cut=model_section(*transformed,pose_id,center,Vec3d::UnitZ());assert(cut.size()==1 && cut.front().holes.size()==1);
    const auto round=cut.front().holes.front().bounding_box().size();assert(std::abs(unscale<double>(round.x())-2)<.02 && std::abs(unscale<double>(round.y())-2)<.02);
    // Editing later plates must retain small physical features even when the
    // project's coordinates are far from the world origin.
    Model distant;auto* far=distant.add_object("Distant body","",make_cube(20,20,3));auto* fi=far->add_instance();fi->set_offset(Vec3d(60000,-60000,7));auto* fp=far->volumes.front();const auto fid=std::to_string(fi->id().id);const auto fb=fp->mesh().transformed_bounding_box(fi->get_matrix()*fp->get_matrix());
    auto* small=append(*far,prepare_primitive(*far,fid,"cylinder",true,fb.center(),Vec3d(.5,.5,10),Vec3d::UnitZ()));auto precision=prepare_mesh_boolean(*far,fid,*fp,{small},"subtract");fp->commit_mesh_keep_paint(std::move(precision));far->delete_volume(1);far->invalidate_bounding_box();const auto fs=model_section(*far,fid,fb.center(),Vec3d::UnitZ());assert(fs.size()==1 && fs.front().holes.size()==1);const auto diameter=fs.front().holes.front().bounding_box().size();assert(std::abs(unscale<double>(diameter.x())-.5)<.002 && std::abs(unscale<double>(diameter.y())-.5)<.002);assert(far->bounding_box().size().isApprox(Vec3d(20,20,3),1e-7));
    Model detailed;auto* sphere=detailed.add_object("Detailed sphere","",make_sphere(10));auto* si=sphere->add_instance();auto* surface=sphere->volumes.front();
    const auto sphere_id=std::to_string(si->id().id);const auto count=surface->mesh().facets_count();assert(count>200);
    auto simplified=prepare_mesh_simplification(*sphere,sphere_id,*surface,200);assert(simplified.mesh.facets_count()<=220 && surface->mesh().facets_count()==count);
    assert(std::abs(its_volume(simplified.mesh.its)/its_volume(surface->mesh().its)-1)<.07);
    rejects([&]{prepare_mesh_simplification(*sphere,sphere_id,*surface,count);});
    rejects([&]{prepare_mesh_simplification(*sphere,sphere_id,*surface,200,[]{return true;});});
    std::cout<<"Assistant exact mesh booleans, physical transformed holes, staged failure/cancellation, and native simplification passed.\n";
}
