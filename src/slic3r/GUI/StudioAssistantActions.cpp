#include "StudioAssistantActions.hpp"
#include "StudioAssistantModel.hpp"
#include "StudioAssistantGeometry.hpp"
#include "StudioAssistantLibrarySearch.hpp"
#include "StudioAssistantState.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_ObjectList.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "NotificationManager.hpp"
#include "SlicedLibraryStorage.hpp"
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "miniz.h"
#include <wx/base64.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <filesystem>
#include <cmath>
#include <set>
#include <fstream>
#include <thread>

namespace Slic3r { namespace GUI { namespace Assistant {
struct Actions::LibraryTransfer {
    std::atomic<bool> cancel{false}, done{false};
    std::string tool, temporary, source, error;
    Json args, result;
    ~LibraryTransfer() {
        if (!temporary.empty()) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(temporary),ec); }
    }
};
namespace {
Plater& plater() {
    auto* p = wxGetApp().plater();
    if (!p) throw std::runtime_error("Studio's project editor is not available.");
    if (p->is_loading_project()) throw std::runtime_error("Wait for the project to finish loading.");
    return *p;
}
std::string lower(std::string value) { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return value; }
Json vec(const Vec3d& v) { return Json::array({v.x(), v.y(), v.z()}); }
Vec3d geometry_arg(const Json& args,const char* key) {
    const auto& value=args.at(key);
    if (!value.is_array() || value.size()!=3) throw std::runtime_error("Supply exactly three coordinates.");
    Vec3d result(value.at(0).get<double>(),value.at(1).get<double>(),value.at(2).get<double>());
    geometry_vector(result); return result;
}
std::string id(const ModelObject* object) { return std::to_string(object->id().id); }
ModelObject* find_object(Plater& p, const std::string& value) {
    for (auto* object : p.model().objects) if (id(object) == value) return object;
    throw std::runtime_error("That object is no longer in this project. Inspect the project again.");
}
PartPlate* find_plate(Plater& p, int number) {
    if (number < 1 || number > p.get_partplate_list().get_plate_count()) throw std::runtime_error("That plate does not exist.");
    return p.get_partplate_list().get_plate(number-1);
}
Json config_values(const DynamicPrintConfig& config) {
    Json result = Json::object();
    for (const auto& key : config.keys()) {
        // Do not transmit machine secrets, custom G-code, URLs, or local paths.
        const auto* def = config.def()->get(key);
        if (def && !def->is_code && key.find("gcode") == std::string::npos && key.find("host") == std::string::npos && key.find("path") == std::string::npos && key.find("url") == std::string::npos)
            result[key] = config.opt_serialize(key);
    }
    return result;
}
Json object_summary(const ModelObject& object) {
    Json volumes = Json::array(), instances = Json::array();
    for (const auto* volume : object.volumes) {
        const auto box=volume->mesh().transformed_bounding_box(volume->get_matrix());
        volumes.push_back({{"id",std::to_string(volume->id().id)}, {"name", volume->name}, {"kind", int(volume->type())},
        {"model_part", volume->is_model_part()}, {"negative", volume->is_negative_volume()}, {"support_painted", !volume->supported_facets.empty()},
        {"color_painted", !volume->mmu_segmentation_facets.empty()}, {"filament_slot", volume->extruder_id()},
        {"size_mm", vec(box.size())}, {"min_object_mm", vec(box.min)}, {"max_object_mm", vec(box.max)},
        {"mesh_volume_mm3", std::abs(double(its_volume(volume->mesh().its))*volume->get_matrix().linear().determinant())}, {"settings", config_values(volume->config.get())}});
    }
    for (const auto* instance : object.instances) {
        const auto matrix = instance->get_transformation().get_matrix();
        Json transform = Json::array();
        for (int row=0; row<4; ++row) for (int col=0; col<4; ++col) transform.push_back(matrix(row,col));
        Json part_bounds=Json::array();
        for (const auto* volume:object.volumes) {
            const auto bounds=volume->mesh().transformed_bounding_box(matrix*volume->get_matrix());
            part_bounds.push_back({{"part_id",std::to_string(volume->id().id)},{"min_mm",vec(bounds.min)},{"max_mm",vec(bounds.max)}});
        }
        instances.push_back({{"id", std::to_string(instance->id().id)}, {"offset_mm", vec(instance->get_offset())}, {"transform", transform}, {"printable", instance->printable},{"part_bounds_plate_mm",part_bounds}});
    }
    const auto box = object.bounding_box();
    return {{"id", id(&object)}, {"name", object.name}, {"size_mm", vec(box.size())}, {"min_mm", vec(box.min)},
        {"max_mm", vec(box.max)}, {"instances", instances}, {"volumes", volumes}, {"overrides", config_values(object.config.get())}};
}
void editable(Plater& p) {
    if (p.only_gcode_mode() || p.is_gcode_3mf()) throw std::runtime_error("This is a sliced file. Open its editable source project before changing geometry or settings.");
    if (p.is_background_process_slicing() || p.is_any_job_running()) throw std::runtime_error("Studio is busy. Wait for the current operation to finish.");
    if (p.is_assemble_undo_stack_active()) throw std::runtime_error("Switch to Prepare before editing this project.");
}
void select(Plater& p, const std::vector<std::string>& ids) {
    if (ids.empty()) throw std::runtime_error("Select at least one object.");
    std::vector<ObjectVolumeID> items;
    for (const auto& value : ids) items.push_back({find_object(p, value), nullptr});
    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    // Tab changes may queue their canvas switch. Native edits must bind the
    // Prepare canvas now, even when the previous assistant action opened Preview.
    p.select_view_3D("3D");
    p.sidebar().obj_list()->select_items(items);
    p.canvas3D()->zoom_to_selection();
}
bool valid_color(const std::string& color) {
    return (color.size()==7 || color.size()==9) && color[0]=='#' && std::all_of(color.begin()+1,color.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
}
}

Actions::~Actions() { cancel_library(); }
void Actions::cancel_library() { if (m_transfer) m_transfer->cancel = true; m_transfer.reset(); }

Json Actions::start_library(const std::string& tool, const Json& args)
{
    cancel_library();
    const auto folder = wxGetApp().app_config->get("sliced_library_folder");
    if (folder.empty()) throw std::runtime_error("Choose a folder in Sliced library first.");
    const auto root = std::filesystem::u8path(folder);
    auto overrides = Json::parse(wxGetApp().app_config->get("sliced_library_source_paths"),nullptr,false);
    if (!overrides.is_object()) overrides = Json::object();
    std::string name;
    if (tool != "search_library") {
        const auto found = m_library_entries.find(args.at("entry_id").get<std::string>());
        if (found == m_library_entries.end()) throw std::runtime_error("Search the library again before using that entry.");
        name = found->second;
    } else if (args.at("query").get<std::string>().size() < 2) throw std::runtime_error("Use at least two characters to search the library.");
    auto transfer = std::make_shared<LibraryTransfer>(); transfer->tool = tool; transfer->args = args;
    if (tool != "search_library") {
        const auto temporary = std::string(wxFileName::CreateTempFileName("studio-assistant-").utf8_string());
        if (!temporary.empty()) {
            transfer->temporary = temporary;
            std::filesystem::rename(std::filesystem::u8path(temporary),std::filesystem::u8path(temporary+".3mf"));
            transfer->temporary += ".3mf";
        }
    }
    if (tool != "search_library" && transfer->temporary.empty()) throw std::runtime_error("A temporary source project could not be created.");
    m_transfer = transfer;
    std::thread([transfer, root, overrides, name] {
        // Network/iCloud file access is isolated from the main thread. Parse
        // models only after staging, on the UI thread: ModelConfig timestamps
        // are global and must not be modified from a background worker.
        try {
            if (transfer->tool == "search_library") {
                LibrarySearch search(transfer->args.at("query").get<std::string>());
                for (const auto& item : std::filesystem::directory_iterator(root)) {
                    if (transfer->cancel) return;
                    search.consider(item.path().filename().u8string());
                }
                Json matches = Json::array();
                for (const auto& filename : search.filenames())
                    matches.push_back({{"entry_id", base64url(sha256(filename))}, {"name", filename.substr(0,filename.size()-10)}, {"filename", filename}});
                transfer->result = {{"entries", matches}, {"total_matches", search.total_matches()}, {"truncated", search.truncated()}};
                if (search.truncated()) transfer->result["note"]="Only the first 40 matching names are shown in alphabetical order. More matches exist; refine the search query before deciding the requested model is unavailable.";
            } else {
                std::set<std::string> available;
                for (const auto& source : LibraryStorage::source_projects(root,name,overrides)) if (source.value("available",false)) available.insert(source.at("path").get<std::string>());
                if (available.size()!=1) throw std::runtime_error(available.empty() ? "The editable source is unavailable. Locate it using the library's Open source project menu." : "This item has multiple editable sources. Choose the intended project in the library menu.");
                transfer->source = *available.begin();
                const auto path = std::filesystem::u8path(transfer->source);
                const auto before = std::filesystem::last_write_time(path);
                const auto size = std::filesystem::file_size(path);
                if (size > 512*1024*1024) throw std::runtime_error("This source exceeds the assistant's 512 MB import limit. Open it through Studio's project menu instead.");
                std::ifstream input(path,std::ios::binary);
                std::ofstream output(std::filesystem::u8path(transfer->temporary),std::ios::binary|std::ios::trunc);
                if (!input || !output) throw std::runtime_error("The source project could not be staged.");
                char block[131072]; uintmax_t copied=0;
                while (input && !transfer->cancel) {
                    input.read(block,sizeof(block)); const auto count=input.gcount(); output.write(block,count); copied+=count;
                    if (copied>size || !output) throw std::runtime_error("The source changed or could not be staged.");
                }
                output.close();
                if (transfer->cancel) return;
                if (!input.eof() || copied!=size || std::filesystem::file_size(path)!=size || std::filesystem::last_write_time(path)!=before)
                    throw std::runtime_error("The source changed during reading. Inspect it again before importing.");
            }
        } catch (const std::exception& e) { transfer->error = e.what(); }
        transfer->done = true;
    }).detach();
    return {{"pending", "library"}};
}

std::optional<Json> Actions::poll_library()
{
    if (!m_transfer || !m_transfer->done) return {};
    auto transfer = std::move(m_transfer);
    if (transfer->cancel) return Json{{"cancelled",true}};
    if (!transfer->error.empty()) throw std::runtime_error(transfer->error);
    if (transfer->tool == "search_library") {
        m_library_entries.clear();
        for (auto& entry : transfer->result.at("entries")) { m_library_entries[entry.at("entry_id").get<std::string>()] = entry.at("filename").get<std::string>(); entry.erase("filename"); }
        return transfer->result;
    }
    if (transfer->tool == "add_library_model") return add_library_object(transfer->temporary,transfer->source,transfer->args);
    DynamicPrintConfig config;
    auto model = read_source_model(transfer->temporary,config);
    Json objects = Json::array();
    for (const auto* object : model.objects) objects.push_back(object_summary(*object));
    Json materials = Json::object();
    for (const auto* key : {"filament_colour","filament_type","filament_settings_id","printer_model"}) if (config.has(key)) materials[key]=config.opt_serialize(key);
    return Json{{"objects",objects},{"materials",materials},{"note","Source objects may span several plates. Choose the requested model, preserving its assembled volumes and color-to-filament assignments."}};
}

Json Actions::inspect()
{
    auto& p = plater();
    const auto full = wxGetApp().preset_bundle->full_config();
    Json objects = Json::array(), plates = Json::array(), warnings = Json::array();
    size_t total_instances=0;
    std::set<std::string> seen;
    auto add_warning = [&](Json value) { if (seen.insert(value.dump()).second) warnings.push_back(std::move(value)); };
    for (auto* object : p.model().objects) { objects.push_back(object_summary(*object)); total_instances+=object->instances.size(); }
    auto& list = p.get_partplate_list();
    for (int i=0; i<list.get_plate_count(); ++i) {
        auto* plate = list.get_plate(i);
        Json membership = Json::array();
        for (auto [object, instance] : plate->get_obj_and_inst_set()) if (object >= 0 && size_t(object) < p.model().objects.size())
            membership.push_back({{"object_id", id(p.model().objects[object])}, {"instance_index", instance}});
        const bool fresh = plate->is_slice_result_valid();
        plates.push_back({{"plate", i+1}, {"name", plate->get_plate_name()}, {"size_mm", Json::array({plate->get_size().x(), plate->get_size().y()})},
            {"origin_mm", vec(plate->get_origin())}, {"members", membership}, {"outside_instances", plate->get_obj_and_inst_outside_set().size()},
            {"locked", plate->is_locked()}, {"slice_valid", fresh}, {"overrides", plate->get_diff_plate_setting()}});
        if (!p.is_background_process_slicing()) {
            PrintBase* base = nullptr; GCodeResult* result = nullptr; int index = 0;
            plate->get_print(&base, &result, &index);
            if (auto* print = dynamic_cast<Print*>(base)) {
                auto append = [&](const PrintStateBase::Warning& w, const std::string& object) {
                    if (w.current) add_warning({{"origin", "slicer"}, {"plate", i+1}, {"object_id", object}, {"message", w.message},
                        {"code", w.message_id}, {"level", w.level == PrintStateBase::WarningLevel::CRITICAL ? "critical" : "warning"}, {"slice_fresh", fresh}});
                };
                for (int step=0; step<psCount; ++step) for (const auto& w : print->step_state_with_warnings(PrintStep(step)).warnings) append(w, "");
                for (const auto* object : print->objects()) for (int step=0; step<posCount; ++step)
                    for (const auto& w : object->step_state_with_warnings(PrintObjectStep(step)).warnings) append(w, id(object->model_object()));
            }
            if (auto* gcode = plate->get_slice_result(); gcode && fresh)
                for (auto warning : gcode->warnings) add_warning({{"origin", "gcode"}, {"plate", i+1}, {"message", std::string(Plater::get_slice_warning_string(warning).utf8_string())},
                    {"code", warning.error_code}, {"level", warning.level >= 2 ? "error" : warning.level == 1 ? "warning" : "information"}, {"slice_fresh", true}});
        }
    }
    if (auto* manager = p.get_notification_manager()) for (const auto& notice : manager->assistant_diagnostics())
        add_warning({{"origin", "visible_notification"}, {"message", notice.text}, {"level", notice.level}, {"slice_fresh", !notice.stale}, {"plate", list.get_curr_plate_index()+1}});
    Json settings = Json::object();
    for (const auto* key : {"printer_model", "printer_settings_id", "print_settings_id", "filament_settings_id", "filament_type", "filament_colour", "filament_is_support", "nozzle_diameter",
        "curr_bed_type", "layer_height", "enable_support", "support_type", "support_style", "support_on_build_plate_only", "support_filament", "support_interface_filament", "support_top_z_distance",
        "support_bottom_z_distance", "support_interface_top_layers", "support_threshold_angle", "enable_prime_tower", "brim_type", "wall_loops", "sparse_infill_density", "print_sequence"})
        if (full.has(key)) settings[key] = full.opt_serialize(key);
    Json revision_data = {{"model_id", std::to_string(p.model().id().id)}, {"objects", objects}, {"plates", plates}, {"settings", config_values(full)}, {"current_plate", list.get_curr_plate_index()}};
    Json selected = Json::array();
    Json selected_parts=Json::array(); std::set<std::string> selected_part_ids;
    const auto& selection=p.get_selection();
    for (const auto index : selection.get_volume_idxs()) {
        const auto* gl=selection.get_volume(index);
        if (!gl || gl->object_idx()<0 || size_t(gl->object_idx())>=p.model().objects.size()) continue;
        const auto* object=p.model().objects[gl->object_idx()];
        if (gl->volume_idx()<0 || size_t(gl->volume_idx())>=object->volumes.size()) continue;
        const auto part=std::to_string(object->volumes[gl->volume_idx()]->id().id);
        if (selected_part_ids.insert(part).second) selected_parts.push_back({{"object_id",id(object)},{"part_id",part}});
    }
    for (const auto& item : p.get_selection().get_content()) {
        if (item.first >= 0 && size_t(item.first) < p.model().objects.size()) selected.push_back(id(p.model().objects[item.first]));
    }
    revision_data["selection"] = selection_revision(int(selection.get_mode()),selection.get_content(),selection.get_volume_idxs());
    // Mesh/painting changes can keep identical bounds. Include their native
    // identities/timestamps in the revision without transmitting geometry.
    Json geometry = Json::array();
    for (const auto* object : p.model().objects) for (const auto* volume : object->volumes) {
        const auto matrix = volume->get_transformation().get_matrix();
        Json transform = Json::array();
        for (int row=0;row<4;++row) for (int col=0;col<4;++col) transform.push_back(matrix(row,col));
        geometry.push_back({{"id", volume->id().id}, {"mesh", reinterpret_cast<uintptr_t>(volume->mesh_ptr())}, {"transform", transform},
            {"support", volume->supported_facets.timestamp()}, {"color", volume->mmu_segmentation_facets.timestamp()}, {"seam", volume->seam_facets.timestamp()}});
    }
    revision_data["geometry"] = geometry;
    return {{"project", std::string(p.get_project_name().utf8_string())}, {"revision", base64url(sha256(revision_data.dump()))}, {"total_instances", total_instances},
        {"objects", objects}, {"plates", plates}, {"current_plate", list.get_curr_plate_index()+1}, {"settings", settings},
        {"warnings", warnings}, {"busy", p.is_background_process_slicing() || p.is_any_job_running() || p.is_background_process_update_scheduled()},
        {"editable", !p.only_gcode_mode() && !p.is_gcode_3mf()}, {"selected_object_ids", selected}, {"selected_parts",selected_parts}, {"can_undo", p.can_undo()}, {"can_redo", p.can_redo()},
        {"limitations", Json::array({"Physical filament colors/materials may differ from digital slots.", "Simple plane cuts, primitive parts/negative volumes and part removal are available. Arbitrary sculpting and face-level support painting are not automated.", "Warnings may be stale until resliced; absence of warnings does not prove printability."})}};
}

std::string Actions::library_source(const std::string& value)
{
    auto found = m_library_entries.find(value);
    if (found == m_library_entries.end()) throw std::runtime_error("Search the library again before opening that entry.");
    const auto root = std::filesystem::u8path(wxGetApp().app_config->get("sliced_library_folder"));
    auto overrides = Json::parse(wxGetApp().app_config->get("sliced_library_source_paths"), nullptr, false);
    if (!overrides.is_object()) overrides = Json::object();
    const auto sources = LibraryStorage::source_projects(root, found->second, overrides);
    std::set<std::string> available;
    for (const auto& source : sources) if (source.value("available", false)) available.insert(source.at("path").get<std::string>());
    if (available.size() != 1) throw std::runtime_error(available.empty() ? "The editable source is unavailable. Use the library's Open source project action to locate it." : "This item has multiple source projects. Open the intended source using the library menu.");
    return *available.begin();
}

Json Actions::execute(const std::string& tool, const Json& args)
{
    auto& p = plater();
    if (!args.is_object()) throw std::runtime_error("Tool arguments must be an object.");
    if (tool == "inspect_project") return inspect();
    if (tool == "model_view") {
        if (p.is_background_process_slicing() || p.is_any_job_running()) throw std::runtime_error("Wait for Studio's current operation before rendering a model view.");
        const auto direction = args.value("direction", "iso");
        const std::map<std::string, Camera::ViewAngleType> views = {{"iso", Camera::ViewAngleType::Iso}, {"top", Camera::ViewAngleType::Top},
            {"bottom", Camera::ViewAngleType::Bottom}, {"front", Camera::ViewAngleType::Front},
            {"rear",Camera::ViewAngleType::Rear},{"left",Camera::ViewAngleType::Left},{"right",Camera::ViewAngleType::Right}};
        if (!views.count(direction)) throw std::runtime_error("Unknown model-view direction.");
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        p.select_view_3D("3D");
        ThumbnailData thumbnail;
        ThumbnailsParams params{{Vec2d(768,768)}, false, true, false, false, p.get_partplate_list().get_curr_plate_index()};
        params.use_plate_box = false; params.background_color = Vec4f(0.32f,0.34f,0.37f,1.0f);
        p.canvas3D()->render_thumbnail(thumbnail, 768,768,params,Camera::EType::Ortho,views.at(direction));
        if (!thumbnail.is_valid()) throw std::runtime_error("Studio could not render this model view.");
        size_t length=0;
        std::unique_ptr<void, decltype(&mz_free)> png(tdefl_write_image_to_png_file_in_memory_ex(thumbnail.pixels.data(),768,768,4,&length,6,true), mz_free);
        if (!png) throw std::runtime_error("The model view could not be encoded.");
        return {{"image_data", "data:image/png;base64," + std::string(wxBase64Encode(png.get(),length).utf8_string())}, {"direction", direction},
            {"plate", p.get_partplate_list().get_curr_plate_index()+1}, {"note", "Rendered model geometry, not a sliced layer or proof of support. Includes the current plate, not the rest of the desktop."}};
    }
    if (tool == "inspect_settings") {
        const auto query = lower(args.at("query").get<std::string>());
        if (query.empty()) throw std::runtime_error("Specify a setting to search for.");
        const auto& print = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        Json results = Json::array();
        for (const auto& key : print.keys()) {
            auto* def = print.def()->get(key);
            if (!def || def->is_code || def->readonly) continue;
            if (lower(key + " " + def->label + " " + def->category).find(query) == std::string::npos) continue;
            results.push_back({{"key", key}, {"label", def->label}, {"description", def->tooltip}, {"value", print.opt_serialize(key)},
                {"type", int(def->type)}, {"min", def->min}, {"max", def->max}, {"enum_values", def->enum_values}, {"category", def->category}});
            if (results.size() >= 50) break;
        }
        return {{"settings", results}, {"note", "These are global process values. Inspect object and plate overrides before editing."}};
    }
    if (tool == "list_profiles") {
        const auto kind = args.at("kind").get<std::string>(), query = lower(args.at("query").get<std::string>());
        auto& bundle = *wxGetApp().preset_bundle;
        const PresetCollection* profiles = kind == "printer" ? &bundle.printers : kind == "process" ? &bundle.prints : kind == "filament" ? &bundle.filaments : nullptr;
        if (!profiles) throw std::runtime_error("Unknown profile type.");
        Json matches = Json::array();
        for (const auto& preset : profiles->get_presets()) if (preset.is_visible && (kind == "printer" || preset.is_compatible) && lower(preset.name).find(query) != std::string::npos) {
            matches.push_back({{"name", preset.name}, {"compatible", preset.is_compatible}});
            if (matches.size() >= 60) break;
        }
        return {{"profiles", matches}, {"project_filaments", bundle.filament_presets}};
    }
    if (tool == "search_library" || tool == "inspect_library_source") return start_library(tool,args);
    if (changes_project(tool)) editable(p);
    static const std::set<std::string> revision_required = {"set_settings", "use_profile", "arrange", "orient", "move_to_plate", "copies", "rename", "new_plate", "transform", "delete_selected", "add_library_model", "assign_part_filament", "split_parts", "set_filament_color", "plane_cut", "add_primitive", "delete_parts"};
    if (revision_required.count(tool) && !args.contains("revision")) throw std::runtime_error("Inspect the project and provide its current revision before editing.");
    if (args.contains("revision") && args.at("revision") != inspect().at("revision"))
        throw std::runtime_error("The project changed since inspection. Inspect again before applying this action.");
    if (tool == "orient" || tool == "move_to_plate" || tool == "transform" || tool == "delete_selected") {
        // Inspection reports Prepare's selection. Preview has its own canvas
        // and selection, so bind Prepare before acting on the inspected items.
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        p.select_view_3D("3D");
    }
    if (tool == "select_objects") { select(p, args.at("object_ids").get<std::vector<std::string>>()); return inspect(); }
    if (tool=="model_section") {
        const auto* object=find_object(p,args.at("object_id"));
        const auto point=geometry_arg(args,"point_mm"), normal=geometry_arg(args,"normal");
        const auto frame=geometry_frame(point,normal);
        const auto section=model_section(*object,args.at("instance_id"),point,normal);
        Json contours=Json::array(); double area=0; size_t count=0; bool truncated=false;
        auto append=[&](const Polygon& ring,bool hole) {
            Json points=Json::array();
            for (const auto& pt:ring.points) {
                if (++count>6000) { truncated=true;break; }
                points.push_back({unscale<double>(pt.x()),unscale<double>(pt.y())});
            }
            if (!points.empty()) contours.push_back({{"hole",hole},{"points_mm",points}});
        };
        for (const auto& shape:section) {
            area+=shape.area();append(shape.contour,false);
            for (const auto& hole:shape.holes) append(hole,true);
        }
        return {{"origin_plate_mm",vec(point)},{"basis_u",vec(frame.linear().col(0))},{"basis_v",vec(frame.linear().col(1))},
            {"normal",vec(normal.normalized())},{"area_mm2",area*SCALING_FACTOR*SCALING_FACTOR},{"contours",contours},{"contours_truncated",truncated}};
    }
    if (tool=="plane_cut" || tool=="add_primitive" || tool=="delete_parts") {
        auto* object=find_object(p,args.at("object_id"));
        const auto index=size_t(std::find(p.model().objects.begin(),p.model().objects.end(),object)-p.model().objects.begin());
        Json details;
        if (tool=="plane_cut") {
            const auto side=args.at("keep").get<std::string>();
            if (side!="positive" && side!="negative") throw std::runtime_error("Choose the positive or negative half-space to keep.");
            auto prepared=prepare_plane_cut(*object,args.at("instance_id"),args.at("part_ids").get<std::vector<std::string>>(),geometry_arg(args,"point_mm"),geometry_arg(args,"normal"),side=="positive");
            details={{"removed_mm3",prepared.removed_mm3},{"paint_note","Paint on rebuilt and capped faces is approximate; inspect the result."}};
            p.take_snapshot("Assistant: plane cut"); prepared.apply(*object);
        } else if (tool=="add_primitive") {
            const auto type=args.at("type").get<std::string>();
            if (type!="model_part" && type!="negative") throw std::runtime_error("Choose printable model_part or negative.");
            auto prepared=prepare_primitive(*object,args.at("instance_id"),args.at("shape"),type=="negative",geometry_arg(args,"center_mm"),geometry_arg(args,"size_mm"),geometry_arg(args,"axis"));
            const auto name=args.value("name",type=="negative"?std::string("Negative ")+args.at("shape").get<std::string>():args.at("shape").get<std::string>());
            if (name.empty() || name.size()>200) throw std::runtime_error("Use a short part name.");
            p.take_snapshot("Assistant: add primitive part");
            auto* volume=object->add_volume(std::move(prepared.mesh),prepared.type);
            volume->set_transformation(Geometry::Transformation(prepared.transform)); volume->name=name;
            volume->source.is_from_builtin_objects=true; object->invalidate_bounding_box();
            details={{"added_part_id",std::to_string(volume->id().id)},{"type",type},{"size_mm",args.at("size_mm")},{"center_plate_mm",args.at("center_mm")}};
        } else {
            const auto indices=removable_parts(*object,args.at("part_ids").get<std::vector<std::string>>());
            p.take_snapshot("Assistant: remove parts");
            for (const auto i:indices) object->delete_volume(i);
            object->invalidate_bounding_box(); details={{"parts_removed",indices.size()}};
        }
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor); p.select_view_3D("3D");
        p.canvas3D()->get_selection().clear();
        p.sidebar().obj_list()->add_volumes_to_object_in_list(index);
        // changed_object() would silently drop the model to the bed. Mesh
        // refresh invalidates slicing while retaining the inspected placement.
        p.changed_mesh(int(index));
        p.get_partplate_list().reset_thumbnail_assembly_view_data();
        select(p,{id(object)});
        auto result=inspect();result["geometry_edit"]=details;result["geometry_edit"]["affected_instances"]=object->instances.size();
        return result;
    }
    if (tool=="select_parts" || tool=="assign_part_filament" || tool=="split_parts") {
        auto* object=find_object(p,args.at("object_id"));
        const auto parts=part_targets(*object,args.at("part_ids").get<std::vector<std::string>>());
        if (tool=="assign_part_filament") {
            const int slot=args.at("slot"); const bool replace=args.value("replace_color_paint",false);
            validate_part_filament(parts,slot,wxGetApp().preset_bundle->filament_presets.size(),replace);
            p.take_snapshot("Assistant: assign part filament");
            assign_part_filament(parts,slot,replace);
            p.sidebar().obj_list()->sync_filament_from_model();
            // Match the native filament setter. Geometry-change notifications
            // would unnecessarily realign intentionally sunk/floating models.
            p.get_partplate_list().reset_thumbnail_assembly_view_data(); p.update();
            return inspect();
        }
        if (tool=="split_parts" && (parts.size()!=1 || !parts.front()->is_splittable()))
            throw std::runtime_error("Choose one part containing disconnected pieces. A single connected surface cannot be separated into colors by this operation; use manual color painting for that case.");
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor); p.select_view_3D("3D");
        std::vector<ObjectVolumeID> items; for (auto* part : parts) items.push_back({object,part});
        p.sidebar().obj_list()->select_items(items); p.canvas3D()->zoom_to_selection();
        if (tool=="split_parts") {
            const auto before=object->volumes.size();
            p.sidebar().obj_list()->split(true);
            if (object->volumes.size()<=before) throw std::runtime_error("Studio did not split the selected mesh. Inspect it before continuing.");
        }
        return inspect();
    }
    if (tool=="set_filament_color") {
        const int slot=args.at("slot"); const auto color=args.at("color").get<std::string>();
        auto& bundle=*wxGetApp().preset_bundle;
        auto* colors=bundle.project_config.option<ConfigOptionStrings>("filament_colour");
        if (!colors || slot<1 || size_t(slot)>colors->values.size() || !valid_color(color)) throw std::runtime_error("Choose an existing project filament slot and a hexadecimal color such as #FFFFFF.");
        p.take_snapshot("Assistant: change project filament color");
        colors->values[slot-1]=color; p.sidebar().update_presets(Preset::TYPE_FILAMENT);
        p.on_config_change(bundle.full_config()); p.update_project_dirty_from_presets(); p.update();
        return inspect();
    }
    if (tool == "select_plate") { find_plate(p, args.at("plate")); p.select_plate(args.at("plate").get<int>()-1); return inspect(); }
    if (tool == "view") {
        const auto target = args.at("target").get<std::string>();
        const std::map<std::string, MainFrame::TabPosition> tabs = {{"prepare", MainFrame::tp3DEditor}, {"preview", MainFrame::tpPreview}, {"home", MainFrame::tpHome}, {"device", MainFrame::tpMonitor}, {"project", MainFrame::tpProject}};
        if (tabs.count(target)) {
            wxGetApp().mainframe->select_tab(tabs.at(target));
            if (target=="prepare" || target=="preview") p.select_view_3D(target=="prepare"?"3D":"Preview");
        }
        else if (target == "fit") p.canvas3D()->zoom_to_selection();
        else if (target == "overhang") { wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor); p.show_view3D_overhang(true); }
        else if (target == "support_paint" || target == "seam_paint" || target == "color_paint" || target == "orient") {
            editable(p);
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            p.select_view_3D("3D");
            const auto type = target == "support_paint" ? GLGizmosManager::FdmSupports : target == "seam_paint" ? GLGizmosManager::Seam : target == "color_paint" ? GLGizmosManager::MmuSegmentation : GLGizmosManager::Flatten;
            if (!p.canvas3D()->get_gizmos_manager().open_gizmo(type)) throw std::runtime_error("Select a suitable object before opening this tool.");
        } else {
            static const std::set<std::string> views = {"top", "bottom", "front", "rear", "left", "right", "iso"};
            if (!views.count(target)) throw std::runtime_error("Unknown Studio view.");
            p.select_view(target);
        }
        return {{"shown", target}};
    }
    if (tool == "set_settings") {
        const auto scope = args.at("scope").get<std::string>();
        const auto& global = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        DynamicPrintConfig changes;
        const auto settings = args.at("settings");
        if (!settings.is_array() || settings.empty() || settings.size() > 30) throw std::runtime_error("Supply between 1 and 30 settings.");
        for (const auto& item : settings) {
            const auto key = item.at("key").get<std::string>(), value = item.at("value").get<std::string>();
            const auto* def = global.def()->get(key);
            if (!global.has(key) || !def || def->readonly || def->is_code || key.find("gcode") != std::string::npos || key.find("script") != std::string::npos || key.find("path") != std::string::npos || value.size() > 1024)
                throw std::runtime_error("That process setting cannot be changed by the assistant.");
            auto* option = changes.option(key, true);
            if (!option || !option->deserialize(value)) throw std::runtime_error("The setting value could not be read: " + key);
            if (def->type == coBool && value != "0" && value != "1") throw std::runtime_error("Boolean settings require 0 or 1.");
            if (def->type == coEnum && std::find(def->enum_values.begin(), def->enum_values.end(), value) == def->enum_values.end()) throw std::runtime_error("Unsupported enum value for " + key);
            if (def->type == coInt || def->type == coFloat || def->type == coPercent || def->type == coFloatOrPercent) {
                size_t used = 0; const double number = std::stod(value, &used);
                if ((used != value.size() && !(used+1 == value.size() && value.back() == '%')) || !std::isfinite(number) || number < def->min || number > def->max)
                    throw std::runtime_error("The value is outside the allowed range for " + key);
                if (def->type == coInt && std::floor(number) != number) throw std::runtime_error("This setting needs an integer: " + key);
                if ((key == "support_filament" || key == "support_interface_filament") && size_t(number) > wxGetApp().preset_bundle->filament_presets.size())
                    throw std::runtime_error("That project filament slot does not exist.");
            }
        }
        ModelObject* object = scope == "object" ? find_object(p, args.at("object_id")) : nullptr;
        PartPlate* plate = scope == "plate" ? find_plate(p, args.at("plate")) : nullptr;
        if (scope != "global" && !object && !plate) throw std::runtime_error("Unknown settings scope.");
        if (object) validate_object_settings(changes);
        // Validate the whole candidate before touching the live project.
        auto candidate = wxGetApp().preset_bundle->full_config();
        if (plate) candidate.apply(*plate->config());
        if (object) candidate.apply(object->config.get());
        const auto previous_validation = candidate.validate();
        candidate.apply(changes);
        const auto validation = candidate.validate();
        for (const auto& problem : validation) if (!previous_validation.count(problem.first) || previous_validation.at(problem.first) != problem.second)
            throw std::runtime_error("Studio rejected these settings: " + problem.second);
        p.take_snapshot("Assistant: change process settings");
        if (object) {
            object->config.apply(changes);
            p.sidebar().obj_list()->object_config_options_changed({object, nullptr});
            // Refresh settings and slicing without changed_object(), which
            // realigns the object to the bed even though no mesh was edited.
        }
        else if (plate) { plate->config()->apply(changes); plate->update_slice_result_valid_state(false); p.update(); }
        else { auto process = global; process.apply(changes); wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(process); p.on_config_change(wxGetApp().preset_bundle->full_config()); }
        p.update();
        return inspect();
    }
    if (tool == "use_profile") {
        const auto kind = args.at("kind").get<std::string>(), name = args.at("name").get<std::string>();
        auto& bundle = *wxGetApp().preset_bundle;
        const auto type = kind == "printer" ? Preset::TYPE_PRINTER : kind == "process" ? Preset::TYPE_PRINT : Preset::TYPE_FILAMENT;
        const PresetCollection* profiles = kind == "printer" ? &bundle.printers : kind == "process" ? &bundle.prints : kind == "filament" ? &bundle.filaments : nullptr;
        if (!profiles) throw std::runtime_error("Unknown profile type.");
        const auto* preset = profiles->find_preset(name, false);
        if (!preset || !preset->is_visible || (kind != "printer" && !preset->is_compatible)) throw std::runtime_error("That profile is not installed or compatible with the current printer.");
        const auto old_count = bundle.filament_presets.size();
        const int slot = args.value("slot", -1);
        const auto color = args.value("color", "#FFFFFF");
        if (kind == "filament" && !valid_color(color)) throw std::runtime_error("Use a six-digit hexadecimal project color such as #FFFFFF.");
        if (kind == "filament" && (slot < 0 || size_t(slot) > old_count)) throw std::runtime_error("Choose an existing project filament slot, or 0 to add one.");
        p.take_snapshot("Assistant: choose profile");
        if (kind == "filament") {
            if (slot == 0) {
                p.sidebar().add_custom_filament(wxColour(wxString::FromUTF8(color)), name);
                if (bundle.filament_presets.size() != old_count+1) throw std::runtime_error("Studio could not add another filament slot.");
            } else {
                const auto previous = bundle.filament_presets.at(slot-1);
                bundle.set_filament_preset(slot-1,name);
                if (!p.on_filament_change(slot-1)) { bundle.set_filament_preset(slot-1, previous); return {{"cancelled", true}}; }
                p.sidebar().update_presets(Preset::TYPE_FILAMENT);
                p.on_config_change(bundle.full_config());
                p.update_project_dirty_from_presets();
                bundle.export_selections(*wxGetApp().app_config);
            }
            if (slot>0 && args.contains("color")) {
                auto* colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
                if (!colors || colors->values.size()<size_t(slot)) throw std::runtime_error("The project's filament colors are unavailable.");
                colors->values[slot-1]=color; p.sidebar().update_presets(Preset::TYPE_FILAMENT); p.on_config_change(bundle.full_config());
            }
        } else if (!wxGetApp().get_tab(type)->select_preset(name)) return {{"cancelled", true}};
        p.update(); return inspect();
    }
    if (tool == "arrange") {
        if (p.model().objects.empty()) throw std::runtime_error("Add models to the project before arranging.");
        if (p.get_partplate_list().get_curr_plate()->is_locked()) throw std::runtime_error("The current plate is locked.");
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
        p.select_view_3D("3D");
        p.select_curr_plate_all(); p.arrange();
        return {{"pending", "arrange"}};
    }
    if (tool == "slice") {
        editable(p);
        if (!p.get_partplate_list().get_curr_plate()->has_printable_instances()) throw std::runtime_error("This plate has no printable instances.");
        p.reslice();
        return {{"pending", "slice"}};
    }
    if (tool == "orient") {
        if (p.is_selection_empty()) throw std::runtime_error("Select the objects to orient first.");
        p.orient(); return {{"pending", "orient"}};
    }
    if (tool == "move_to_plate") {
        auto* target = find_plate(p, args.at("plate"));
        if (target->is_locked()) throw std::runtime_error("That plate is locked.");
        auto& selection = p.canvas3D()->get_selection();
        if (!selection.is_single_full_instance() && !selection.is_multiple_full_instance()) throw std::runtime_error("Select complete instances before moving between plates.");
        selection.center_plate(args.at("plate").get<int>()-1);
        p.select_plate(args.at("plate").get<int>()-1); p.update(); return inspect();
    }
    if (tool == "copies") {
        const int count = args.at("additional");
        if (count < 1 || count > 50) throw std::runtime_error("Add between 1 and 50 copies per action.");
        select(p, {args.at("object_id").get<std::string>()});
        auto* object = find_object(p, args.at("object_id"));
        if (object->instances.empty()) throw std::runtime_error("That object has no instance to copy.");
        const auto found = std::find(p.model().objects.begin(), p.model().objects.end(), object);
        auto& selection = p.canvas3D()->get_selection();
        selection.remove_all(); selection.add_instance(unsigned(found-p.model().objects.begin()), 0, true);
        const auto previous=p.model().objects.size();
        if (selection.is_empty()) throw std::runtime_error("Studio could not select the source instance in Prepare. No copies were added.");
        selection.clone(count);
        p.update();
        auto result=inspect(); result["copies_added"]=p.model().objects.size()-previous;
        if (p.model().objects.size()!=previous+size_t(count)) result["operation_error"]="Clone did not add the requested number of objects. Inspect current state; do not automatically repeat the copy request.";
        return result;
    }
    if (tool == "rename") {
        const auto name = args.at("name").get<std::string>();
        if (name.empty() || name.size()>180 || name.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Use a short, nonempty name.");
        if (args.contains("object_id") == args.contains("plate")) throw std::runtime_error("Choose either an object or a plate to rename.");
        auto* object = args.contains("object_id") ? find_object(p, args.at("object_id")) : nullptr;
        auto* plate = args.contains("plate") ? find_plate(p, args.at("plate")) : nullptr;
        p.take_snapshot("Assistant: rename");
        if (object) { object->name = name; p.object_list_changed(); }
        else plate->set_plate_name(name);
        p.update(); return inspect();
    }
    if (tool == "new_plate") {
        p.take_snapshot("Assistant: new plate");
        int index = p.get_partplate_list().create_plate();
        if (index < 0) throw std::runtime_error("Studio could not create another plate.");
        p.select_plate(index); p.update(); return inspect();
    }
    if (tool == "transform") {
        auto* canvas = p.canvas3D(); auto& selection = canvas->get_selection();
        if (p.is_selection_empty() || (!selection.is_single_full_instance() && !selection.is_multiple_full_instance())) throw std::runtime_error("Select a complete object or instance before transforming.");
        const Vec3d v(args.at("x").get<double>(), args.at("y").get<double>(), args.at("z").get<double>());
        for (int i=0;i<3;++i) if (!std::isfinite(v[i]) || std::abs(v[i])>1000) throw std::runtime_error("Invalid transform value.");
        const auto kind = args.at("kind").get<std::string>();
        if (kind == "scale" && (v.x() != v.y() || v.x() != v.z() || v.x()<0.01 || v.x()>10)) throw std::runtime_error("Use a uniform scale factor between 0.01 and 10.");
        if (kind != "translate" && kind != "rotate" && kind != "scale") throw std::runtime_error("Unknown transform.");
        selection.start_dragging();
        if (kind == "translate") { selection.translate(v, TransformationType::World_Relative_Joint); canvas->do_move("Assistant: move"); }
        if (kind == "rotate") { selection.rotate(v * (M_PI/180.0), TransformationType::World_Relative_Joint); canvas->do_rotate("Assistant: rotate"); }
        if (kind == "scale") { selection.scale(v, TransformationType::World_Relative_Joint); canvas->do_scale("Assistant: scale"); }
        selection.stop_dragging(); p.update(); return inspect();
    }
    if (tool == "undo") { if (!p.can_undo()) throw std::runtime_error("There is nothing to undo."); p.undo(); return inspect(); }
    if (tool == "redo") { if (!p.can_redo()) throw std::runtime_error("There is nothing to redo."); p.redo(); return inspect(); }
    if (tool == "delete_selected") { if (p.is_selection_empty()) throw std::runtime_error("Select an object first."); p.remove_selected(); return inspect(); }
    if (tool == "open_library_project") {
        const auto path = library_source(args.at("entry_id"));
        const auto result = p.load_project(wxString::FromUTF8(path));
        if (result == wxID_CANCEL) return {{"cancelled", true}};
        return unconfirmed_project_open(inspect());
    }
    if (tool == "add_library_model") return start_library(tool,args);
    if (tool == "document") {
        const auto action = args.at("action").get<std::string>();
        if (action == "open") {
            if (p.load_project() == wxID_CANCEL) return {{"cancelled", true}};
            return unconfirmed_project_open(inspect());
        }
        else if (action == "import") {
            p.add_file();
            auto result=inspect();
            result["outcome"]="unconfirmed";
            result["status"]="The import chooser closed. This does not confirm a model was added.";
            result["instruction"]="Compare objects and parts with the state before import. Honor Studio's busy/loading state; loading or SVG processing may still be running. Do not claim an import succeeded or reopen the chooser solely because it closed.";
            return result;
        }
        else if (action == "save" || action == "save_as") {
            if (p.save_project(action == "save_as") != wxID_YES) return {{"cancelled", true}};
        } else if (action == "export_sliced") {
            if (!p.get_partplate_list().get_curr_plate()->is_slice_result_ready_for_export()) throw std::runtime_error("Slice this plate successfully before exporting.");
            p.export_gcode_3mf(false);
            return {{"outcome", "unconfirmed"}, {"status", "The export dialog was opened. The person must confirm whether a file was saved."}};
        } else throw std::runtime_error("Unknown document action.");
        return inspect();
    }
    throw std::runtime_error("That Studio operation is not available.");
}

