#ifndef slic3r_StudioAssistantTransforms_hpp_
#define slic3r_StudioAssistantTransforms_hpp_
#include "StudioAssistantGeometry.hpp"

namespace Slic3r { namespace GUI { namespace Assistant {
struct PreparedItemTransform {
    ModelObject* object{nullptr};ModelInstance* instance{nullptr};Transform3d instance_matrix{Transform3d::Identity()};
    std::vector<std::pair<ModelVolume*,Transform3d>> parts;
    void apply(){if(instance)instance->set_transformation(Geometry::Transformation(instance_matrix));for(auto& part:parts)part.first->set_transformation(Geometry::Transformation(part.second));object->invalidate_bounding_box();}
};
inline PreparedItemTransform prepare_item_transform(ModelObject& object,const std::string& instance_id,
    const std::vector<std::string>& part_ids,const std::string& kind,const Vec3d& values=Vec3d::Zero(),
    const std::optional<Vec3d>& pivot_override=std::nullopt)
{
    const Transform3d world=geometry_instance(object,instance_id);geometry_vector(values);if(pivot_override)geometry_vector(*pivot_override);
    ModelInstance* instance=nullptr;for(auto* i:object.instances)if(std::to_string(i->id().id)==instance_id)instance=i;
    std::vector<ModelVolume*> parts;std::set<std::string> seen;
    if(part_ids.size()>256)throw std::runtime_error("Transform at most 256 explicit parts at a time.");
    for(const auto& id:part_ids){if(!seen.insert(id).second)throw std::runtime_error("Choose each part once.");ModelVolume* part=nullptr;for(auto* v:object.volumes)if(std::to_string(v->id().id)==id)part=v;if(!part)throw std::runtime_error("A part is stale. Inspect again.");parts.push_back(part);}
    if(!parts.empty())geometry_editable(object);
    BoundingBoxf3 bounds;for(auto* part:object.volumes)if(parts.empty()?part->is_model_part():std::find(parts.begin(),parts.end(),part)!=parts.end())bounds.merge(part->mesh().transformed_bounding_box(world*part->get_matrix()));
    if(!bounds.defined)throw std::runtime_error("No printable geometry is available to transform.");
    const Vec3d pivot=pivot_override.value_or(bounds.center());Transform3d change=Transform3d::Identity();
    if(kind=="translate")change.translation()=values;
    else if(kind=="bed")change.translation().z()=-bounds.min.z();
    else {
        if(kind=="rotate")change.linear()=(Eigen::AngleAxisd(values.z()*PI/180.,Vec3d::UnitZ())*Eigen::AngleAxisd(values.y()*PI/180.,Vec3d::UnitY())*Eigen::AngleAxisd(values.x()*PI/180.,Vec3d::UnitX())).toRotationMatrix();
        else if(kind=="face") {if(values.norm()<1e-9)throw std::runtime_error("Use a measured outward face normal.");change.linear()=Eigen::Quaterniond::FromTwoVectors(values.normalized(),Vec3d(0,0,-1)).toRotationMatrix();}
        else {
            Vec3d factors=values;if(kind=="resize"){if(bounds.size().minCoeff()<1e-6)throw std::runtime_error("A zero-size part cannot be resized.");factors=factors.cwiseQuotient(bounds.size());}
            else if(kind!="scale" && kind!="mirror")throw std::runtime_error("Unknown transform.");
            for(int n=0;n<3;++n)if(kind=="mirror"?std::abs(factors[n])!=1:factors[n]<.001 || factors[n]>1000)throw std::runtime_error("Use positive scale factors or +/-1 mirror axes.");change.linear()=factors.asDiagonal();
        }
        change.translation()=pivot-change.linear()*pivot;
    }
    if(kind=="face") {BoundingBoxf3 placed;for(auto* part:object.volumes)if(part->is_model_part())placed.merge(part->mesh().transformed_bounding_box(change*world*part->get_matrix()));change.translation().z()-=placed.min.z();}
    PreparedItemTransform result;result.object=&object;
    if(parts.empty()){result.instance=instance;result.instance_matrix=change*world;}
    else for(auto* part:parts)result.parts.emplace_back(part,world.inverse()*change*world*part->get_matrix());
    return result;
}
}}}
#endif
