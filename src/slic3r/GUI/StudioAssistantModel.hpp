#ifndef slic3r_StudioAssistantModel_hpp_
#define slic3r_StudioAssistantModel_hpp_
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace Slic3r { namespace GUI { namespace Assistant {
inline void validate_object_settings(const DynamicPrintConfig& changes)
{
    // The slicer reads object and region options from object overrides. A
    // global-only option can be stored there, but would have no effect.
    const PrintObjectConfig object_config;
    const PrintRegionConfig region_config;
    for (const auto& key : changes.keys())
        if (!object_config.has(key) && !region_config.has(key))
            throw std::runtime_error("This setting cannot be changed for one object: " + key + ". Use global or plate scope instead.");
}
inline std::vector<ModelVolume*> part_targets(ModelObject& object,const std::vector<std::string>& ids)
{
    if (ids.empty() || ids.size()>256) throw std::runtime_error("Choose between 1 and 256 parts.");
    std::set<std::string> seen; std::vector<ModelVolume*> result;
    for (const auto& id : ids) {
        if (!seen.insert(id).second) throw std::runtime_error("A part was selected more than once.");
        ModelVolume* found=nullptr;
        for (auto* part : object.volumes) if (std::to_string(part->id().id)==id) found=part;
        if (!found) throw std::runtime_error("That part is no longer in this object. Inspect the project again.");
        if (!found->is_model_part()) throw std::runtime_error("Choose printable model parts, not negative volumes or modifiers.");
        result.push_back(found);
    }
    return result;
}
inline void validate_part_filament(const std::vector<ModelVolume*>& parts,int slot,size_t slots,bool replace_paint)
{
    if (slot<1 || size_t(slot)>slots) throw std::runtime_error("That project filament slot does not exist. Choose or add a project filament first.");
    for (const auto* part : parts) if (!replace_paint && !part->mmu_segmentation_facets.empty())
        throw std::runtime_error("A selected part has color painting that overrides its base filament. Ask whether to replace that painting for a uniform color, or leave it unchanged.");
}
inline void assign_part_filament(const std::vector<ModelVolume*>& parts,int slot,bool replace_paint)
{
    for (auto* part : parts) {
        part->config.set("extruder",slot);
        if (replace_paint) part->mmu_segmentation_facets.reset();
    }
}
// These functions modify ModelConfig's global timestamp counter and therefore
// run on Studio's UI thread (or in a standalone test process), never a worker.
inline ModelObject* copy_source_object(Model& destination,const ModelObject& source)
{
    auto* added=destination.add_object(source);
    while (added->instances.size()>1) added->delete_instance(added->instances.size()-1);
    // Match native paste: reused part GUIDs would merge imported copies when
    // the person later enters the Assembly view.
    for (auto* volume : added->volumes) if (volume->is_model_part()) volume->ensure_part_guid(true);
    return added;
}
inline Model read_source_model(const std::string& filename,DynamicPrintConfig& config)
{
    struct ImportData {
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        ~ImportData() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
    } data;
    return Model::read_from_file(filename,&config,nullptr,
        LoadStrategy::LoadModel|LoadStrategy::LoadConfig|LoadStrategy::AddDefaultInstances|LoadStrategy::Silence,&data.plates,&data.presets);
}

class RemappingSelector : public TriangleSelector {
public:
    using TriangleSelector::TriangleSelector;
    void remap(const std::map<int,int>& slots) {
        // One simultaneous permutation handles swaps and split triangles,
        // without cascading the states produced by an earlier replacement.
        for (auto& triangle : m_triangles) if (triangle.valid() && !triangle.is_split()) {
            const auto found=slots.find(int(triangle.get_state()));
            if (found!=slots.end()) triangle.set_state(EnforcerBlockerType(found->second));
        }
    }
};

inline std::map<int,int> source_filament_mapping(const ModelObject& source,const DynamicPrintConfig& source_config,
    const DynamicPrintConfig& destination,size_t slots,const std::optional<std::vector<int>>& explicit_map={})
{
    const auto* source_colors=source_config.option<ConfigOptionStrings>("filament_colour");
    const auto* target_colors=destination.option<ConfigOptionStrings>("filament_colour");
    const auto* source_types=source_config.option<ConfigOptionStrings>("filament_type");
    const auto* target_types=destination.option<ConfigOptionStrings>("filament_type");
    const std::vector<std::string> keys={"extruder","wall_filament","solid_infill_filament","sparse_infill_filament","support_filament","support_interface_filament"};
    std::set<int> used{source.config.has("extruder")?std::max(1,source.config.extruder()):1};
    auto collect=[&](const ModelConfig& config) { for (const auto& key:keys) if (config.has(key) && config.opt_int(key)>0) used.insert(config.opt_int(key)); };
    collect(source.config);
    for (const auto* volume:source.volumes) { collect(volume->config); for (int value:volume->get_extruders()) if (value>0) used.insert(value); }
    auto fold=[](std::string value) { std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));}); return value; };
    std::map<int,int> mapping;
    for (int value:used) {
        if (explicit_map) {
            if (explicit_map->size()<size_t(value)) throw std::runtime_error("Provide a destination slot for every used source filament.");
            const int target=explicit_map->at(value-1);
            if (target<1 || size_t(target)>slots) throw std::runtime_error("The mapped destination filament does not exist.");
            mapping[value]=target;
        } else {
            if (!source_colors || !target_colors || !source_types || !target_types || source_colors->values.size()<size_t(value) || source_types->values.size()<size_t(value))
                throw std::runtime_error("The source does not provide enough material/color information to map it automatically. Inspect the source and supply an explicit filament_map.");
            for (size_t target=0;target<slots && target<target_colors->values.size() && target<target_types->values.size();++target)
                if (fold(source_colors->values[value-1])==fold(target_colors->values[target]) && source_types->values[value-1]==target_types->values[target]) { mapping[value]=int(target)+1; break; }
            if (!mapping.count(value)) throw std::runtime_error("Source filament "+std::to_string(value)+" ("+source_colors->values[value-1]+", "+source_types->values[value-1]+") needs a matching project filament. Add a compatible slot with that project color, or ask the person how to map it.");
        }
    }
    return mapping;
}

inline void apply_source_filament_mapping(ModelObject& source,const std::map<int,int>& mapping)
{
    const std::vector<std::string> keys={"extruder","wall_filament","solid_infill_filament","sparse_infill_filament","support_filament","support_interface_filament"};
    auto remap=[&](ModelConfig& config) { for (const auto& key:keys) if (config.has(key) && config.opt_int(key)>0) config.set(key,mapping.at(config.opt_int(key))); };
    remap(source.config);
    if (!source.config.has("extruder") || source.config.extruder()==0) source.config.set("extruder",mapping.at(1));
    for (auto* volume:source.volumes) {
        remap(volume->config);
        if (!volume->mmu_segmentation_facets.empty()) {
            RemappingSelector selector(volume->mesh());
            selector.deserialize(volume->mmu_segmentation_facets.get_data(),false);
            selector.remap(mapping); volume->mmu_segmentation_facets.set(selector);
        }
    }
}
}}}
#endif