Json Actions::add_library_object(const std::string& staged, const std::string& original, const Json& args)
{
        auto& p = plater(); editable(p);
        if (args.at("revision") != inspect().at("revision")) throw std::runtime_error("The project changed while reading the source. Inspect again before importing.");
        DynamicPrintConfig source_config;
        auto model = read_source_model(staged,source_config);
        const auto name = args.at("object_name").get<std::string>();
        ModelObject* source = nullptr;
        for (auto* object : model.objects) if (object->name == name) {
            if (source) throw std::runtime_error("Multiple source objects have that name. Open the project and select the intended object.");
            source = object;
        }
        if (!source || source->instances.empty()) throw std::runtime_error("That source model was not found.");
        const auto destination = wxGetApp().preset_bundle->full_config();
        const size_t slots = wxGetApp().preset_bundle->filament_presets.size();
        std::optional<std::vector<int>> explicit_map;
        if (args.contains("filament_map")) explicit_map=args.at("filament_map").get<std::vector<int>>();
        const auto mapping=source_filament_mapping(*source,source_config,destination,slots,explicit_map);
        apply_source_filament_mapping(*source,mapping);
        for (auto* volume : source->volumes) {
            volume->source.input_file=original;
        }
        source->input_file=original;
        p.take_snapshot("Assistant: add library model");
        auto* added = copy_source_object(p.model(),*source);
        const auto center = p.get_partplate_list().get_curr_plate()->get_center_origin();
        const auto box = added->bounding_box();
        added->instances[0]->set_offset(added->instances[0]->get_offset() + Vec3d(center.x()-box.center().x(), center.y()-box.center().y(), -box.min.z()));
        added->invalidate_bounding_box();
        p.model().set_assembly_pos(added);
        p.sidebar().obj_list()->add_object_to_list(p.model().objects.size()-1);
        p.changed_objects({p.model().objects.size()-1});
        p.update(); return inspect();
}
}}}
