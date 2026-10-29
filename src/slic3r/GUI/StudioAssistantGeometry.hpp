#ifndef slic3r_StudioAssistantGeometry_hpp_
#define slic3r_StudioAssistantGeometry_hpp_
#include "StudioAssistantModel.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <cmath>

namespace Slic3r { namespace GUI { namespace Assistant {
// Geometry tools use physical build-plate millimeters for one explicitly named
// reference instance. Store the result back in volume/object space: no native
// recenter/ensure_on_bed calls, and no loss of instance or part transforms.
inline Transform3d geometry_instance(const ModelObject& object,const std::string& id)
{
    for (const auto* instance:object.instances) if (std::to_string(instance->id().id)==id) {
        const auto matrix=instance->get_matrix();
        if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant())<1e-9)
            throw std::runtime_error("This instance has an invalid transform.");
        return matrix;
    }
    throw std::runtime_error("That reference instance no longer exists. Inspect again.");
}
inline void geometry_vector(const Vec3d& value)
{
    if (!value.allFinite() || value.cwiseAbs().maxCoeff()>100000)
        throw std::runtime_error("Use finite build-plate coordinates in millimeters.");
}
inline Transform3d geometry_frame(const Vec3d& center,const Vec3d& axis)
{
    geometry_vector(center); geometry_vector(axis);
    if (axis.norm()<1e-9) throw std::runtime_error("The plane normal or primitive axis must be nonzero.");
    Transform3d frame=Transform3d::Identity();
    frame.linear()=Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(),axis.normalized()).toRotationMatrix();
    frame.translation()=center; return frame;
}
inline void geometry_editable(const ModelObject& object)
{
    if (object.is_cut()) throw std::runtime_error("This object has cut connectors. Use Studio's cut tool to manage its joined pieces first.");
}
struct PreparedPlaneCut {
    struct Part { size_t index; bool removed; PaintKeepPrepared replacement; };
    std::vector<Part> parts;
    double removed_mm3{0};
    void apply(ModelObject& object) {
        // Reverse indices so removing a whole part does not invalidate the rest.
        for (auto i=parts.rbegin();i!=parts.rend();++i) {
            if (i->removed) object.delete_volume(i->index);
            else {
                auto* volume=object.volumes[i->index];
                volume->commit_mesh_keep_paint(std::move(i->replacement));
                // The renderer and slicer cache geometry by volume ID. Match
                // native remeshing: a rebuilt mesh must receive a fresh ID.
                volume->set_new_unique_id();
                volume->calculate_convex_hull(); volume->source={};
            }
        }
        object.invalidate_bounding_box();
    }
};
inline PreparedPlaneCut prepare_plane_cut(ModelObject& object,const std::string& instance_id,
    const std::vector<std::string>& part_ids,const Vec3d& point,const Vec3d& normal,bool keep_positive)
{
    geometry_editable(object);
    const auto instance=geometry_instance(object,instance_id);
    const auto frame=geometry_frame(point,normal);
    const auto targets=part_targets(object,part_ids);
    PreparedPlaneCut result; size_t remaining=0, faces=0;
    for (size_t index=0;index<object.volumes.size();++index) {
        auto* volume=object.volumes[index];
        if (!volume->is_model_part()) continue;
        ++remaining;
        if (std::find(targets.begin(),targets.end(),volume)==targets.end()) continue;
        faces+=volume->mesh().facets_count();
        if (faces>2000000) throw std::runtime_error("These pieces are too detailed for an assistant cut. Simplify them or use Studio's cut tool.");
        const Transform3d to_cut=frame.inverse()*instance*volume->get_matrix();
        TriangleMesh transformed=volume->mesh(); transformed.transform(to_cut,true);
        const auto bounds=transformed.bounding_box();
        if ((keep_positive && bounds.min.z()>=-1e-6) || (!keep_positive && bounds.max.z()<=1e-6)) continue;
        const double before=std::abs(double(its_volume(transformed.its)));
        PreparedPlaneCut::Part part; part.index=index;
        part.removed=keep_positive?bounds.max.z()<=0:bounds.min.z()>=0;
        if (part.removed) { --remaining; result.removed_mm3+=before; }
        else {
            indexed_triangle_set kept;
            cut_mesh(transformed.its,0,keep_positive?&kept:nullptr,keep_positive?nullptr:&kept);
            its_compactify_vertices(kept);
            TriangleMesh mesh(std::move(kept));
            if (mesh.empty()) throw std::runtime_error("The cut did not produce a usable piece. Nothing was changed.");
            result.removed_mm3+=std::max(0.,before-std::abs(double(its_volume(mesh.its))));
            mesh.transform(to_cut.inverse(),true);
            // Existing native reprojection keeps all four paint layers on the
            // retained surface. Newly capped faces have approximate paint.
            if (!volume->reproject_paint_keep(mesh,part.replacement)) throw std::runtime_error("Cut cancelled.");
            part.replacement.mesh=std::move(mesh);
        }
        result.parts.push_back(std::move(part));
    }
    if (!remaining) throw std::runtime_error("This cut would remove every printable piece. Nothing was changed.");
    if (result.parts.empty() || result.removed_mm3<1e-6) throw std::runtime_error("This plane removes no material from the chosen pieces. Check its coordinates and retained side.");
    return result;
}
struct PreparedPrimitive { TriangleMesh mesh; Transform3d transform; ModelVolumeType type; };
inline PreparedPrimitive prepare_primitive(const ModelObject& object,const std::string& instance_id,
    const std::string& shape,bool negative,const Vec3d& center,const Vec3d& size,const Vec3d& axis)
{
    geometry_editable(object); geometry_vector(size);
    if (size.minCoeff()<.05 || size.maxCoeff()>1000) throw std::runtime_error("Primitive dimensions must be between 0.05 and 1000 mm.");
    PreparedPrimitive result;
    if (shape=="box") result.mesh=make_cube(size.x(),size.y(),size.z());
    else if (shape=="cylinder") {
        if (std::abs(size.x()-size.y())>1e-6) throw std::runtime_error("For a round cylinder, X and Y must both equal the diameter.");
        result.mesh=make_cylinder(size.x()/2,size.z());
    } else if (shape=="sphere") {
        if ((size.array()-size.x()).abs().maxCoeff()>1e-6) throw std::runtime_error("All sphere dimensions must equal its diameter.");
        result.mesh=make_sphere(size.x()/2);
    } else throw std::runtime_error("Choose a box, cylinder, or sphere.");
    // Constructors use different origins. Center their actual bounds once.
    result.mesh.translate((-result.mesh.bounding_box().center()).cast<float>().eval());
    const auto instance=geometry_instance(object,instance_id);
    const auto frame=geometry_frame(center,axis);
    result.transform=instance.inverse()*frame;
    result.type=negative?ModelVolumeType::NEGATIVE_VOLUME:ModelVolumeType::MODEL_PART;
    if (negative) {
        BoundingBoxf3 printable;
        for (const auto* volume:object.volumes) if (volume->is_model_part())
            printable.merge(volume->mesh().transformed_bounding_box(instance*volume->get_matrix()));
        const auto cut=result.mesh.transformed_bounding_box(frame);
        if (!printable.defined || (cut.max.array()<=printable.min.array()).any() || (cut.min.array()>=printable.max.array()).any())
            throw std::runtime_error("The negative part misses the printable object's bounds. Check its position and depth.");
    }
    return result;
}
inline std::vector<size_t> removable_parts(const ModelObject& object,const std::vector<std::string>& ids)
{
    geometry_editable(object);
    if (ids.empty() || ids.size()>256) throw std::runtime_error("Choose between 1 and 256 parts to remove.");
    std::set<std::string> seen; std::vector<size_t> result; size_t remaining=0;
    for (const auto* part:object.volumes) if (part->is_model_part()) ++remaining;
    for (const auto& id:ids) {
        if (!seen.insert(id).second) throw std::runtime_error("A part was selected more than once.");
        bool found=false;
        for (size_t i=0;i<object.volumes.size();++i) if (std::to_string(object.volumes[i]->id().id)==id) {
            if (object.volumes[i]->is_model_part()) --remaining;
            result.push_back(i); found=true; break;
        }
        if (!found) throw std::runtime_error("That part no longer exists. Inspect again.");
    }
    if (!remaining) throw std::runtime_error("Keep at least one printable part. To remove the whole object, use delete_selected.");
    std::sort(result.rbegin(),result.rend()); return result;
}
inline ExPolygons model_section(const ModelObject& object,const std::string& instance_id,const Vec3d& point,const Vec3d& normal)
{
    const auto instance=geometry_instance(object,instance_id);
    const auto frame=geometry_frame(point,normal);
    ExPolygons positive,negative; size_t faces=0;
    for (const auto* volume:object.volumes) {
        if (!volume->is_model_part() && !volume->is_negative_volume()) continue;
        faces+=volume->mesh().facets_count();
        if (faces>2000000) throw std::runtime_error("This object is too detailed for an assistant section. Use Studio's layer Preview.");
        MeshSlicingParams params; params.trafo=frame.inverse()*instance*volume->get_matrix();
        auto rings=union_ex(slice_mesh(volume->mesh().its,0.f,params),ClipperLib::pftEvenOdd);
        auto& result=volume->is_negative_volume()?negative:positive;
        result=union_ex(result,rings);
    }
    return diff_ex(positive,negative);
}
}}}
#endif
