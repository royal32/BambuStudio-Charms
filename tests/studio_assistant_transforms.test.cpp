#include "slic3r/GUI/StudioAssistantTransforms.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_test_paths.hpp"
#include <cassert>
#include <iostream>
using namespace Slic3r;using namespace Slic3r::GUI::Assistant;
template<class F>void rejects(F&& fn){bool rejected=false;try{fn();}catch(...){rejected=true;}assert(rejected);}
BoundingBoxf3 physical(const ModelVolume& part,const ModelInstance& instance){return part.mesh().transformed_bounding_box(instance.get_matrix()*part.get_matrix());}
int main(){
    configure_assistant_test_paths();
    Model model;auto* object=model.add_object("Explicit transforms","",make_cube(10,20,3));auto* a=object->add_instance();auto* b=object->add_instance();
    a->set_offset(Vec3d(60,70,9));b->set_offset(Vec3d(110,120,9));a->set_rotation(Vec3d(.2,.3,.1));a->set_scaling_factor(Vec3d(1.4,.7,1.2));
    const auto aid=std::to_string(a->id().id);auto* part=object->volumes.front();const auto pid=std::to_string(part->id().id);
    const auto untouched=b->get_matrix();const auto first=physical(*part,*a);
    auto staged=prepare_item_transform(*object,aid,{},"translate",Vec3d(4,-3,2));assert(physical(*part,*a).center().isApprox(first.center()));staged.apply();
    assert(physical(*part,*a).center().isApprox(first.center()+Vec3d(4,-3,2),1e-6));assert(b->get_matrix().isApprox(untouched));
    auto resized=prepare_item_transform(*object,aid,{},"resize",Vec3d(30,40,5));resized.apply();const auto sized=physical(*part,*a);assert(sized.size().isApprox(Vec3d(30,40,5),1e-5));
    assert(b->get_matrix().isApprox(untouched));const auto center=sized.center();
    prepare_item_transform(*object,aid,{},"mirror",Vec3d(-1,1,1)).apply();assert(physical(*part,*a).center().isApprox(center));assert(physical(*part,*a).size().isApprox(Vec3d(30,40,5),1e-5));
    prepare_item_transform(*object,aid,{},"bed").apply();assert(std::abs(physical(*part,*a).min.z())<1e-5);assert(b->get_matrix().isApprox(untouched));
    // A part-level physical move is converted through the rotated/mirrored
    // reference frame once; it changes the shared part, not instance offsets.
    const auto old_part=physical(*part,*a);const auto a_pose=a->get_matrix();const auto b_part=physical(*part,*b);
    prepare_item_transform(*object,aid,{pid},"translate",Vec3d(2,3,4)).apply();assert(physical(*part,*a).center().isApprox(old_part.center()+Vec3d(2,3,4),1e-5));
    assert(a->get_matrix().isApprox(a_pose) && b->get_matrix().isApprox(untouched));assert(!physical(*part,*b).center().isApprox(b_part.center()));
    rejects([&]{prepare_item_transform(*object,aid,{pid,pid},"scale",Vec3d(2,2,2));});rejects([&]{prepare_item_transform(*object,aid,{},"scale",Vec3d(0,1,1));});
    rejects([&]{prepare_item_transform(*object,"stale-instance",{},"bed");});
    // Place a measured physical side normal down, without moving the other copy.
    const Vec3d normal=(a->get_matrix().linear()*part->get_matrix().linear()).inverse().transpose()*Vec3d::UnitX();
    prepare_item_transform(*object,aid,{},"face",normal).apply();
    const Vec3d down=((a->get_matrix().linear()*part->get_matrix().linear()).inverse().transpose()*Vec3d::UnitX()).normalized();assert(down.isApprox(Vec3d(0,0,-1),1e-5));assert(std::abs(physical(*part,*a).min.z())<1e-5);assert(b->get_matrix().isApprox(untouched));
    std::cout<<"Assistant explicit instance/part transforms, physical resize, mirrors, bed/face placement, shared geometry, and invalid inputs passed.\n";
}
