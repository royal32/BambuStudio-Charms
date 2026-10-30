#include "StudioAssistantActions.hpp"
#include "StudioAssistantSurfaces.hpp"
#include "StudioAssistantSurfaceView.hpp"
#include "StudioAssistantMeshEdits.hpp"
#include "StudioAssistantSlicing.hpp"
#include "StudioAssistantTransforms.hpp"
#include "StudioAssistantText.hpp"
#include "StudioAssistantProfiles.hpp"
#include "StudioAssistantState.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "Gizmos/GLGizmoText.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceManager.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <map>
#include <set>
#include <wx/dcmemory.h>
#include <wx/mstream.h>
#include <wx/base64.h>
#include <wx/graphics.h>
#include <wx/fontenum.h>
#include <thread>
#include <atomic>
#include <filesystem>

namespace Slic3r { namespace GUI { namespace Assistant {
namespace {
Json str(){return {{"type","string"}};}
Json num(double low,double high){return {{"type","number"},{"minimum",low},{"maximum",high}};}
Json enumeration(std::initializer_list<const char*> values){return {{"type","string"},{"enum",values}};}
Json obj(Json props,Json required){return {{"type","object"},{"properties",props},{"required",required},{"additionalProperties",false}};}
Json strings(){return {{"type","array"},{"items",str()},{"minItems",1},{"maxItems",256}};}
Json triple(){return {{"type","array"},{"items",{{"type","number"}}},{"minItems",3},{"maxItems",3}};}
Json vector_json(const Vec3d& v){return {v.x(),v.y(),v.z()};}
Vec3d vector_arg(const Json& args,const char* key){const auto v=args.at(key).get<std::vector<double>>();if(v.size()!=3)throw std::runtime_error("Use three physical coordinates.");Vec3d p(v[0],v[1],v[2]);geometry_vector(p);return p;}
ModelObject* get_object(Plater& p,const std::string& id){for(auto* o:p.model().objects)if(std::to_string(o->id().id)==id)return o;throw std::runtime_error("That object is stale. Inspect the project again.");}
ModelVolume* get_part(ModelObject& object,const std::string& id){for(auto* v:object.volumes)if(std::to_string(v->id().id)==id)return v;throw std::runtime_error("That part is stale. Inspect again.");}
ModelInstance* get_instance(ModelObject& object,const std::string& id){for(auto* i:object.instances)if(std::to_string(i->id().id)==id)return i;throw std::runtime_error("That instance is stale. Inspect again.");}
Json matrix_json(const Transform3d& matrix){Json a=Json::array();for(int r=0;r<4;++r)for(int c=0;c<4;++c)a.push_back(matrix(r,c));return a;}
void prepare_canvas(Plater& p){wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);p.select_view_3D("3D");p.exit_gizmo();}
void refresh_part(Plater& p,ModelObject& object,bool mesh=false){
    object.invalidate_bounding_box();const auto i=size_t(std::find(p.model().objects.begin(),p.model().objects.end(),&object)-p.model().objects.begin());
    p.canvas3D()->get_selection().clear();p.sidebar().obj_list()->add_volumes_to_object_in_list(i);
    p.sidebar().obj_list()->update_info_items(i);p.get_partplate_list().reset_thumbnail_assembly_view_data();
    if(mesh)p.changed_mesh(int(i));else {p.update();p.schedule_background_process();}
}
}
Json workbench_tool_definitions()
{
    Json tools=Json::array();auto add=[&](const char* name,const char* description,Json schema){tools.push_back({{"type","function"},{"name",name},{"description",description},{"parameters",schema},{"strict",false}});};
    add("inspect_surfaces","Survey connected surface patches on a printable part, with stable surface IDs, outward normals, physical area, centroids and bounds in build-plate mm. Use to target supports, seams, colors or placing a face on the bed; correlate with model views before editing. filter can restrict to downward/upward/vertical surfaces. Small patches may be omitted by limit; lower minimum_area_mm2 for tiny loops. Negative/modifier geometry is not a printable surface. Painting applies to every instance sharing the object's parts.",obj({{"object_id",str()},{"instance_id",str()},{"part_ids",strings()},{"filter",enumeration({"all","downward","upward","vertical"})},{"minimum_area_mm2",num(0,1000000)},{"limit",{{"type","integer"},{"minimum",1},{"maximum",128}}}}, {"object_id","instance_id","part_ids"}));
    add("surface_view","Render an isolated instance with numbered, depth-tested measured surface patches and a legend mapping numbers to surface IDs. Use after inspect_surfaces to identify the physical feature to paint or lay flat. Other printable parts stay gray and occlude hidden surfaces correctly. This is a topology diagram, NOT actual filament colors or sliced negative-volume subtraction; use model_view for original colors and sliced layers/sections for holes and supports. Invisible IDs are listed separately; choose another direction to see them.",obj({{"surface_ids",strings()},{"direction",enumeration({"iso","top","bottom","front","rear","left","right"})}}, {"surface_ids","direction"}));
    add("paint_surfaces","Paint measured surface IDs with support/seam enforcers, blockers, color slots or fuzzy-skin state, preserving all other surfaces and annotation layers. Inspect surfaces and view the target first. layer=color uses slot (1-based), action=erase restores base part color. Support/seam use enforce/block/erase; fuzzy uses enforce/erase. Optional center_mm plus radius_mm clips the paint to a physical spherical brush on ONLY those measured surfaces, with native triangle subdivision; omit both to paint whole patches. Does not guarantee sufficient supports: enable suitable process settings, slice and inspect actual support layers. Returns touched triangles and any brush budget limit. A limit means partial coverage: inspect and use smaller measured regions rather than claiming the whole target was painted. IDs become stale if geometry/placement changes. Undoable.",obj({{"revision",str()},{"surface_ids",strings()},{"layer",enumeration({"support","seam","color","fuzzy"})},{"action",enumeration({"enforce","block","erase","color"})},{"slot",{{"type","integer"},{"minimum",1},{"maximum",32}}},{"center_mm",triple()},{"radius_mm",num(.1,100)}}, {"revision","surface_ids","layer","action"}));
    add("clear_paint","Clear one specified annotation layer on identified printable parts without changing the mesh or other layers. Use only when the person requests clearing that painting. Undoable.",obj({{"revision",str()},{"object_id",str()},{"part_ids",strings()},{"layer",enumeration({"support","seam","color","fuzzy"})}}, {"revision","object_id","part_ids","layer"}));
    add("transform_item","Move, rotate, nonuniformly scale, mirror, or resize ONE explicit instance or specified parts. No implicit selection or bed drop. values are build-plate mm for translate, XYZ degrees for rotate, positive XYZ factors for scale, target XYZ physical bounding-box dimensions for resize, and +/-1 per axis for mirror. Pivot defaults to the target's physical bounds center; pivot_mm overrides it. Part edits affect all instances of that object; an instance edit affects only that instance. Negative/modifier parts can be moved too. Scaling in build-plate axes may change proportions; use equal factors for uniform scaling. Inspect and check resulting dimensions/fit. Undoable.",obj({{"revision",str()},{"object_id",str()},{"instance_id",str()},{"part_ids",strings()},{"kind",enumeration({"translate","rotate","scale","resize","mirror"})},{"values",triple()},{"pivot_mm",triple()}}, {"revision","object_id","instance_id","kind","values"}));
    add("place_on_bed","Move one instance vertically so its printable geometry rests on Z=0, retaining XY placement and orientation. Does not arrange or change other instances. Undoable.",obj({{"revision",str()},{"object_id",str()},{"instance_id",str()}}, {"revision","object_id","instance_id"}));
    add("place_on_face","Rotate one explicit instance so a measured surface's outward normal faces down, then drop it to Z=0. Use a geometrically/visually identified flat surface, not a guessed face. Retains its XY center and part colors; all its parts/negative volumes rotate together. Does not paint supports or guarantee support-free printing. Undoable.",obj({{"revision",str()},{"surface_id",str()}}, {"revision","surface_id"}));
    add("create_object","Create a standalone printable box, cylinder, or sphere on the current unlocked plate, including an empty project. size_mm=[XYZ dimensions]; cylinder XY are both DIAMETER, Z is length; sphere XYZ are equal diameter. axis aligns local Z; optional center_mm is physical project/build-plate space, otherwise centers on the current plate and rests on the bed. Uses an existing filament slot and native object initialization. Does not automatically arrange or add supports; check bounds/collisions and slice. For a hole/modifier inside an existing object use add_primitive instead. Undoable.",obj({{"revision",str()},{"shape",enumeration({"box","cylinder","sphere"})},{"size_mm",triple()},{"axis",triple()},{"center_mm",triple()},{"name",str()},{"slot",{{"type","integer"},{"minimum",1},{"maximum",32}}}},{"revision","shape","size_mm"}));
    add("list_fonts","Find installed native font names for lettering. A query filters names; an exact face-name query also reports its outline font collection entries/indexes. Returns names only, not local font paths. Use an installed legible default such as Arial when the person has not expressed a font preference.",obj({{"query",str()}},Json::array()));
    add("inspect_profile_settings","Read actual temperatures, cooling, flow, material labels, dimensions and other safe parameters from an installed/current filament, printer or process profile. query searches actual names/labels/descriptions. Optional name must be an actual installed profile name; otherwise filament slot (1-based, default 1) selects its assigned profile and other kinds use the current edited profile. Reports current unsaved edits when that profile is being edited. Read-only: selecting a compatible material profile uses use_profile; process edits use inspect_settings/set_settings. Credentials, custom G-code, scripts, arbitrary strings and paths are omitted.",obj({{"kind",enumeration({"printer","process","filament"})},{"query",str()},{"name",str()},{"slot",{{"type","integer"},{"minimum",1},{"maximum",32}}}},{"kind","query"}));
    add("part_map_view","Identify pieces with a numbered, depth-tested view of ONE reference instance. Every requested printable part receives one number mapped to its stable part_id/name/slot; other geometry is gray and occludes hidden pieces. Optional part_ids narrows candidates; omitted means all printable parts (at most 64 numbered pieces). Use this when appearance cannot be confidently mapped to inspected part IDs; choose a direction exposing the feature. Diagram colors identify pieces and are NOT original material colors. Negative subtraction is not represented; verify holes through sections/slices and actual appearance through model_view. Makes no project changes.",obj({{"object_id",str()},{"instance_id",str()},{"part_ids",strings()},{"direction",enumeration({"iso","top","bottom","front","rear","left","right"})}},{"object_id","instance_id","direction"}));
    add("inspect_printers","Read Studio's cached paired-printer models, names, online/connection/print state, and REPORTED AMS slot materials/colors. Sends no printer command, changes no settings, and exposes no access codes, IP addresses, serial numbers or account tokens. An opaque printer_ref is just a local reference. Cached/offline devices can include old machines; reported slot colors/materials may be placeholders or stale and do not prove what is physically loaded. Use this before asking what printers are available, and confirm uncertain physical materials through the normal print workflow.",obj(Json::object(),Json::array()));
    add("add_text","Create native editable extruded lettering as a standalone printable object, or as a printable/negative part of an existing object. Omit object_id and instance_id for standalone text; supply both to attach it. text is literal UTF-8; height_mm is the actual visible glyph bounding-box height (not points), depth_mm is physical thickness. center_mm is its physical midpoint (required when attaching; standalone text defaults to the plate center and rests on the bed); axis points out of the text face (default +Z), rotation_deg rotates in that face. A negative engraves ONLY where it intersects this object's body; thickness is not automatically the engraving depth, so place it against the measured surface and verify sections. Uses an installed outline font (default Arial), optional collection_index/bold/italic and an existing filament slot. Does not wrap text onto curved surfaces. Preserves existing parts/profiles and runs as a cancelable staged operation. Check fit, readability and actual slicing. Undoable.",obj({{"revision",str()},{"object_id",str()},{"instance_id",str()},{"text",str()},{"height_mm",num(.5,200)},{"depth_mm",num(.1,20)},{"center_mm",triple()},{"axis",triple()},{"rotation_deg",num(-3600,3600)},{"negative",{{"type","boolean"}}},{"font",str()},{"collection_index",{{"type","integer"},{"minimum",0},{"maximum",127}}},{"bold",{{"type","boolean"}}},{"italic",{{"type","boolean"}}},{"slot",{{"type","integer"},{"minimum",1},{"maximum",32}}}},{"revision","text","height_mm","depth_mm"}));
    const auto targets=Json{{"type","array"},{"minItems",1},{"maxItems",64},{"items",obj({{"object_id",str()},{"instance_id",str()}},{"object_id","instance_id"})}};
    add("select_instances","Focus explicit individual instances, rather than all copies of an object. This changes selection only. Use inspected object/instance IDs.",obj({{"targets",targets}},{"targets"}));
    add("layout_selection","Apply native center, fit-to-bed, align, or equal-spacing distribution to explicit full instances on the CURRENT unlocked plate. alignment=min/center/max, axis=x/y/z; distribution needs at least three instances and alignment needs two. Center preserves group arrangement; fit scales the group to the build volume and changes dimensions. Use arrange for collision-aware automatic placement. Does not select parts or touch copies on other plates. Undoable.",obj({{"revision",str()},{"targets",targets},{"operation",enumeration({"center","fit","align","distribute"})},{"axis",enumeration({"x","y","z"})},{"alignment",enumeration({"min","center","max"})}},{"revision","targets","operation"}));
    add("part_properties","Rename a part or convert its native type: model_part, negative, modifier, support_enforcer, support_blocker. Requires explicit part IDs; changes all instances sharing those parts. A negative subtracts only from its own object during slicing. A modifier supplies region settings without adding material; use set_settings scope=part afterward. Refuses to remove the last model-part type. Preserves geometry and placement; inspect/section/slice to verify. Undoable.",obj({{"revision",str()},{"object_id",str()},{"part_ids",strings()},{"name",str()},{"type",enumeration({"model_part","negative","modifier","support_enforcer","support_blocker"})}},{"revision","object_id","part_ids"}));
    add("instance_printable","Enable or disable printing for explicit instances, retaining them in the project and leaving other copies unchanged. Undoable.",obj({{"revision",str()},{"targets",targets},{"printable",{{"type","boolean"}}}},{"revision","targets","printable"}));
    add("detach_instances","Make selected linked instances independent objects using Studio's native separation, retaining their positions, parts, colors and painting. Use BEFORE changing geometry, painting or part colors for only one linked copy; otherwise those edits affect all copies sharing an object. An instance already independent is a harmless no-op. Inspect fresh IDs afterward. Undoable.",obj({{"revision",str()},{"targets",targets}},{"revision","targets"}));
    add("object_structure","Split disconnected shells into independently movable objects, or merge whole objects into ONE multipart object while keeping their part identities/colors and relative placements. split_objects needs one object; merge_to_parts needs at least two. For coloring internal pieces while retaining assembly, use split_parts instead. After restructuring inspect fresh object/part/instance IDs. Native undo is available.",obj({{"revision",str()},{"object_ids",strings()},{"operation",enumeration({"split_objects","merge_to_parts"})}},{"revision","object_ids","operation"}));
    add("plate_operation","Lock or unlock an identified plate using Studio's native behavior. This controls layout edits; it does not delete library files. Plate numbers are 1-based. Undoable.",obj({{"revision",str()},{"plate",{{"type","integer"},{"minimum",1},{"maximum",128}}},{"operation",enumeration({"lock","unlock"})}},{"revision","plate","operation"}));
    add("delete_plate","Delete an identified project plate through native confirmation. A populated plate can lose its contents; use only when requested and honor confirmation. Undo is available. This does not delete sliced library files. Plate numbers shift afterward; inspect again.",obj({{"revision",str()},{"plate",{{"type","integer"},{"minimum",1},{"maximum",128}}}},{"revision","plate"}));
    add("reset_overrides","Remove named process-setting overrides from an object, printable/modifier part, or plate, restoring inheritance. Does not reset other settings or a saved profile. Inspect exact keys/scope first. Missing overrides already inherit and are a harmless no-op. Undoable.",obj({{"revision",str()},{"scope",enumeration({"object","part","plate"})},{"object_id",str()},{"part_id",str()},{"plate",{{"type","integer"},{"minimum",1},{"maximum",128}}},{"keys",strings()}},{"revision","scope","keys"}));
    add("open_editor","Open Studio's native detailed editor for the selection: measure, cut, boolean, text, SVG, support/color/seam/fuzzy painting, simplify, or flatten. Opening is not completing an edit. Prefer direct modeling/painting tools when they can perform the requested operation. Some advanced interactive controls still need the person; report that precise limitation rather than saying the whole capability is unavailable.",obj({{"editor",enumeration({"measure","cut","boolean","text","svg","support","color","seam","fuzzy","simplify","flatten"})}},{"editor"}));
    add("library_export","Add the current sliced plate or all sliced plates to the shared sliced library using Studio's native saved-source/vector-preview workflow. Requires valid slicing and an editable named project; native save/folder dialogs may require the person. This writes library artifacts; use only when requested. Does not send a print.",obj({{"revision",str()},{"all_plates",{{"type","boolean"}}}},{"revision","all_plates"}));
    add("review_print","Open the normal print-review dialog for the current or all sliced plates. Requires valid sliced data. The person chooses the printer/AMS mapping and confirms the actual handoff. This tool does not confirm or send a job by itself; never claim a print started from opening the dialog.",obj({{"all_plates",{{"type","boolean"}}}},{"all_plates"}));
    add("load_model_file","Open a user-identified local editable 3MF project, or import a user-identified local 3D model without asking them to use a file chooser. path must be an existing absolute local path to a supported model, at most 512 MB. mode=project replaces the current project using native unsaved-change prompts; mode=import uses native geometry import and retains the current printer/process profiles. Native import/material prompts may still require the person. For library items prefer their source-project/library import tools. Does not fetch URLs, read arbitrary document types or execute code. Returns pending native work; inspect the resulting project/parts/materials before claiming it opened or imported correctly.",obj({{"revision",str()},{"path",str()},{"mode",enumeration({"project","import"})}},{"revision","path","mode"}));
    add("mesh_boolean","Perform a native exact mesh union, subtraction, or intersection on parts of ONE object, retaining primary_part_id as the printable result and consuming the operand parts. For different objects first merge_to_parts. All geometry is evaluated in physical coordinates; painting on the primary is reprojected (approximate on new faces), while operand-specific paint/settings are consumed. Prefer multipart assembly to preserve distinct material assignments; prefer negative volumes for editable holes. Closed, valid meshes are required. Runs in the background, is cancelable and rejects stale results; Undo is available. Verify actual shape/sections and slicing afterward, not just successful completion.",obj({{"revision",str()},{"object_id",str()},{"instance_id",str()},{"primary_part_id",str()},{"operand_part_ids",strings()},{"operation",enumeration({"union","subtract","intersect"})}},{"revision","object_id","instance_id","primary_part_id","operand_part_ids","operation"}));
    add("simplify_mesh","Reduce one part's triangles using Studio's native quadric simplifier, targeting an explicit lower triangle count. This approximates geometry and reprojects paint; it can lose small features and is not a mesh repair or a guaranteed dimensional tolerance. Use only when reduction is requested/necessary; inspect critical dimensions and holes afterward. Runs in the background, can be canceled, rejects stale results and supports Undo. Other parts and placement stay unchanged.",obj({{"revision",str()},{"object_id",str()},{"instance_id",str()},{"part_id",str()},{"target_triangles",{{"type","integer"},{"minimum",12},{"maximum",2000000}}}},{"revision","object_id","instance_id","part_id","target_triangles"}));
    add("inspect_slice","Inspect fresh native sliced data for a plate: actual object/support layer counts, support height range, instance IDs, estimated time, filament use and tool changes. Refuses stale/missing slices. Slice first, then use slice_layer_view at heights relevant to a warning or feature. A successful slice or support count alone does not prove reliable physical printing.",obj({{"plate",{{"type","integer"},{"minimum",1},{"maximum",128}}}},Json::array()));
    add("slice_all","Run Studio's native Slice All workflow for editable project plates, wait for completion, and report any printable plates still missing valid results. Does not export or send prints. Resolve warnings/errors before claiming readiness.",obj({{"revision",str()}},{"revision"}));
    add("slice_layer_view","Render actual sliced solid regions, holes, support islands, and generated extrusion paths at a physical height on a plate. Gray regions/dark paths=model; blue regions/paths=support. Optional object_id isolates that source object's instances, and instance_id isolates a particular copy; optional bounds_mm=[xmin,ymin,xmax,ymax] zooms a measured project-space area. Does not infer supports from geometry normals. Requires a fresh valid slice. Returns actual sampled print heights; different variable-height objects can use different layers. This is a top-down slice/toolpath diagram, not the original color thumbnail or a guarantee of print success.",obj({{"plate",{{"type","integer"},{"minimum",1},{"maximum",128}}},{"print_z_mm",num(0,100000)},{"object_id",str()},{"instance_id",str()},{"bounds_mm",{{"type","array"},{"items",{{"type","number"}}},{"minItems",4},{"maxItems",4}}}},{"print_z_mm"}));
    return tools;
}
bool workbench_changes_project(const std::string& name){static const std::set<std::string> names={"paint_surfaces","clear_paint","transform_item","place_on_bed","place_on_face","create_object","add_text","layout_selection","part_properties","instance_printable","detach_instances","object_structure","plate_operation","delete_plate","reset_overrides","library_export","load_model_file","mesh_boolean","simplify_mesh"};return names.count(name);}
bool workbench_requires_confirmation(const std::string& name){return name=="delete_plate";}

std::optional<Json> Actions::execute_workbench(const std::string& tool,const Json& args)
{
    auto* pointer=wxGetApp().plater();if(!pointer)throw std::runtime_error("Studio's editor is unavailable.");auto& p=*pointer;
    if(tool=="inspect_profile_settings") {
        const auto kind=args.at("kind").get<std::string>();const auto query=wxString::FromUTF8(args.at("query").get<std::string>()).Lower();if(query.empty())throw std::runtime_error("Specify a profile parameter to search for.");
        auto& bundle=*wxGetApp().preset_bundle;const PresetCollection* collection=kind=="filament"?&bundle.filaments:kind=="printer"?&bundle.printers:kind=="process"?&bundle.prints:nullptr;if(!collection)throw std::runtime_error("Choose printer, filament or process.");
        auto name=args.value("name",std::string());if(name.empty() && kind=="filament"){const int slot=args.value("slot",1);if(slot<1 || size_t(slot)>bundle.filament_presets.size())throw std::runtime_error("Choose an existing project filament slot.");name=bundle.filament_presets[slot-1];}
        const auto& edited=collection->get_edited_preset();const Preset* preset=name.empty()||name==edited.name?&edited:collection->find_preset(name,false);if(!preset)throw std::runtime_error("Choose a profile returned by list_profiles.");
        Json values=Json::array();for(const auto& key:preset->config.keys()){const auto* definition=preset->config.def()->get(key);if(!definition || !readable_profile_parameter(key,*definition))continue;const auto searchable=wxString::FromUTF8(key+" "+definition->label+" "+definition->tooltip).Lower();if(!searchable.Contains(query))continue;values.push_back({{"key",key},{"label",definition->label},{"description",definition->tooltip},{"value",preset->config.opt_serialize(key)},{"type",int(definition->type)},{"min",definition->min},{"max",definition->max},{"enum_values",definition->enum_values}});if(values.size()==50)break;}
        return Json{{"kind",kind},{"profile",preset->name},{"settings",values},{"read_only",true},{"note","These are configured profile parameters, not evidence of the actual loaded spool or physical nozzle."}};
    }
    if(tool=="inspect_printers") {
        Json printers=Json::array();auto* manager=wxGetApp().getDeviceManager();
        if(manager)for(const auto& entry:manager->get_my_machine_list())if(auto* device=entry.second) {
            Json slots=Json::array();if(auto system=device->GetFilaSystem())for(const auto& ams:system->GetAmsList())if(ams.second)for(const auto& tray:ams.second->GetTrays())if(tray.second)slots.push_back({{"ams_id",ams.first},{"slot_id",tray.first},{"label",tray.second->tray_id_name},{"reported_present",tray.second->is_exists},{"reported_material",tray.second->m_fila_type},{"reported_color",tray.second->color}});
            printers.push_back({{"printer_ref",base64url(sha256(device->get_dev_id())).substr(0,16)},{"name",device->get_dev_name()},{"model",device->get_printer_type_display_str().ToUTF8().data()},{"online",device->is_online()},{"connected",device->is_connected()},{"reported_print_state",device->print_status},{"reported_ams_slots",slots}});
        }
        return Json{{"printers",printers},{"data_source","Studio's cached device/AMS state; no refresh or printer command was sent."},{"note","Device lists can contain old/offline machines, and reported materials/colors may not match actual loaded spools. Do not infer a physical color from placeholder white slots."}};
    }
    if(tool=="load_model_file") {
        const auto path=std::filesystem::u8path(args.at("path").get<std::string>());const auto mode=args.at("mode").get<std::string>();
        std::error_code error;if(!path.is_absolute() || !std::filesystem::is_regular_file(path,error) || error || std::filesystem::file_size(path,error)>512*1024*1024 || error)throw std::runtime_error("Choose an existing absolute local model path no larger than 512 MB.");
        auto extension=path.extension().u8string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
        const std::set<std::string> supported={".3mf",".stl",".step",".stp",".obj",".svg",".amf",".ply"};
        if(!supported.count(extension) || (mode=="project" && extension!=".3mf"))throw std::runtime_error("Use an editable .3mf for project mode, or a supported 3D model for import.");
        auto filename=path.filename().u8string();std::transform(filename.begin(),filename.end(),filename.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(filename.size()>=10 && filename.substr(filename.size()-10)==".gcode.3mf")throw std::runtime_error("Choose this sliced item's editable source project instead of its .gcode.3mf.");
        std::set<std::string> before;for(const auto* object:p.model().objects){before.insert("object:"+std::to_string(object->id().id));for(const auto* part:object->volumes)before.insert("part:"+std::to_string(part->id().id));}
        // Native loading already owns its progress/unsaved-change dialogs.
        // Do not queue it behind a timer: a timer can mistake the close/save
        // dialog for a completed load before native parsing has even started.
        const auto file=path.u8string();size_t imported=0;
        if(mode=="project"){if(p.load_project(wxString::FromUTF8(file))==wxID_CANCEL)return Json{{"cancelled",true},{"message","The person cancelled the native project workflow. Do not retry without a new request."}};}
        else imported=p.load_files(std::vector<std::string>{file},LoadStrategy::LoadModel,false).size();
        bool fresh=false;for(const auto* object:p.model().objects){if(!object->volumes.empty() && !before.count("object:"+std::to_string(object->id().id)))fresh=true;for(const auto* part:object->volumes)if(!before.count("part:"+std::to_string(part->id().id)))fresh=true;}
        auto result=inspect();result["requested_file"]=file;result["new_content_detected"]=fresh;result["imported_object_count"]=imported;
        // A project filename is assigned before parsing; neither it nor the
        // close/save return code proves that the intended contents loaded.
        if(mode=="project" || !fresh)return unconfirmed_project_open(std::move(result));
        result["instruction"]="Compare the inspected imported objects/parts and their bounds with the requested model before editing. Native imports may report skipped or invalid objects.";return result;
    }
    if(tool=="create_object") {
        auto* plate=p.get_partplate_list().get_curr_plate();if(plate->is_locked())throw std::runtime_error("Unlock this plate before adding geometry.");
        const int slot=args.value("slot",1);if(slot<1 || size_t(slot)>wxGetApp().preset_bundle->filament_presets.size())throw std::runtime_error("Choose an existing project filament slot.");
        const auto shape=args.at("shape").get<std::string>(),name=args.value("name",shape);if(name.empty() || name.size()>200 || name.find_first_of("\r\n\t")!=std::string::npos)throw std::runtime_error("Choose a short single-line model name.");
        Model staging;auto* empty=staging.add_object();auto* reference=empty->add_instance();const Vec3d axis=args.contains("axis")?vector_arg(args,"axis"):Vec3d::UnitZ();
        auto primitive=prepare_primitive(*empty,std::to_string(reference->id().id),shape,false,Vec3d::Zero(),vector_arg(args,"size_mm"),axis);primitive.mesh.transform(primitive.transform,true);
        const auto extent=primitive.mesh.bounding_box();const Vec3d center=args.contains("center_mm")?vector_arg(args,"center_mm"):Vec3d(plate->get_origin().x()+plate->get_size().x()/2,plate->get_origin().y()+plate->get_size().y()/2,extent.size().z()/2);
        prepare_canvas(p);p.take_snapshot("Assistant: create printable object");const size_t before=p.model().objects.size();p.sidebar().obj_list()->load_mesh_object(primitive.mesh,wxString::FromUTF8(name),true);
        if(p.model().objects.size()!=before+1)throw std::runtime_error("Studio did not create exactly one object. Inspect the current project before proceeding.");
        auto* created=p.model().objects.back();auto* instance=created->instances.front();const auto bounds=created->volumes.front()->mesh().transformed_bounding_box(instance->get_matrix()*created->volumes.front()->get_matrix());
        instance->set_offset(instance->get_offset()+center-bounds.center());created->config.set("extruder",slot);created->volumes.front()->config.set("extruder",slot);p.sidebar().obj_list()->sync_filament_from_model();refresh_part(p,*created,true);
        auto result=inspect();result["created_object_id"]=std::to_string(created->id().id);result["created_instance_id"]=std::to_string(instance->id().id);return result;
    }
    if(tool=="list_fonts") {
        const auto query=wxString::FromUTF8(args.value("query",std::string())).Lower();auto names=wxFontEnumerator::GetFacenames();names.Sort();Json found=Json::array(),collections=Json::array();
        for(const auto& name:names)if(query.empty() || name.Lower().Contains(query)) {
            if(found.size()<64)found.push_back(std::string(name.utf8_string()));
            if(!query.empty() && name.Lower()==query){wxFont face(wxFontInfo(10).FaceName(name));auto file=WxFontUtils::create_font_file(face);if(file){const auto variants=font_collection_names(*file);for(size_t i=0;i<variants.size();++i)collections.push_back({{"index",i},{"name",variants[i]}});}}
        }
        return Json{{"font_names",found},{"collection_faces",collections},{"note","Exact-name lookup reports collection indexes; outline availability is validated again before creating text."}};
    }
    if(tool=="mesh_boolean" || tool=="simplify_mesh" || tool=="add_text")return start_model_work(tool,args);
    if(tool=="slice_all") {
        if(p.only_gcode_mode() || p.is_gcode_3mf() || p.is_any_job_running() || p.is_background_process_slicing())throw std::runtime_error("Wait for an editable project before slicing all plates.");
        if(p.model().objects.empty())throw std::runtime_error("Add printable models first.");wxPostEvent(&p,SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL));return Json{{"pending","slice_all"}};
    }
    if(tool=="inspect_slice" || tool=="slice_layer_view") {
        auto& plates=p.get_partplate_list();const int index=args.value("plate",plates.get_curr_plate_index()+1)-1;
        if(index<0 || index>=plates.get_plate_count())throw std::runtime_error("That plate does not exist.");auto* plate=plates.get_plate(index);
        if(p.is_background_process_slicing() || p.is_any_job_running() || !plate->is_slice_result_valid() || !plate->fff_print())throw std::runtime_error("Slice this editable project's plate and wait for completion before inspecting actual layers.");
        const auto& print=*plate->fff_print();
        if(tool=="inspect_slice") {
            Json objects=Json::array();for(const auto* object:print.objects()) {
                Json instances=Json::array();for(const auto& instance:object->instances())instances.push_back(std::to_string(instance.model_instance->id().id));
                double support_min=0,support_max=0;if(!object->support_layers().empty()){support_min=object->support_layers().front()->bottom_z();support_max=object->support_layers().back()->print_z;}
                objects.push_back({{"object_id",std::to_string(object->model_object()->id().id)},{"name",object->model_object()->name},{"instance_ids",instances},{"model_layers",object->layers().size()},{"support_layers",object->support_layers().size()},{"support_height_range_mm",{support_min,support_max}},{"first_print_z_mm",object->layers().empty()?0:object->layers().front()->print_z},{"last_print_z_mm",object->layers().empty()?0:object->layers().back()->print_z}});
            }
            const auto& stats=print.print_statistics();return Json{{"plate",index+1},{"objects",objects},{"estimated_print_time",stats.estimated_normal_print_time},{"filament_length_mm",stats.total_used_filament},{"filament_weight_g",stats.total_weight},{"tool_changes",stats.total_toolchanges},{"note","Counts and estimates from actual fresh slicing. Check support contact at the relevant feature using slice_layer_view."}};
        }
        const auto scene=collect_slice_scene(print,args.at("print_z_mm"),args.value("object_id",std::string()),300000,args.value("instance_id",std::string()));
        if(!scene.bounds.defined)throw std::runtime_error("No sliced model/support regions at that height for this target.");
        double xmin=unscale<double>(scene.bounds.min.x()),ymin=unscale<double>(scene.bounds.min.y()),xmax=unscale<double>(scene.bounds.max.x()),ymax=unscale<double>(scene.bounds.max.y());
        if(args.contains("bounds_mm")){const auto box=args.at("bounds_mm").get<std::vector<double>>();if(box.size()!=4)throw std::runtime_error("Provide xmin,ymin,xmax,ymax.");for(double v:box)if(!std::isfinite(v)||std::abs(v)>100000)throw std::runtime_error("Use finite project-space bounds.");xmin=box[0];ymin=box[1];xmax=box[2];ymax=box[3];}
        if(xmax-xmin<.01 || ymax-ymin<.01)throw std::runtime_error("Choose slice bounds at least 0.01 mm wide/high.");
        const double scale=720/std::max(xmax-xmin,ymax-ymin),cx=(xmin+xmax)/2,cy=(ymin+ymax)/2;
        auto pixel=[&](const Point& p){return wxPoint2DDouble(384+(unscale<double>(p.x())-cx)*scale,384-(unscale<double>(p.y())-cy)*scale);};
        wxBitmap bitmap(768,768,32);wxMemoryDC dc(bitmap);dc.SetBackground(wxBrush(wxColour(242,244,246)));dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));if(!gc)throw std::runtime_error("Could not render this sliced layer.");
        auto draw_shapes=[&](const ExPolygons& shapes,const wxColour& fill){gc->SetBrush(wxBrush(fill));gc->SetPen(*wxTRANSPARENT_PEN);for(const auto& shape:shapes){auto path=gc->CreatePath();auto ring=[&](const Polygon& polygon){bool first=true;for(const auto& p:polygon.points){const auto v=pixel(p);if(first){path.MoveToPoint(v.m_x,v.m_y);first=false;}else path.AddLineToPoint(v.m_x,v.m_y);}path.CloseSubpath();};ring(shape.contour);for(const auto& hole:shape.holes)ring(hole);gc->DrawPath(path,wxODDEVEN_RULE);}};
        auto draw_paths=[&](const Polylines& paths,const wxColour& color){gc->SetPen(wxPen(color,1));for(const auto& line:paths){if(line.points.empty())continue;auto path=gc->CreatePath();auto v=pixel(line.points.front());path.MoveToPoint(v.m_x,v.m_y);for(size_t i=1;i<line.points.size();++i){v=pixel(line.points[i]);path.AddLineToPoint(v.m_x,v.m_y);}gc->StrokePath(path);}};
        draw_shapes(scene.supports,wxColour(151,214,243));draw_shapes(scene.solids,wxColour(200,204,209));draw_paths(scene.support_paths,wxColour(0,111,180));draw_paths(scene.model_paths,wxColour(46,57,69));gc.reset();dc.SelectObject(wxNullBitmap);
        wxMemoryOutputStream stream;if(!bitmap.ConvertToImage().SaveFile(stream,wxBITMAP_TYPE_PNG))throw std::runtime_error("Could not encode this sliced layer.");std::vector<unsigned char> bytes(stream.GetSize());stream.CopyTo(bytes.data(),bytes.size());
        size_t holes=0;double area=0;for(const auto& solid:scene.solids){holes+=solid.holes.size();area+=solid.area()*SCALING_FACTOR*SCALING_FACTOR;}
        return Json{{"image_data","data:image/png;base64,"+std::string(wxBase64Encode(bytes.data(),bytes.size()).utf8_string())},{"plate",index+1},{"requested_z_mm",args.at("print_z_mm")},{"actual_print_heights_mm",scene.print_heights},{"model_region_area_mm2",area},{"model_hole_contours",holes},{"support_regions",scene.supports.size()},{"support_paths",scene.support_paths.size()},{"paths_truncated",scene.paths_truncated},{"bounds_project_mm",{xmin,ymin,xmax,ymax}},{"legend","Gray/dark=model; blue=support. Top-down actual sliced regions and generated paths."}};
    }
    auto selected_targets=[&](bool require_current_plate) {
        std::vector<std::pair<size_t,size_t>> result;std::set<std::string> seen;
        for(const auto& item:args.at("targets")) {
            auto* object=get_object(p,item.at("object_id"));auto* instance=get_instance(*object,item.at("instance_id"));
            if(!seen.insert(std::to_string(instance->id().id)).second)throw std::runtime_error("Choose each instance once.");
            const size_t oi=std::find(p.model().objects.begin(),p.model().objects.end(),object)-p.model().objects.begin(),ii=std::find(object->instances.begin(),object->instances.end(),instance)-object->instances.begin();
            if(require_current_plate && !p.get_partplate_list().get_curr_plate()->get_obj_and_inst_set().count({int(oi),int(ii)}))throw std::runtime_error("Select a plate containing all requested instances before laying them out.");
            result.emplace_back(oi,ii);
        }
        if(result.empty())throw std::runtime_error("Choose at least one instance.");return result;
    };
    auto focus=[&](const std::vector<std::pair<size_t,size_t>>& targets) {
        prepare_canvas(p);auto& selection=p.canvas3D()->get_selection();selection.clear();
        bool first=true;for(const auto& item:targets){selection.add_instance(item.first,item.second,first);first=false;}p.sidebar().obj_list()->update_selections();p.canvas3D()->zoom_to_selection();
    };
    if(tool=="select_instances" || tool=="layout_selection") {
        const auto targets=selected_targets(tool=="layout_selection");
        if(tool=="select_instances"){focus(targets);return inspect();}
        if(p.get_partplate_list().get_curr_plate()->is_locked())throw std::runtime_error("Unlock the current plate before changing its layout.");
        const auto operation=args.at("operation").get<std::string>();
        if((operation=="align" && targets.size()<2)||(operation=="distribute" && targets.size()<3))throw std::runtime_error("Alignment needs two instances; equal spacing needs three.");
        if(operation=="align" && !args.contains("alignment"))throw std::runtime_error("Choose min, center, or max alignment.");
        if((operation=="align" || operation=="distribute") && !args.contains("axis"))throw std::runtime_error("Choose the X, Y, or Z axis.");
        focus(targets);
        if(operation=="center")p.center_selection();else if(operation=="fit")p.scale_selection_to_fit_print_volume();
        else if(operation=="distribute") {const auto axis=args.at("axis").get<std::string>();if(axis=="x")p.distribute_selection_x();else if(axis=="y")p.distribute_selection_y();else p.distribute_selection_z();}
        else if(operation=="align") {
            const auto axis=args.at("axis").get<std::string>(),alignment=args.at("alignment").get<std::string>();
            if(axis=="x"){if(alignment=="min")p.align_selection_x_min();else if(alignment=="max")p.align_selection_x_max();else p.align_selection_x_center();}
            else if(axis=="y"){if(alignment=="min")p.align_selection_y_min();else if(alignment=="max")p.align_selection_y_max();else p.align_selection_y_center();}
            else {if(alignment=="min")p.align_selection_z_min();else if(alignment=="max")p.align_selection_z_max();else p.align_selection_z_center();}
        }else throw std::runtime_error("Unknown layout operation.");return inspect();
    }
    if(tool=="part_properties") {
        auto* object=get_object(p,args.at("object_id"));std::vector<ModelVolume*> parts;std::set<ModelVolume*> unique;
        for(const auto& id:args.at("part_ids")){auto* part=get_part(*object,id);if(!unique.insert(part).second)throw std::runtime_error("Choose each part once.");parts.push_back(part);}
        if(parts.empty() || (!args.contains("type")&&!args.contains("name")))throw std::runtime_error("Specify a part property to change.");
        ModelVolumeType type=ModelVolumeType::MODEL_PART;
        if(args.contains("type")) {
            const std::map<std::string,ModelVolumeType> types={{"model_part",ModelVolumeType::MODEL_PART},{"negative",ModelVolumeType::NEGATIVE_VOLUME},{"modifier",ModelVolumeType::PARAMETER_MODIFIER},{"support_enforcer",ModelVolumeType::SUPPORT_ENFORCER},{"support_blocker",ModelVolumeType::SUPPORT_BLOCKER}};
            type=types.at(args.at("type"));size_t remaining=0;for(auto* part:object->volumes)if(unique.count(part)?type==ModelVolumeType::MODEL_PART:part->is_model_part())++remaining;
            if(!remaining)throw std::runtime_error("Keep at least one printable model-part type in this object.");
        }
        const auto name=args.value("name",std::string());if(args.contains("name") && (name.empty() || name.size()>200 || name.find_first_of("\r\n\t")!=std::string::npos))throw std::runtime_error("Choose a short single-line part name.");
        prepare_canvas(p);p.take_snapshot("Assistant: change part properties");
        for(auto* part:parts){if(args.contains("type"))part->set_type(type);if(args.contains("name")){part->name=name;p.sync_assemble_volume_name(part->part_guid(),name);}}
        refresh_part(p,*object,true);return inspect();
    }
    if(tool=="instance_printable") {
        const auto targets=selected_targets(false);prepare_canvas(p);p.take_snapshot("Assistant: set instance printing");
        for(const auto& item:targets)p.model().objects[item.first]->instances[item.second]->printable=args.at("printable");
        std::set<size_t> changed;for(const auto& item:targets)changed.insert(item.first);std::vector<size_t> indices(changed.begin(),changed.end());p.canvas3D()->update_instance_printable_state_for_objects(indices);for(size_t i:changed)refresh_part(p,*p.model().objects[i]);return inspect();
    }
    if(tool=="detach_instances") {
        selected_targets(false);prepare_canvas(p);p.take_snapshot("Assistant: make copies independent");
        for(const auto& item:args.at("targets")) {
            auto* object=get_object(p,item.at("object_id"));if(object->instances.size()<2)continue;auto* instance=get_instance(*object,item.at("instance_id"));
            const int oi=int(std::find(p.model().objects.begin(),p.model().objects.end(),object)-p.model().objects.begin()),ii=int(std::find(object->instances.begin(),object->instances.end(),instance)-object->instances.begin());
            p.sidebar().obj_list()->instances_to_separated_object(oi,{ii});
        }
        p.get_partplate_list().reload_all_objects();p.update();p.schedule_background_process();return inspect();
    }
    if(tool=="object_structure") {
        std::vector<size_t> indices;std::set<size_t> seen;for(const auto& id:args.at("object_ids")){auto* object=get_object(p,id);const size_t index=std::find(p.model().objects.begin(),p.model().objects.end(),object)-p.model().objects.begin();if(!seen.insert(index).second)throw std::runtime_error("Choose distinct objects.");indices.push_back(index);}
        const auto operation=args.at("operation").get<std::string>();if((operation=="merge_to_parts" && indices.size()<2) || (operation=="split_objects" && indices.size()!=1))throw std::runtime_error("Merge needs at least two objects; split one object at a time.");
        prepare_canvas(p);std::vector<ObjectVolumeID> selections;for(size_t index:indices)selections.push_back({p.model().objects[index],nullptr});p.sidebar().obj_list()->select_items(selections);
        if(operation=="merge_to_parts") {if(!p.sidebar().obj_list()->can_merge_to_multipart_object())throw std::runtime_error("This selection cannot be assembled into one multipart object.");p.sidebar().obj_list()->merge_objects(indices);}
        else {if(!p.can_split_to_objects())throw std::runtime_error("This object has no independently separable shells.");p.sidebar().obj_list()->split_objects();}
        return inspect();
    }
    if(tool=="plate_operation" || tool=="delete_plate") {
        const int index=args.at("plate").get<int>()-1;auto& plates=p.get_partplate_list();if(index<0 || index>=plates.get_plate_count())throw std::runtime_error("That plate does not exist.");
        if(tool=="delete_plate"){if(plates.get_plate_count()<=1)throw std::runtime_error("Keep at least one project plate. Remove its objects if an empty plate is wanted.");if(p.delete_plate(index)<0)throw std::runtime_error("Studio did not delete that plate. Inspect the current plates before continuing.");}else {const auto operation=args.at("operation").get<std::string>();p.take_snapshot("Assistant: change plate lock");if(plates.lock_plate(index,operation=="lock")<0)throw std::runtime_error("Studio could not change this plate's lock.");p.update();}return inspect();
    }
    if(tool=="reset_overrides") {
        const auto scope=args.at("scope").get<std::string>();const DynamicPrintConfig* config=nullptr;ModelObject* object=nullptr;ModelVolume* part=nullptr;PartPlate* plate=nullptr;
        if(scope=="plate"){const int index=args.at("plate").get<int>()-1;if(index<0 || index>=p.get_partplate_list().get_plate_count())throw std::runtime_error("That plate is stale.");plate=p.get_partplate_list().get_plate(index);config=plate->config();}
        else {object=get_object(p,args.at("object_id"));if(scope=="part"){part=get_part(*object,args.at("part_id"));config=&part->config.get();}else if(scope=="object")config=&object->config.get();else throw std::runtime_error("Unknown settings scope.");}
        std::vector<std::string> keys;const auto& process=wxGetApp().preset_bundle->prints.get_edited_preset().config;
        for(const auto& value:args.at("keys")){const auto key=value.get<std::string>();const auto* definition=process.def()->get(key);if(!process.has(key)||!definition||definition->is_code||definition->readonly||key.find("gcode")!=std::string::npos)throw std::runtime_error("Choose ordinary process-setting override keys.");if(config->has(key))keys.push_back(key);}
        if(keys.empty())return inspect();p.take_snapshot("Assistant: restore inherited settings");for(const auto& key:keys){if(plate)plate->config()->erase(key);else if(part)part->config.erase(key);else object->config.erase(key);}
        if(plate)plate->update_slice_result_valid_state(false);if(object)p.sidebar().obj_list()->object_config_options_changed({object,part});p.update();p.schedule_background_process();return inspect();
    }
    if(tool=="open_editor") {
        if(p.is_any_job_running() || p.is_background_process_slicing())throw std::runtime_error("Wait for Studio's current operation before opening an editor.");
        const std::map<std::string,GLGizmosManager::EType> editors={{"measure",GLGizmosManager::Measure},{"cut",GLGizmosManager::Cut},{"boolean",GLGizmosManager::MeshBoolean},{"text",GLGizmosManager::Text},{"svg",GLGizmosManager::Svg},{"support",GLGizmosManager::FdmSupports},{"color",GLGizmosManager::MmuSegmentation},{"seam",GLGizmosManager::Seam},{"fuzzy",GLGizmosManager::FuzzySkin},{"simplify",GLGizmosManager::Simplify},{"flatten",GLGizmosManager::Flatten}};
        prepare_canvas(p);if(!p.canvas3D()->get_gizmos_manager().open_gizmo(editors.at(args.at("editor"))))throw std::runtime_error("Select a suitable model or parts for this editor first.");return Json{{"opened",args.at("editor")},{"completed_edit",false},{"note","This opens native interactive controls. No edit has been completed by this tool."}};
    }
    if(tool=="library_export") {const bool all=args.at("all_plates");if(!p.can_add_plates_to_library(all))throw std::runtime_error("Slice the requested editable plates and resolve their slicing errors before adding them to the library.");if(!p.add_plates_to_library(all))return Json{{"outcome","unconfirmed"},{"exported",false},{"operation_error","Studio did not complete the library export. Check its cancellation/error before continuing."}};return Json{{"exported",true},{"all_plates_requested",all},{"source_project",std::string(p.get_project_filename(".3mf").utf8_string())},{"note","The native workflow confirmed publication and saved the editable source in the library. Existing plate exclusions were honored."},{"state",inspect()}};}
    if(tool=="review_print") {const bool all=args.at("all_plates");auto& plates=p.get_partplate_list();if(!(all?plates.is_all_slice_results_ready_for_print():plates.get_curr_plate()->is_slice_result_ready_for_print()))throw std::runtime_error("Slice the requested plates and resolve slicing errors before opening print review.");p.send_to_printer(all);return Json{{"review_workflow_returned",true},{"assistant_sent_job",false},{"note","The person controls the final printer/AMS selection and any send from the native dialog. Its final print status was not observed by this tool."}};}
    if(tool=="inspect_surfaces") {
        auto* object=get_object(p,args.at("object_id"));const auto instance=geometry_instance(*object,args.at("instance_id"));
        struct Candidate{ModelVolume* part;SurfacePatch patch;};std::vector<Candidate> candidates;
        const auto filter=args.value("filter","all");const double minimum=args.value("minimum_area_mm2",0.);
        for(auto* part:part_targets(*object,args.at("part_ids").get<std::vector<std::string>>()))
            for(auto& patch:surface_patches(*part,instance*part->get_matrix())) {
                if(patch.area<minimum || (filter=="downward" && patch.normal.z()>=-.1) || (filter=="upward" && patch.normal.z()<=.1) || (filter=="vertical" && std::abs(patch.normal.z())>.2))continue;
                candidates.push_back({part,std::move(patch)});
            }
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.patch.area>b.patch.area;});
        Json surfaces=Json::array();const size_t limit=std::min<size_t>(128,args.value("limit",64));
        size_t incoming=0;for(size_t i=0;i<std::min(limit,candidates.size());++i)incoming+=candidates[i].patch.faces.size();
        if(m_surface_targets.size()>4096 || m_surface_face_count+incoming>4000000){m_surface_targets.clear();m_surface_face_count=0;}
        for(size_t i=0;i<std::min(limit,candidates.size());++i) {
            const auto& c=candidates[i];const auto part_id=std::to_string(c.part->id().id);
            const auto key="surface-"+part_id+"-"+args.at("instance_id").get<std::string>()+"-"+std::to_string(c.patch.seed);
            if(auto existing=m_surface_targets.find(key);existing!=m_surface_targets.end())m_surface_face_count-=existing->second.at("faces").size();m_surface_face_count+=c.patch.faces.size();
            m_surface_targets[key]={{"object_id",args.at("object_id")},{"part_id",part_id},{"instance_id",args.at("instance_id")},{"mesh",reinterpret_cast<uintptr_t>(c.part->mesh_ptr())},{"world",matrix_json(instance*c.part->get_matrix())},{"faces",c.patch.faces},{"normal",vector_json(c.patch.normal)},{"sample",vector_json(c.patch.sample)},{"face_normal",vector_json(c.patch.sample_normal)}};
            surfaces.push_back({{"surface_id",key},{"part_id",part_id},{"area_mm2",c.patch.area},{"center_plate_mm",vector_json(c.patch.center)},{"normal_plate",vector_json(c.patch.normal)},{"sample_point_plate_mm",vector_json(c.patch.sample)},{"sample_normal_plate",vector_json(c.patch.sample_normal)},{"min_plate_mm",vector_json(c.patch.bounds.min)},{"max_plate_mm",vector_json(c.patch.bounds.max)},{"original_triangles",c.patch.faces.size()}});
        }
        return Json{{"revision",inspect().at("revision")},{"surfaces",surfaces},{"matching_surfaces",candidates.size()},{"truncated",candidates.size()>limit},{"note","Normals identify outward-facing geometry, not whether a surface is already supported. Cross-check views and sliced layers."}};
    }
    auto target=[&](const std::string& key)->const Json& {
        const auto found=m_surface_targets.find(key);if(found==m_surface_targets.end())throw std::runtime_error("Inspect surfaces first; this surface reference is unavailable.");
        const auto& record=found->second;auto* object=get_object(p,record.at("object_id"));auto* part=get_part(*object,record.at("part_id"));
        if(!part->is_model_part())throw std::runtime_error("That measured part is no longer printable model geometry. Inspect the surfaces again.");
        const auto world=geometry_instance(*object,record.at("instance_id"))*part->get_matrix();
        if(record.at("mesh").get<uintptr_t>()!=reinterpret_cast<uintptr_t>(part->mesh_ptr()) || record.at("world")!=matrix_json(world))throw std::runtime_error("The surface geometry/placement changed. Inspect its surfaces again.");return record;
    };
    if(tool=="surface_view" || tool=="part_map_view") {
        std::string object_id,instance_id;std::map<std::string,std::map<int,int>> labels;std::map<std::string,int> part_labels;Json legend=Json::array();int number=0;
        if(tool=="part_map_view") {
            object_id=args.at("object_id");instance_id=args.at("instance_id");auto* object=get_object(p,object_id);geometry_instance(*object,instance_id);
            auto ids=args.value("part_ids",std::vector<std::string>());if(ids.empty())for(const auto* part:object->volumes)if(part->is_model_part())ids.push_back(std::to_string(part->id().id));
            if(ids.empty() || ids.size()>64)throw std::runtime_error("Choose between 1 and 64 candidate printable pieces for a clear numbered view.");
            for(auto* part:part_targets(*object,ids)){const auto id=std::to_string(part->id().id);if(part_labels.count(id))throw std::runtime_error("Choose each candidate part once.");part_labels[id]=++number;legend.push_back({{"number",number},{"part_id",id},{"name",part->name},{"filament_slot",part->extruder_id()}});}
        }else for(const auto& key:args.at("surface_ids")) {
            const auto& record=target(key);if(object_id.empty()){object_id=record.at("object_id");instance_id=record.at("instance_id");}
            if(record.at("object_id")!=object_id || record.at("instance_id")!=instance_id)throw std::runtime_error("Render surfaces from one reference instance at a time.");
            ++number;for(int face:record.at("faces").get<std::vector<int>>())labels[record.at("part_id").get<std::string>()][face]=number;
            legend.push_back({{"number",number},{"surface_id",key},{"part_id",record.at("part_id")},{"normal_plate",record.at("normal")}});
        }
        auto* object=get_object(p,object_id);const auto instance=geometry_instance(*object,instance_id);std::vector<SurfaceRenderPart> parts;
        for(const auto* part:object->volumes)if(part->is_model_part()){const auto id=std::to_string(part->id().id);parts.push_back({part,instance*part->get_matrix(),labels[id],part_labels.count(id)?part_labels.at(id):-1});}
        auto rendered=render_surface_map(parts,args.at("direction"));wxImage image(768,768,rendered.rgb.data(),true);wxBitmap bitmap(image);wxMemoryDC dc(bitmap);
        dc.SetFont(wxFont(wxFontInfo(12).Bold()));dc.SetTextForeground(*wxWHITE);dc.SetPen(wxPen(*wxWHITE,2));dc.SetBrush(wxBrush(wxColour(30,35,40)));
        for(auto& entry:legend){const int label=entry.at("number");const size_t visible=rendered.visible_pixels[label];entry["visible_pixels"]=visible;if(visible<20)continue;
            const auto position=rendered.anchors.at(label);const wxString text=wxString::Format("%d",label);const wxSize size=dc.GetTextExtent(text);const int radius=std::max(12,size.x/2+5);
            dc.DrawCircle(position.first,position.second,radius);dc.DrawText(text,position.first-size.x/2,position.second-size.y/2);entry["label_pixel"]={position.first,position.second};}
        dc.SelectObject(wxNullBitmap);wxMemoryOutputStream stream;if(!bitmap.ConvertToImage().SaveFile(stream,wxBITMAP_TYPE_PNG))throw std::runtime_error("Could not encode the surface view.");
        std::vector<unsigned char> bytes(stream.GetSize());stream.CopyTo(bytes.data(),bytes.size());
        return Json{{"image_data","data:image/png;base64,"+std::string(wxBase64Encode(bytes.data(),bytes.size()).utf8_string())},{"direction",args.at("direction")},{"legend",legend},{"note","Numbered topology view. Original material colors and subtracted negative geometry are not represented."}};
    }
    if(tool=="paint_surfaces") {
        const auto layer=args.at("layer").get<std::string>(),action=args.at("action").get<std::string>();int state=0;
        if(layer=="color") {if(action=="color")state=args.at("slot");else if(action!="erase")throw std::runtime_error("Use color or erase for color painting.");if(state<0 || size_t(state)>wxGetApp().preset_bundle->filament_presets.size())throw std::runtime_error("Choose an existing project filament slot.");}
        else if(action=="enforce")state=1;else if(action=="block" && layer!="fuzzy")state=2;else if(action!="erase")throw std::runtime_error("Use enforce, block, or erase for this paint layer.");
        if(args.contains("center_mm")!=args.contains("radius_mm"))throw std::runtime_error("A brush needs both a physical center and radius.");
        struct Item{ModelObject* object;ModelVolume* part;std::vector<int> faces;Transform3d world;PaintKeepPrepared paint;
            Item(ModelObject* o,ModelVolume* p,Transform3d w):object(o),part(p),world(std::move(w)){}
        };std::map<std::string,Item> items;
        std::map<std::string,std::string> references;
        for(const auto& id:args.at("surface_ids")) {
            const auto& record=target(id);const auto key=record.at("part_id").get<std::string>();
            const auto object_id=record.at("object_id").get<std::string>(),instance_id=record.at("instance_id").get<std::string>();
            if(references.count(object_id) && references.at(object_id)!=instance_id)throw std::runtime_error("These parts are shared between copies. Use one reference instance per object, or detach the copy before painting it independently.");references[object_id]=instance_id;
            if(!items.count(key)){auto* object=get_object(p,record.at("object_id"));auto* part=get_part(*object,key);items.emplace(key,Item(object,part,geometry_instance(*object,record.at("instance_id"))*part->get_matrix()));}
            auto& item=items.at(key);const auto faces=record.at("faces").get<std::vector<int>>();item.faces.insert(item.faces.end(),faces.begin(),faces.end());
        }
        size_t touched=0;bool limited=false;
        for(auto& entry:items) {
            auto& item=entry.second;std::sort(item.faces.begin(),item.faces.end());item.faces.erase(std::unique(item.faces.begin(),item.faces.end()),item.faces.end());
            if(args.contains("center_mm")){auto prepared=prepared_surface_brush(*item.part,paint_layer(*item.part,layer),item.world,item.faces,state,vector_arg(args,"center_mm"),args.at("radius_mm"));touched+=prepared.touched;limited|=prepared.limited;item.paint=std::move(prepared.paint);}
            else {item.paint=prepared_surface_paint(*item.part,paint_layer(*item.part,layer),item.faces,state);touched+=item.faces.size();}
        }
        prepare_canvas(p);p.take_snapshot("Assistant: paint measured surfaces");std::set<ModelObject*> changed;
        for(auto& entry:items){auto& item=entry.second;paint_layer(*item.part,layer).assign(std::move(item.paint.supported));changed.insert(item.object);}
        for(auto* object:changed)refresh_part(p,*object);
        auto result=inspect();result["painting"]={{"layer",layer},{"triangles_touched",touched},{"brush_budget_limited",limited}};return result;
    }
    if(tool=="clear_paint") {
        auto* object=get_object(p,args.at("object_id"));const auto parts=part_targets(*object,args.at("part_ids").get<std::vector<std::string>>());const auto layer=args.at("layer").get<std::string>();
        for(auto* part:parts)paint_layer(*part,layer);prepare_canvas(p);p.take_snapshot("Assistant: clear painting");for(auto* part:parts)paint_layer(*part,layer).reset();refresh_part(p,*object);return inspect();
    }
    if(tool=="transform_item" || tool=="place_on_bed" || tool=="place_on_face") {
        ModelObject* object;ModelInstance* instance;Vec3d face_normal=Vec3d::Zero(),sample=Vec3d::Zero();
        if(tool=="place_on_face") {const auto& record=target(args.at("surface_id"));object=get_object(p,record.at("object_id"));instance=get_instance(*object,record.at("instance_id"));face_normal=vector_arg(record,"face_normal");sample=vector_arg(record,"sample");}
        else {object=get_object(p,args.at("object_id"));instance=get_instance(*object,args.at("instance_id"));}
        const auto parts=args.value("part_ids",std::vector<std::string>());
        const auto kind=tool=="place_on_bed"?"bed":tool=="place_on_face"?"face":args.at("kind").get<std::string>();
        const Vec3d values=tool=="place_on_face"?face_normal:tool=="transform_item"?vector_arg(args,"values"):Vec3d::Zero();
        const std::optional<Vec3d> pivot=args.contains("pivot_mm")?std::optional<Vec3d>(vector_arg(args,"pivot_mm")):std::nullopt;
        auto prepared=prepare_item_transform(*object,std::to_string(instance->id().id),parts,kind,values,pivot);
        if(tool=="place_on_face" && (prepared.instance_matrix*instance->get_matrix().inverse()*sample).z()>.02)throw std::runtime_error("Other geometry would reach the bed before that face. Choose an exterior contact face, or use explicit rotation/translation for the intended placement.");
        prepare_canvas(p);p.take_snapshot("Assistant: transform explicit target");prepared.apply();
        refresh_part(p,*object,true);return inspect();
    }
    return std::nullopt;
}
struct Actions::ModelWork {
    std::atomic<bool> done{false},canceled{false};std::string tool,error,revision;Json args,stats;
    std::unique_ptr<Model> model;std::unique_ptr<PaintKeepPrepared> result;
    std::unique_ptr<::Slic3r::Emboss::FontFileWithCache> font;EmbossStyle style;std::string font_name;std::unique_ptr<PreparedTextMesh> lettering;
};
Json Actions::start_model_work(const std::string& tool,const Json& args)
{
    if(m_model_work && m_model_work->canceled && m_model_work->done)m_model_work.reset();
    if(m_model_work)throw std::runtime_error("Wait for the assistant's current modeling operation.");
    auto* p=wxGetApp().plater();ModelObject* object=nullptr;
    if(args.contains("object_id")){object=get_object(*p,args.at("object_id"));geometry_editable(*object);geometry_instance(*object,args.at("instance_id"));}
    else if(tool!="add_text" || args.contains("instance_id"))throw std::runtime_error("Specify both object and reference instance IDs for this operation.");
    if(tool!="add_text")get_part(*object,args.at(tool=="mesh_boolean"?"primary_part_id":"part_id"));
    auto work=std::make_shared<ModelWork>();work->tool=tool;work->args=args;work->revision=args.at("revision");work->model=std::make_unique<Model>(p->model());
    if(tool=="add_text") {
        if(args.contains("center_mm"))vector_arg(args,"center_mm");else if(object)throw std::runtime_error("Choose the measured physical center for text attached to this object.");
        if(!object && args.value("negative",false))throw std::runtime_error("Negative text must belong to the object it engraves.");
        if(!object && p->get_partplate_list().get_curr_plate()->is_locked())throw std::runtime_error("Unlock the plate before adding lettering.");
        if(args.contains("axis"))geometry_frame(Vec3d::Zero(),vector_arg(args,"axis"));const int slot=args.value("slot",1);
        if(slot<1 || size_t(slot)>wxGetApp().preset_bundle->filament_presets.size())throw std::runtime_error("Choose an existing project filament slot.");
        const auto requested=wxString::FromUTF8(args.value("font",std::string("Arial")));const auto names=wxFontEnumerator::GetFacenames();const int index=names.Index(requested,false);
        if(index==wxNOT_FOUND)throw std::runtime_error("That font is not installed. Use list_fonts to choose an available face.");
        wxFontInfo info(10);info.FaceName(names[index]);if(args.value("bold",false))info.Bold();if(args.value("italic",false))info.Italic();const wxFont face(info);
        auto file=WxFontUtils::create_font_file(face);if(!file)throw std::runtime_error("This installed font has no supported outline file. Choose another face from list_fonts.");
        work->font_name=std::string(face.GetFaceName().utf8_string());work->style=WxFontUtils::create_emboss_style(face);
        const auto variants=font_collection_names(*file);int collection=args.value("collection_index",0);
        if(!args.contains("collection_index") && variants.size()>1) {
            for(size_t i=0;i<variants.size();++i){const auto variant=wxString::FromUTF8(variants[i]).Lower();const bool bold=variant.Contains("bold"),italic=variant.Contains("italic")||variant.Contains("oblique");if(bold==args.value("bold",false) && italic==args.value("italic",false)){collection=int(i);break;}}
        }
        if(collection<0 || size_t(collection)>=variants.size())throw std::runtime_error("Choose a valid font collection index from list_fonts.");
        work->style.prop.collection_number=unsigned(collection);work->font=std::make_unique<::Slic3r::Emboss::FontFileWithCache>(std::move(file));
    }
    m_model_work=work;
    std::thread([work] {
        try {
            if(work->tool=="add_text") {
                work->lettering=std::make_unique<PreparedTextMesh>(prepare_text_mesh(*work->font,work->style,work->args.at("text"),work->args.at("height_mm"),work->args.at("depth_mm"),[&]{return work->canceled.load();}));
                work->stats={{"font",work->font_name},{"visible_height_mm",work->lettering->height_mm},{"thickness_mm",work->lettering->depth_mm},{"triangles",work->lettering->mesh.facets_count()}};work->done=true;return;
            }
            ModelObject* object=nullptr;for(auto* candidate:work->model->objects)if(std::to_string(candidate->id().id)==work->args.at("object_id").get<std::string>())object=candidate;
            if(!object)throw std::runtime_error("Could not stage the requested object.");
            auto* primary=get_part(*object,work->args.at(work->tool=="mesh_boolean"?"primary_part_id":"part_id"));
            const auto canceled=[&]{return work->canceled.load();};const size_t before=primary->mesh().facets_count();
            work->result=std::make_unique<PaintKeepPrepared>();
            if(work->tool=="mesh_boolean") {
                std::vector<ModelVolume*> operands;for(const auto& id:work->args.at("operand_part_ids"))operands.push_back(get_part(*object,id));
                *work->result=prepare_mesh_boolean(*object,work->args.at("instance_id"),*primary,operands,work->args.at("operation"),canceled);
            }else *work->result=prepare_mesh_simplification(*object,work->args.at("instance_id"),*primary,work->args.at("target_triangles"),canceled);
            work->stats={{"before_triangles",before},{"after_triangles",work->result->mesh.facets_count()},{"paint_reprojection_approximate",true}};
        }catch(const std::exception& error){work->error=error.what();}catch(...){work->error="Studio could not complete this mesh operation. No project changes applied.";}
        work->done=true;
    }).detach();return {{"pending","model_work"}};
}
void Actions::cancel_model_work(){if(m_model_work){m_model_work->canceled=true;if(m_model_work->done)m_model_work.reset();}}
std::optional<Json> Actions::poll_model_work()
{
    if(!m_model_work || !m_model_work->done)return std::nullopt;auto work=std::move(m_model_work);
    if(work->canceled)return Json{{"cancelled",true},{"changes_applied",false}};
    if(!work->error.empty())return Json{{"operation_error",work->error},{"changes_applied",false}};
    auto* p=wxGetApp().plater();if(!p || p->is_background_process_slicing() || p->is_any_job_running() || inspect().at("revision")!=work->revision)
        return Json{{"operation_error","The project or active operation changed while modeling. The staged result was discarded; inspect again."},{"changes_applied",false}};
    auto* object=work->args.contains("object_id")?get_object(*p,work->args.at("object_id")):nullptr;
    if(work->tool=="add_text") {
        const auto instance=object?geometry_instance(*object,work->args.at("instance_id")):Transform3d::Identity();const Vec3d axis=work->args.contains("axis")?vector_arg(work->args,"axis"):Vec3d::UnitZ();
        auto frame=geometry_frame(Vec3d::Zero(),axis);frame.rotate(Eigen::AngleAxisd(work->args.value("rotation_deg",0.)*PI/180.,Vec3d::UnitZ()));
        auto* plate=p->get_partplate_list().get_curr_plate();const Vec3d center=work->args.contains("center_mm")?vector_arg(work->args,"center_mm"):Vec3d(plate->get_origin().x()+plate->get_size().x()/2,plate->get_origin().y()+plate->get_size().y()/2,-work->lettering->mesh.transformed_bounding_box(frame).min.z());frame.translation()=center;
        const bool negative=work->args.value("negative",false);
        if(negative) {BoundingBoxf3 body;for(const auto* part:object->volumes)if(part->is_model_part())body.merge(part->mesh().transformed_bounding_box(instance*part->get_matrix()));const auto text=work->lettering->mesh.transformed_bounding_box(frame);if(!body.defined || (text.max.array()<body.min.array()).any() || (text.min.array()>body.max.array()).any())throw std::runtime_error("The negative lettering misses this object's body. Check its measured center and depth; no part was added.");}
        TextInfo info;info.m_font_name=work->font_name;info.m_font_version=CUR_FONT_VERSION;info.m_font_size=work->lettering->style.prop.size_in_mm;info.m_bold=work->args.value("bold",false);info.m_italic=work->args.value("italic",false);
        info.m_text=work->args.at("text");info.m_thickness=float(work->lettering->depth_mm);info.m_surface_type=TextInfo::HORIZONAL;info.m_rotate_angle=float(work->args.value("rotation_deg",0.));info.text_configuration.text=info.m_text;info.text_configuration.style=work->lettering->style;
        prepare_canvas(*p);p->take_snapshot("Assistant: add lettering");ModelVolume* part=nullptr;
        if(!object){const size_t before=p->model().objects.size();p->sidebar().obj_list()->load_mesh_object(work->lettering->mesh,"Lettering",true);if(p->model().objects.size()!=before+1)throw std::runtime_error("Studio did not create exactly one text object. Inspect the project.");object=p->model().objects.back();part=object->volumes.front();part->set_transformation(Geometry::Transformation(Transform3d::Identity()));object->instances.front()->set_transformation(Geometry::Transformation(frame));object->config.set("extruder",work->args.value("slot",1));}
        else {part=object->add_volume(std::move(work->lettering->mesh),negative?ModelVolumeType::NEGATIVE_VOLUME:ModelVolumeType::MODEL_PART);part->set_transformation(Geometry::Transformation(instance.inverse()*frame));}
        part->set_text_info(info);part->name="Lettering";part->config.set("extruder",work->args.value("slot",1));part->calculate_convex_hull();p->sidebar().obj_list()->sync_filament_from_model();refresh_part(*p,*object,true);
        auto result=inspect();result["lettering"]=work->stats;result["created_part_id"]=std::to_string(part->id().id);return result;
    }
    auto* primary=get_part(*object,work->args.at(work->tool=="mesh_boolean"?"primary_part_id":"part_id"));
    std::vector<size_t> operands;
    if(work->tool=="mesh_boolean")for(const auto& id:work->args.at("operand_part_ids")){auto* part=get_part(*object,id);operands.push_back(std::find(object->volumes.begin(),object->volumes.end(),part)-object->volumes.begin());}
    prepare_canvas(*p);p->take_snapshot("Assistant: "+work->tool);
    primary->commit_mesh_keep_paint(std::move(*work->result));primary->set_new_unique_id();primary->calculate_convex_hull();primary->source={};
    std::sort(operands.rbegin(),operands.rend());for(size_t index:operands){p->propagate_volume_delete_to_assemble(*object->volumes[index]);object->delete_volume(index);}
    refresh_part(*p,*object,true);auto result=inspect();result["mesh_operation"]=work->stats;return result;
}
}}}
