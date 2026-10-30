#ifndef slic3r_StudioAssistantSlicing_hpp_
#define slic3r_StudioAssistantSlicing_hpp_
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <stdexcept>

namespace Slic3r { namespace GUI { namespace Assistant {
struct SliceScene {
    ExPolygons solids,supports;Polylines model_paths,support_paths;BoundingBox bounds;
    std::vector<double> print_heights;size_t points{0},region_points{0};bool paths_truncated{false};
};
inline SliceScene collect_slice_scene(const Print& print,double z,const std::string& object_id={},size_t point_limit=300000,const std::string& instance_id={})
{
    if(!std::isfinite(z) || z<0 || z>100000)throw std::runtime_error("Choose a physical print height in millimeters.");SliceScene scene;
    auto append_shapes=[&](const ExPolygons& source,const Point& offset,ExPolygons& target){for(const auto& shape:source){scene.region_points+=shape.contour.points.size();for(const auto& hole:shape.holes)scene.region_points+=hole.points.size();if(scene.region_points>500000)throw std::runtime_error("This layer is too detailed for one diagram. Isolate an object or instance.");auto copy=shape;copy.translate(offset);scene.bounds.merge(copy.contour.bounding_box());target.push_back(std::move(copy));}};
    auto append_paths=[&](const ExtrusionEntityCollection& entities,const Point& offset,Polylines& target){
        if(scene.paths_truncated)return;auto paths=entities.as_polylines();for(auto& path:paths){if(scene.points+path.points.size()>point_limit){scene.paths_truncated=true;return;}scene.points+=path.points.size();path.translate(offset);scene.bounds.merge(path.bounding_box());target.push_back(std::move(path));}
    };
    for(const auto* object:print.objects()) {
        if(!object_id.empty() && std::to_string(object->model_object()->id().id)!=object_id)continue;
        for(const auto& instance:object->instances()) {
            if(!instance_id.empty() && std::to_string(instance.model_instance->id().id)!=instance_id)continue;
            for(const auto* layer:object->layers())if(z>layer->bottom_z()-1e-6 && z<=layer->print_z+1e-6) {
                append_shapes(layer->lslices,instance.shift,scene.solids);scene.print_heights.push_back(layer->print_z);
                for(const auto* region:layer->regions()){append_paths(region->perimeters,instance.shift,scene.model_paths);append_paths(region->fills,instance.shift,scene.model_paths);}break;
            }
            for(const auto* layer:object->support_layers())if(z>layer->bottom_z()-1e-6 && z<=layer->print_z+1e-6){append_shapes(layer->support_islands,instance.shift,scene.supports);append_paths(layer->support_fills,instance.shift,scene.support_paths);break;}
        }
    }
    std::sort(scene.print_heights.begin(),scene.print_heights.end());scene.print_heights.erase(std::unique(scene.print_heights.begin(),scene.print_heights.end()),scene.print_heights.end());return scene;
}
}}}
#endif
