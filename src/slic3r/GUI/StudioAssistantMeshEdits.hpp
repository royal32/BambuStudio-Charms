#ifndef slic3r_StudioAssistantMeshEdits_hpp_
#define slic3r_StudioAssistantMeshEdits_hpp_
#include "StudioAssistantGeometry.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"

namespace Slic3r { namespace GUI { namespace Assistant {
inline PaintKeepPrepared prepare_mesh_boolean(const ModelObject& object,const std::string& instance_id,
    const ModelVolume& primary,const std::vector<ModelVolume*>& operands,const std::string& operation,
    const std::function<bool()>& canceled={})
{
    geometry_editable(object);
    if(!primary.is_model_part() || operands.empty() || operands.size()>8 || std::find(object.volumes.begin(),object.volumes.end(),&primary)==object.volumes.end())throw std::runtime_error("Choose one printable result part and one to eight operand parts from the same object.");
    const std::map<std::string,std::string> operations={{"union","UNION"},{"subtract","A_NOT_B"},{"intersect","INTERSECTION"}};
    const auto found=operations.find(operation);if(found==operations.end())throw std::runtime_error("Choose union, subtract, or intersect.");
    // Keep the native float-mesh computation close to the result part. Large
    // multi-plate offsets otherwise spend precision on unrelated translation.
    Transform3d world=geometry_instance(object,instance_id)*primary.get_matrix();
    const Vec3d origin=primary.mesh().transformed_bounding_box(world).center();world.translation()-=origin;
    TriangleMesh result=primary.mesh();result.transform(world,true);size_t faces=result.facets_count();
    std::set<const ModelVolume*> seen;
    for(const auto* operand:operands) {
        if(!operand || operand==&primary || !seen.insert(operand).second || std::find(object.volumes.begin(),object.volumes.end(),operand)==object.volumes.end())throw std::runtime_error("Choose distinct operand parts in this object, excluding the result part.");
        faces+=operand->mesh().facets_count();if(faces>1000000)throw std::runtime_error("Simplify these parts before an assistant mesh boolean; their combined detail exceeds one million triangles.");
        if(canceled && canceled())throw std::runtime_error("Model operation canceled; no project changes applied.");
        Transform3d operand_world=geometry_instance(object,instance_id)*operand->get_matrix();operand_world.translation()-=origin;
        TriangleMesh other=operand->mesh();other.transform(operand_world,true);
        const double before=std::abs(double(its_volume(result.its)));std::vector<TriangleMesh> pieces;bool failed=false;
        MeshBoolean::mcut::make_boolean(result,other,pieces,found->second,canceled,nullptr,[&]{failed=true;});
        if(canceled && canceled())throw std::runtime_error("Model operation canceled; no project changes applied.");
        if(failed || pieces.empty())throw std::runtime_error("Studio's mesh boolean failed or produced no retained material. No project changes applied; inspect closed geometry and overlap.");
        result=std::move(pieces.front());for(size_t i=1;i<pieces.size();++i)result.merge(pieces[i]);
        if(result.empty())throw std::runtime_error("This boolean would erase the result part. No project changes applied.");
        if(operation=="subtract" && std::abs(std::abs(double(its_volume(result.its)))-before)<std::max(1e-8,before*1e-8))throw std::runtime_error("The operand did not remove measurable material. Check overlap and coordinates; no changes applied.");
    }
    its_compactify_vertices(result.its);result=TriangleMesh(std::move(result.its));result.transform(world.inverse(),true);PaintKeepPrepared paint;
    if(!primary.reproject_paint_keep(result,paint,nullptr,canceled))throw std::runtime_error("Could not preserve the result part's painting. No changes applied.");paint.mesh=std::move(result);return paint;
}
inline PaintKeepPrepared prepare_mesh_simplification(const ModelObject& object,const std::string& instance_id,
    const ModelVolume& part,size_t target_triangles,const std::function<bool()>& canceled={})
{
    geometry_editable(object);const auto count=part.mesh().facets_count();
    if(count>2000000 || target_triangles<12 || target_triangles>=count)throw std::runtime_error("Choose a lower triangle count (at least 12) for a part with at most two million triangles.");
    const Transform3d world=geometry_instance(object,instance_id)*part.get_matrix();TriangleMesh mesh=part.mesh();mesh.transform(world,true);
    its_quadric_edge_collapse(mesh.its,uint32_t(target_triangles),nullptr,[&]{if(canceled && canceled())throw std::runtime_error("Model operation canceled; no project changes applied.");});
    if(mesh.its.indices.empty() || mesh.its.indices.size()>=count)throw std::runtime_error("Studio could not reduce this mesh. No changes applied.");
    its_compactify_vertices(mesh.its);TriangleMesh simplified(std::move(mesh.its));simplified.transform(world.inverse(),true);PaintKeepPrepared paint;
    if(!part.reproject_paint_keep(simplified,paint,nullptr,canceled))throw std::runtime_error("Could not preserve this part's painting. No changes applied.");paint.mesh=std::move(simplified);return paint;
}
}}}
#endif
