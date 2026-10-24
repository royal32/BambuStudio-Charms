#ifndef slic3r_GUI_PlateVectorGeometry_hpp_
#define slic3r_GUI_PlateVectorGeometry_hpp_
#include "PlateVectorPreview.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include <map>
#include <set>

namespace Slic3r { namespace GUI {
// Flat, face-down artwork only. It is never used to generate print toolpaths.
// Empty means that PNG is the honest representation for this plate.
inline nlohmann::json generate_plate_vector_geometry(const Model& model,const DynamicPrintConfig& config,
    const std::vector<std::pair<int,int>>& instances,int plate_index,const std::set<int>& used_filaments)
{
    using Json=nlohmann::json;
    const auto* colors=config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || instances.empty()) return {};
    std::map<int,ExPolygons> regions;
    std::set<std::pair<int,int>> seen;
    for (const auto& selected:instances) {
        if (!seen.insert(selected).second || selected.first<0 || size_t(selected.first)>=model.objects.size()) return {};
        const auto& object=*model.objects[selected.first];
        if (selected.second<0 || size_t(selected.second)>=object.instances.size()) return {};
        const auto& instance=*object.instances[selected.second];
        if (!instance.printable || !object.printable) continue;
        BoundingBoxf3 bounds;
        for (const auto* volume:object.volumes) {
            if (!volume->printable()) continue;
            if (!volume->mmu_segmentation_facets.empty() || volume->is_modifier()) return {};
            if (volume->is_model_part()) bounds.merge(volume->mesh().transformed_bounding_box(instance.get_matrix()*volume->get_matrix()));
        }
        if (!bounds.defined) continue;
        const auto size=bounds.size();
        // A face section is misleading for tall sculptures or near-vertical
        // pieces. Keep their normal renderer instead of inventing flat art.
        if (size.z()<=0 || size.z()>25 || size.z()>.25*std::min(size.x(),size.y())) return {};
        const double bottom=std::max(0.,bounds.min.z());
        if (bounds.max.z()<=bottom) continue;
        const float z=float(bottom+std::min(.1,(bounds.max.z()-bottom)/3));
        std::map<int,ExPolygons> current;
        ExPolygons cuts;
        for (const auto* volume:object.volumes) {
            if (!volume->printable()) continue;
            if (!volume->is_model_part() && !volume->is_negative_volume()) continue;
            MeshSlicingParams params;
            params.trafo=instance.get_matrix()*volume->get_matrix();
            auto shape=union_ex(slice_mesh(volume->mesh().its,z,params),ClipperLib::pftEvenOdd);
            if (shape.empty()) continue;
            if (volume->is_negative_volume()) { cuts=union_ex(cuts,shape);continue; }
            const int filament=std::max(1,volume->extruder_id())-1;
            if (filament>=int(colors->values.size()) || !used_filaments.count(filament)) return {};
            // Region-specific material overrides cannot be represented by a
            // single part-to-slot map. Preserve the PNG in that case too.
            for (const auto* key:{"wall_filament","solid_infill_filament","sparse_infill_filament"}) {
                const ConfigOption* option=volume->config.option(key);
                if (!option) option=object.config.option(key);
                if (!option) option=config.option(key);
                if (option && option->getInt()>0 && option->getInt()!=filament+1) return {};
            }
            // Match prepared-part priority: later parts carve earlier ones.
            for (auto& old:current) old.second=diff_ex(old.second,shape);
            current[filament]=union_ex(current[filament],shape);
        }
        for (auto& item:current) regions[item.first]=union_ex(regions[item.first],diff_ex(item.second,cuts));
    }
    BoundingBox bounds;
    for (const auto& item:regions) for (const auto& polygon:item.second) bounds.merge(polygon.contour.bounding_box());
    if (!bounds.defined || bounds.size().x()<=0 || bounds.size().y()<=0) return {};
    Json result={{"version",2},{"plate_index",plate_index},{"width",unscale<double>(bounds.size().x())},
        {"height",unscale<double>(bounds.size().y())},{"print_sha256",std::string(64,'0')},{"view","bottom"},{"regions",Json::array()}};
    size_t count=0;
    for (const auto& item:regions) {
        Json contours=Json::array();
        auto add=[&](const Polygon& polygon) {
            Json ring=Json::array();
            for (const auto& point:polygon.points) {
                if (++count>200000) throw std::runtime_error("Vector preview exceeds point limit");
                ring.push_back({unscale<double>(bounds.max.x()-point.x()),unscale<double>(bounds.max.y()-point.y())});
            }
            if (ring.size()>=3) contours.push_back(std::move(ring));
        };
        for (const auto& polygon:item.second) {
            add(polygon.contour);for (const auto& hole:polygon.holes) add(hole);
        }
        if (!contours.empty()) result["regions"].push_back({{"filament",item.first},{"color",colors->values[item.first].substr(0,7)},{"contours",contours}});
    }
    if (result["regions"].empty()) return {};
    PlateVectorPreview::parse(result);
    return result;
}
}}
#endif
