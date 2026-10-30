#ifndef CHARMS_ASSISTANT_WORKBENCH_EVAL_HPP
#define CHARMS_ASSISTANT_WORKBENCH_EVAL_HPP
// Opt-in headless reasoning checks. The model operates disposable native models
// through the SAME geometry/painting/text/slicing helpers used by the GUI. This
// does not substitute for GUI integration checks and never sends a print.
#include "slic3r/GUI/StudioAssistantClient.hpp"
#include "slic3r/GUI/StudioAssistantGeometry.hpp"
#include "slic3r/GUI/StudioAssistantSurfaces.hpp"
#include "slic3r/GUI/StudioAssistantSurfaceView.hpp"
#include "slic3r/GUI/StudioAssistantTransforms.hpp"
#include "slic3r/GUI/StudioAssistantMeshEdits.hpp"
#include "slic3r/GUI/StudioAssistantText.hpp"
#include "slic3r/GUI/StudioAssistantSlicing.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "miniz.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace CharmsWorkbenchEval {
using namespace Slic3r;using namespace Slic3r::GUI::Assistant;
inline Json vec(const Vec3d& value){return {value.x(),value.y(),value.z()};}
inline Vec3d point(const Json& args,const char* key){const auto v=args.at(key).get<std::vector<double>>();if(v.size()!=3)throw std::runtime_error("Use three coordinates.");Vec3d out(v[0],v[1],v[2]);geometry_vector(out);return out;}
struct State {
    Model model;DynamicPrintConfig config;std::unique_ptr<Print> print;bool fresh{false};size_t generation{0},image_number{0};
    std::filesystem::path output;std::map<std::string,Json> surfaces;Json selected=Json::array();
    Json fixture_limitations=Json::array();
    std::map<std::string,std::pair<Transform3d,const TriangleMesh*>> surface_sources;
    std::unique_ptr<Model> previous;DynamicPrintConfig previous_config;
    ModelObject* object(const std::string& id){for(auto* o:model.objects)if(std::to_string(o->id().id)==id)return o;throw std::runtime_error("Stale object; inspect again.");}
    ModelVolume* part(ModelObject& o,const std::string& id){for(auto* p:o.volumes)if(std::to_string(p->id().id)==id)return p;throw std::runtime_error("Stale part; inspect again.");}
    const Json& surface(const std::string& id){const auto& s=surfaces.at(id);auto* o=object(s.at("object_id"));auto* p=part(*o,s.at("part_id"));const auto& source=surface_sources.at(id);if(source.second!=&p->mesh()||!source.first.matrix().isApprox((geometry_instance(*o,s.at("instance_id"))*p->get_matrix()).matrix()))throw std::runtime_error("Surface geometry changed; inspect surfaces again.");return s;}
    BoundingBoxf3 bounds(const ModelObject& o,const ModelInstance& i)const{BoundingBoxf3 result;for(const auto* p:o.volumes)if(p->is_model_part())result.merge(p->mesh().transformed_bounding_box(i.get_matrix()*p->get_matrix()));return result;}
    std::string revision()const{return "fixture-"+std::to_string(generation);}
    void changing(){previous=std::make_unique<Model>(model);previous_config=config;fresh=false;++generation;}
    Json snapshot()const {
        Json objects=Json::array(),members=Json::array(),settings=Json::object(),warnings=Json::array();size_t count=0;
        if(fresh && print){auto append=[&](const PrintStateBase::Warning& w,const std::string& id){if(w.current)warnings.push_back({{"origin","slicer"},{"plate",1},{"object_id",id},{"message",w.message},{"level",w.level==PrintStateBase::WarningLevel::CRITICAL?"critical":"warning"},{"slice_fresh",true}});};for(int s=0;s<psCount;++s)for(const auto& w:print->step_state_with_warnings(PrintStep(s)).warnings)append(w,"");for(const auto* o:print->objects())for(int s=0;s<posCount;++s)for(const auto& w:o->step_state_with_warnings(PrintObjectStep(s)).warnings)append(w,std::to_string(o->model_object()->id().id));}
        for(const auto* o:model.objects){Json volumes=Json::array(),instances=Json::array();
            for(const auto* p:o->volumes){const auto b=p->mesh().transformed_bounding_box(p->get_matrix());Json text=nullptr;if(p->is_text())text={{"content",p->get_text_info().m_text},{"font",p->get_text_info().m_font_name}};
                volumes.push_back({{"id",std::to_string(p->id().id)},{"name",p->name},{"kind",int(p->type())},{"model_part",p->is_model_part()},{"negative",p->is_negative_volume()},{"filament_slot",p->extruder_id()},{"support_painted",!p->supported_facets.empty()},{"color_painted",!p->mmu_segmentation_facets.empty()},{"size_mm",vec(b.size())},{"min_object_mm",vec(b.min)},{"max_object_mm",vec(b.max)},{"text",text},{"settings",Json::object()}});}
            for(size_t n=0;n<o->instances.size();++n){const auto* i=o->instances[n];Json matrix=Json::array(),parts=Json::array();for(int r=0;r<4;++r)for(int c=0;c<4;++c)matrix.push_back(i->get_matrix()(r,c));for(const auto* p:o->volumes){const auto b=p->mesh().transformed_bounding_box(i->get_matrix()*p->get_matrix());parts.push_back({{"part_id",std::to_string(p->id().id)},{"min_mm",vec(b.min)},{"max_mm",vec(b.max)}});}
                instances.push_back({{"id",std::to_string(i->id().id)},{"offset_mm",vec(i->get_offset())},{"transform",matrix},{"printable",i->printable},{"part_bounds_plate_mm",parts}});members.push_back({{"object_id",std::to_string(o->id().id)},{"instance_index",n}});++count;}
            const auto b=o->bounding_box();objects.push_back({{"id",std::to_string(o->id().id)},{"name",o->name},{"volumes",volumes},{"instances",instances},{"size_mm",vec(b.size())},{"min_mm",vec(b.min)},{"max_mm",vec(b.max)},{"overrides",Json::object()}});
        }
        for(const auto* key:{"printer_model","printer_settings_id","print_settings_id","filament_settings_id","filament_type","filament_colour","nozzle_diameter","layer_height","enable_support","support_type","support_style","support_on_build_plate_only","support_filament","support_interface_filament","support_top_z_distance","support_bottom_z_distance","wall_loops","sparse_infill_density"})if(config.has(key))settings[key]=config.opt_serialize(key);
        return {{"project","Disposable native workbench evaluation"},{"revision",revision()},{"objects",objects},{"plates",Json::array({{{"plate",1},{"name","Evaluation"},{"size_mm",{180,180}},{"origin_mm",{0,0,0}},{"members",members},{"locked",false},{"slice_valid",fresh}}})},{"current_plate",1},{"settings",settings},{"warnings",warnings},{"busy",false},{"editable",true},{"total_instances",count},{"selected_object_ids",selected},{"selected_parts",Json::array()},{"can_undo",bool(previous)},{"can_redo",false}};
    }
    Json image(SurfaceRaster raster,const std::string& direction,Json legend=Json::array()) {
        static const unsigned digits[]={0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
        auto pixel=[&](int x,int y,unsigned char value){if(x>=0&&y>=0&&x<768&&y<768)for(int k=0;k<3;++k)raster.rgb[(y*768+x)*3+k]=value;};
        auto line=[&](int x,int y,int w,int h,unsigned char value){for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixel(xx,yy,value);};
        for(auto& entry:legend){int label=entry.at("number");entry["visible_pixels"]=raster.visible_pixels[label];if(raster.visible_pixels[label]<20)continue;const auto at=raster.anchors.at(label);const auto text=std::to_string(label);const int left=at.first-int(text.size())*7;line(left-3,at.second-12,int(text.size())*14+4,24,25);
            for(size_t c=0;c<text.size();++c){const unsigned bits=digits[text[c]-'0'];int x=left+int(c)*14,y=at.second-9;if(bits&1)line(x+2,y,8,2,255);if(bits&2)line(x+10,y+2,2,6,255);if(bits&4)line(x+10,y+10,2,6,255);if(bits&8)line(x+2,y+16,8,2,255);if(bits&16)line(x,y+10,2,6,255);if(bits&32)line(x,y+2,2,6,255);if(bits&64)line(x+2,y+8,8,2,255);}}
        size_t size=0;std::unique_ptr<void,decltype(&mz_free)> png(tdefl_write_image_to_png_file_in_memory_ex(raster.rgb.data(),768,768,3,&size,6,false),mz_free);if(!png)throw std::runtime_error("Could not encode the native fixture view.");
        const auto path=output/("view-"+std::to_string(++image_number)+".png");std::ofstream file(path,std::ios::binary);file.write(static_cast<const char*>(png.get()),size);
        auto encoded=base64url(std::string(static_cast<const char*>(png.get()),size));std::replace(encoded.begin(),encoded.end(),'-','+');std::replace(encoded.begin(),encoded.end(),'_','/');while(encoded.size()%4)encoded+='=';
        return {{"image_data","data:image/png;base64,"+encoded},{"direction",direction},{"legend",legend},{"note","Headless native geometry diagram; original filament colors are not represented."}};
    }
    Json slice_info()const{if(!fresh||!print)throw std::runtime_error("Slice first.");Json list=Json::array();for(const auto* o:print->objects())list.push_back({{"object_id",std::to_string(o->model_object()->id().id)},{"model_layers",o->layers().size()},{"support_layers",o->support_layers().size()},{"first_print_z_mm",o->layers().empty()?0:o->layers().front()->print_z},{"last_print_z_mm",o->layers().empty()?0:o->layers().back()->print_z}});return {{"plate",1},{"objects",list}};}
    Json execute(const std::string& tool,const Json& args){
        if(args.contains("revision") && args.at("revision")!=revision())throw std::runtime_error("Stale revision. Inspect again.");
        if(tool=="inspect_project")return snapshot();
        if(tool=="model_view"){std::vector<SurfaceRenderPart> parts;for(auto* o:model.objects)for(auto* i:o->instances)for(auto* p:o->volumes)if(p->is_model_part())parts.push_back({p,i->get_matrix()*p->get_matrix(),{}});return image(render_surface_map(parts,args.value("direction","iso")),args.value("direction","iso"));}
        if(tool=="view")return {{"shown",args.at("target")}};
        if(tool=="select_objects"){selected=args.at("object_ids");++generation;return snapshot();}
        if(tool=="select_parts")return snapshot();
        if(tool=="inspect_settings"){Json matches=Json::array();const auto query=args.at("query").get<std::string>();for(const auto& key:config.keys()){const auto* d=config.def()->get(key);if(key.find(query)==std::string::npos||!d||d->is_code||d->readonly)continue;matches.push_back({{"key",key},{"label",d->label},{"description",d->tooltip},{"value",config.opt_serialize(key)},{"type",int(d->type)},{"min",d->min},{"max",d->max},{"enum_values",d->enum_values},{"valid_scopes",{"global","object"}}});if(matches.size()==50)break;}return {{"settings",matches}};}
        if(tool=="set_settings"){DynamicPrintConfig changes;for(const auto& item:args.at("settings")){const auto key=item.at("key").get<std::string>(),value=item.at("value").get<std::string>();const auto* def=config.def()->get(key);if(!config.has(key)||!def||def->is_code||def->readonly||key.find("gcode")!=std::string::npos||key.find("path")!=std::string::npos)throw std::runtime_error("Unsupported process setting.");auto* option=changes.option(key,true);if(!option||!option->deserialize(value))throw std::runtime_error("Invalid setting value.");}
            if(args.at("scope")!="object" && args.at("scope")!="global"){fixture_limitations.push_back("set_settings:"+args.at("scope").get<std::string>());throw std::runtime_error("This bounded fixture covers global/object settings only.");}changing();if(args.at("scope")=="object")object(args.at("object_id"))->config.apply(changes);else if(args.at("scope")=="global")config.apply(changes);else throw std::runtime_error("This bounded fixture covers global/object settings only.");return snapshot();}
        if(tool=="inspect_surfaces"){auto* o=object(args.at("object_id"));const auto matrix=geometry_instance(*o,args.at("instance_id"));Json entries=Json::array();const auto filter=args.value("filter","all");
            for(auto* p:part_targets(*o,args.at("part_ids").get<std::vector<std::string>>()))for(const auto& patch:surface_patches(*p,matrix*p->get_matrix())){if((filter=="downward"&&patch.normal.z()>=-.1)||(filter=="upward"&&patch.normal.z()<=.1)||(filter=="vertical"&&std::abs(patch.normal.z())>.2)||patch.area<args.value("minimum_area_mm2",0.))continue;if(entries.size()>=args.value("limit",64))break;
                const auto id="surface-"+std::to_string(p->id().id)+"-"+std::to_string(patch.seed);Json record={{"surface_id",id},{"object_id",args.at("object_id")},{"part_id",std::to_string(p->id().id)},{"instance_id",args.at("instance_id")},{"faces",patch.faces},{"area_mm2",patch.area},{"normal_plate",vec(patch.normal)},{"sample_normal_plate",vec(patch.sample_normal)},{"center_plate_mm",vec(patch.center)},{"sample_point_plate_mm",vec(patch.sample)},{"min_plate_mm",vec(patch.bounds.min)},{"max_plate_mm",vec(patch.bounds.max)}};surfaces[id]=record;surface_sources[id]={matrix*p->get_matrix(),&p->mesh()};record.erase("faces");entries.push_back(record);}
            return {{"revision",revision()},{"surfaces",entries}};}
        if(tool=="surface_view"){Json legend=Json::array();std::map<std::string,std::map<int,int>> labels;ModelObject* o=nullptr;Transform3d matrix=Transform3d::Identity();int number=0;for(const auto& id:args.at("surface_ids")){const auto& s=surface(id.get<std::string>());o=object(s.at("object_id"));matrix=geometry_instance(*o,s.at("instance_id"));++number;for(int f:s.at("faces").get<std::vector<int>>())labels[s.at("part_id").get<std::string>()][f]=number;legend.push_back({{"number",number},{"surface_id",id},{"part_id",s.at("part_id")},{"normal_plate",s.at("normal_plate")}});}std::vector<SurfaceRenderPart> parts;for(auto* p:o->volumes)if(p->is_model_part())parts.push_back({p,matrix*p->get_matrix(),labels[std::to_string(p->id().id)]});return image(render_surface_map(parts,args.at("direction")),args.at("direction"),legend);}
        if(tool=="paint_surfaces"){struct Job{ModelVolume* part;PaintKeepPrepared paint;};std::vector<std::unique_ptr<Job>> jobs;const auto layer=args.at("layer").get<std::string>(),action=args.at("action").get<std::string>();const int state=action=="erase"?0:action=="block"?2:action=="color"?args.at("slot").get<int>():1;
            std::map<std::string,std::vector<int>> grouped;std::map<std::string,Json> targets;for(const auto& id:args.at("surface_ids")){const auto& s=surface(id.get<std::string>());const auto key=s.at("part_id").get<std::string>();const auto faces=s.at("faces").get<std::vector<int>>();grouped[key].insert(grouped[key].end(),faces.begin(),faces.end());targets[key]=s;}
            for(auto& entry:grouped){const auto& s=targets.at(entry.first);auto* o=object(s.at("object_id"));auto* p=part(*o,s.at("part_id"));auto job=std::make_unique<Job>();job->part=p;std::sort(entry.second.begin(),entry.second.end());entry.second.erase(std::unique(entry.second.begin(),entry.second.end()),entry.second.end());if(args.contains("center_mm"))job->paint=std::move(prepared_surface_brush(*p,paint_layer(*p,layer),geometry_instance(*o,s.at("instance_id"))*p->get_matrix(),entry.second,state,point(args,"center_mm"),args.at("radius_mm")).paint);else job->paint=prepared_surface_paint(*p,paint_layer(*p,layer),entry.second,state);jobs.push_back(std::move(job));}changing();for(auto& job:jobs)paint_layer(*job->part,layer).assign(std::move(job->paint.supported));return snapshot();}
        if(tool=="slice" || tool=="slice_all"){print=std::make_unique<Print>();print->set_status_silent();print->apply(model,config);print->process();fresh=true;++generation;return snapshot();}
        if(tool=="inspect_slice")return slice_info();
        if(tool=="slice_layer_view"){if(!fresh||!print)throw std::runtime_error("Slice first.");const auto scene=collect_slice_scene(*print,args.at("print_z_mm"),args.value("object_id",std::string()),300000,args.value("instance_id",std::string()));
            Model diagram;std::vector<SurfaceRenderPart> parts;for(const auto* shapes:{&scene.solids,&scene.supports})if(!shapes->empty()){auto* o=diagram.add_object("Sliced region","",TriangleMesh(slices_to_mesh({*shapes},0,.01,.01)));auto* i=o->add_instance();auto* p=o->volumes.front();parts.push_back({p,i->get_matrix()*p->get_matrix(),{}});}if(parts.empty())throw std::runtime_error("No sliced regions at that height.");auto result=image(render_surface_map(parts,"top"),"top");result["support_paths"]=scene.support_paths.size();result["support_regions"]=scene.supports.size();result["actual_print_heights_mm"]=scene.print_heights;result["note"]="Native sliced solid/support regions; this headless fixture diagram omits individual extrusion lines.";return result;}
        if(tool=="model_section"){auto* o=object(args.at("object_id"));const auto at=point(args,"point_mm"),normal=point(args,"normal");const auto frame=geometry_frame(at,normal);const auto section=model_section(*o,args.at("instance_id"),at,normal);Json contours=Json::array();double area=0;for(const auto& p:section){area+=p.area();auto add=[&](const Polygon& ring,bool hole){Json points=Json::array();for(const auto& q:ring.points)points.push_back({unscale<double>(q.x()),unscale<double>(q.y())});contours.push_back({{"hole",hole},{"points_mm",points}});};add(p.contour,false);for(const auto& hole:p.holes)add(hole,true);}return {{"origin_plate_mm",vec(at)},{"basis_u",vec(frame.linear().col(0))},{"basis_v",vec(frame.linear().col(1))},{"area_mm2",area*SCALING_FACTOR*SCALING_FACTOR},{"contours",contours},{"contours_truncated",false}};}
        if(tool=="plane_cut"){auto* o=object(args.at("object_id"));auto cut=prepare_plane_cut(*o,args.at("instance_id"),args.at("part_ids").get<std::vector<std::string>>(),point(args,"point_mm"),point(args,"normal"),args.at("keep")=="positive");changing();cut.apply(*o);return snapshot();}
        if(tool=="add_primitive"){auto* o=object(args.at("object_id"));const auto type=args.at("type").get<std::string>();auto primitive=prepare_primitive(*o,args.at("instance_id"),args.at("shape"),type=="negative",point(args,"center_mm"),point(args,"size_mm"),point(args,"axis"));if(type=="modifier")primitive.type=ModelVolumeType::PARAMETER_MODIFIER;else if(type=="support_enforcer")primitive.type=ModelVolumeType::SUPPORT_ENFORCER;else if(type=="support_blocker")primitive.type=ModelVolumeType::SUPPORT_BLOCKER;changing();auto* p=o->add_volume(std::move(primitive.mesh),primitive.type);p->set_transformation(Geometry::Transformation(primitive.transform));p->name=args.value("name",type);o->invalidate_bounding_box();return snapshot();}
        if(tool=="create_object"){Model staging;auto* dummy=staging.add_object();auto* i=dummy->add_instance();auto shape=prepare_primitive(*dummy,std::to_string(i->id().id),args.at("shape"),false,Vec3d::Zero(),point(args,"size_mm"),args.contains("axis")?point(args,"axis"):Vec3d::UnitZ());shape.mesh.transform(shape.transform,true);const auto size=shape.mesh.bounding_box().size();changing();auto* o=model.add_object(args.value("name",std::string("Shape")).c_str(),"",std::move(shape.mesh));auto* instance=o->add_instance();const auto at=args.contains("center_mm")?point(args,"center_mm"):Vec3d(90,90,size.z()/2);instance->set_offset(at);o->config.set("extruder",args.value("slot",1));return snapshot();}
        if(tool=="transform_item" || tool=="place_on_bed"){auto* o=object(args.at("object_id"));auto edit=prepare_item_transform(*o,args.at("instance_id"),args.value("part_ids",std::vector<std::string>()),tool=="place_on_bed"?"bed":args.at("kind").get<std::string>(),tool=="place_on_bed"?Vec3d::Zero():point(args,"values"),args.contains("pivot_mm")?std::optional<Vec3d>(point(args,"pivot_mm")):std::nullopt);changing();edit.apply();return snapshot();}
        if(tool=="delete_parts"){auto* o=object(args.at("object_id"));const auto indices=removable_parts(*o,args.at("part_ids").get<std::vector<std::string>>());changing();for(size_t i:indices)o->delete_volume(i);o->invalidate_bounding_box();return snapshot();}
        if(tool=="list_fonts")return {{"font_names",{"Arial"}},{"collection_faces",Json::array({{{"index",0},{"name","Arial"}}})}};
        if(tool=="add_text"){if(args.value("font",std::string("Arial"))!="Arial"){fixture_limitations.push_back("add_text:font");throw std::runtime_error("This disposable headless fixture uses installed Arial only; the real GUI enumerates all fonts.");}auto file=Emboss::create_font_file("/System/Library/Fonts/Supplemental/Arial.ttf");Emboss::FontFileWithCache font(std::move(file));EmbossStyle style;style.name="Arial";auto text=prepare_text_mesh(font,style,args.at("text"),args.at("height_mm"),args.at("depth_mm"));auto* o=object(args.at("object_id"));const auto matrix=geometry_instance(*o,args.at("instance_id"));auto frame=geometry_frame(point(args,"center_mm"),args.contains("axis")?point(args,"axis"):Vec3d::UnitZ());frame.rotate(Eigen::AngleAxisd(args.value("rotation_deg",0.)*PI/180,Vec3d::UnitZ()));changing();auto* p=o->add_volume(std::move(text.mesh),args.value("negative",false)?ModelVolumeType::NEGATIVE_VOLUME:ModelVolumeType::MODEL_PART);p->set_transformation(Geometry::Transformation(matrix.inverse()*frame));TextInfo info;info.m_text=args.at("text");info.m_font_name="Arial";info.m_font_size=text.style.prop.size_in_mm;info.m_thickness=float(text.depth_mm);info.text_configuration.text=info.m_text;info.text_configuration.style=text.style;p->set_text_info(info);p->name="Lettering";o->invalidate_bounding_box();return snapshot();}
        if(tool=="undo" && previous){auto copy=std::make_unique<Model>(model);auto cfg=config;model=*previous;config=previous_config;previous=std::move(copy);previous_config=cfg;fresh=false;surfaces.clear();++generation;return snapshot();}
        fixture_limitations.push_back(tool);throw std::runtime_error("This bounded headless fixture does not exercise that UI operation. The real Studio app exposes its full declared native tool; this is an evaluation-harness limitation.");
    }
};
inline std::string prepare_fixture(State& state,const std::string& scenario)
{
    state.config=DynamicPrintConfig::full_print_config();state.config.set_key_value("layer_height",new ConfigOptionFloat(.2));state.config.set_key_value("initial_layer_print_height",new ConfigOptionFloat(.2));state.config.set_key_value("nozzle_diameter",new ConfigOptionFloats({.4}));state.config.set_key_value("enable_support",new ConfigOptionBool(false));
    if(scenario=="supports") {
        auto mesh=make_cube(20,20,3);mesh.translate(Vec3f(-10,-10,-1.5));auto* object=state.model.add_object("Overhanging top","",std::move(mesh));auto* instance=object->add_instance();instance->set_offset(Vec3d(90,90,7.5));
        auto pillar=prepare_primitive(*object,std::to_string(instance->id().id),"box",false,Vec3d(90,90,3.1),Vec3d(2,2,6.2),Vec3d::UnitZ());auto* post=object->add_volume(std::move(pillar.mesh));post->set_transformation(Geometry::Transformation(pillar.transform));auto* cap=object->volumes.front();auto joined=prepare_mesh_boolean(*object,std::to_string(instance->id().id),*cap,{post},"union");cap->commit_mesh_keep_paint(std::move(joined));object->delete_volume(1);object->invalidate_bounding_box();
        return "Please paint supports just under the wide overhanging top, leaving the narrow post clear, and check the sliced result. Keep the model shape and placement unchanged.";
    }
    if(scenario=="loop") {
        auto xy=[](double x,double y){return Slic3r::Point(scale_(x),scale_(y));};ExPolygon outline(Polygon({xy(0,0),xy(20,0),xy(20,30),xy(14,30),xy(14,36),xy(6,36),xy(6,30),xy(0,30)}));outline.holes.push_back(Polygon({xy(8,31),xy(8,34),xy(12,34),xy(12,31)}));auto* object=state.model.add_object("Earring with a loop","",TriangleMesh(slices_to_mesh({{outline}},0,3,3)));object->add_instance()->set_offset(Vec3d(80,75,0));
        return "Please remove the loop on the upper edge and replace it with a 2 mm through-hole, centered 3 mm down from the new top edge. Keep the body dimensions and placement unchanged.";
    }
    if(scenario=="keychain")return "Please make a plain 30 by 20 by 3 mm keychain, add a 3 mm through-hole 4 mm in from its left edge and centered vertically, and add raised Mom lettering 5 mm high in the middle. Keep it flat on the bed.";
    throw std::runtime_error("Choose supports, loop, or keychain.");
}
inline Json verify(const State& state,const Model& initial_model,const std::string& scenario,bool completed)
{
    Json report=Json::object();
    bool passed=completed&&state.fresh;
    if(scenario=="loop" && state.model.objects.size()!=initial_model.objects.size())passed=false;
    if(scenario=="supports") {
        bool unchanged=state.model.objects.size()==initial_model.objects.size(),supports=false;double area=0,wrong_area=0;
        for(size_t n=0;n<state.model.objects.size();++n){const auto* o=state.model.objects[n];if(n>=initial_model.objects.size()){unchanged=false;continue;}const auto* original=initial_model.objects[n];unchanged&=o->volumes.size()==original->volumes.size()&&o->instances.size()==original->instances.size();
            for(size_t k=0;k<o->instances.size();++k)unchanged&=k<original->instances.size()&&o->instances[k]->get_matrix().matrix().isApprox(original->instances[k]->get_matrix().matrix(),1e-10);
            for(size_t k=0;k<o->volumes.size();++k){const auto* p=o->volumes[k];if(k>=original->volumes.size()){unchanged=false;continue;}const auto& a=p->mesh().its;const auto& b=original->volumes[k]->mesh().its;unchanged&=p->get_matrix().matrix().isApprox(original->volumes[k]->get_matrix().matrix(),1e-10)&&a.vertices==b.vertices&&a.indices==b.indices;
                if(o->instances.empty())continue;const auto matrix=o->instances.front()->get_matrix()*p->get_matrix();const auto painted=p->supported_facets.get_facets_strict(*p,EnforcerBlockerType::ENFORCER);for(const auto& f:painted.indices){const Vec3d at=matrix*painted.vertices[f[0]].cast<double>(),u=matrix*painted.vertices[f[1]].cast<double>(),v=matrix*painted.vertices[f[2]].cast<double>();Vec3d normal=(u-at).cross(v-at);const double size=normal.norm()/2;area+=size;if(size>0 && (normal.normalized().z()>-.99||std::min({at.z(),u.z(),v.z()})<1))wrong_area+=size;}}
        }
        if(state.print)for(const auto* o:state.print->objects())supports|=!o->support_layers().empty();passed&=unchanged&&supports&&area>100&&wrong_area<.01;report["geometry_and_placement_unchanged"]=unchanged;report["enforced_area_mm2"]=area;report["paint_outside_target_underside_mm2"]=wrong_area;report["actual_support_layers_generated"]=supports;
    }
    if(scenario=="loop" && !state.model.objects.empty()) {
        const auto* o=state.model.objects.front();const auto* i=o->instances.front();const auto b=state.bounds(*o,*i);bool through=true;
        for(double z:{.25,1.5,2.75}){const auto section=model_section(*o,std::to_string(i->id().id),Vec3d(b.center().x(),b.center().y(),b.min.z()+z),Vec3d::UnitZ());size_t holes=0;bool round=false;for(const auto& p:section)for(const auto& hole:p.holes){++holes;const auto box=hole.bounding_box();round|=std::abs(unscale<double>(box.size().x())-2)<.03 && std::abs(unscale<double>(box.size().y())-2)<.03 && std::abs(unscale<double>(box.center().x()))<.03 && std::abs(unscale<double>(box.center().y())-(b.size().y()/2-3))<.03;}through&=holes==1&&round;}
        const auto* original=initial_model.objects.front();const bool placement=b.min.isApprox(state.bounds(*original,*original->instances.front()).min,1e-8) && i->get_matrix().matrix().isApprox(original->instances.front()->get_matrix().matrix(),1e-10);
        passed&=b.size().isApprox(Vec3d(20,30,3),.001)&&through&&placement;report["body_dimensions_mm"]=vec(b.size());report["hole_verified_at_three_depths"]=through;report["placement_unchanged"]=placement;
    }
    if(scenario=="keychain") {
        bool lettering=false,hole=false,body=false;BoundingBoxf3 base;
        for(const auto* o:state.model.objects)for(const auto* i:o->instances)for(const auto* p:o->volumes)if(p->is_model_part() && !p->is_text()){const auto b=p->mesh().transformed_bounding_box(i->get_matrix()*p->get_matrix());if(b.size().isApprox(Vec3d(30,20,3),.001) && std::abs(b.min.z())<.01){body=true;base.merge(b);}}
        for(const auto* o:state.model.objects){const auto* i=o->instances.front();for(const auto* p:o->volumes)if(p->is_text() && p->is_model_part() && base.defined){const auto b=p->mesh().transformed_bounding_box(i->get_matrix()*p->get_matrix());lettering|=p->get_text_info().m_text=="Mom" && std::abs(b.size().y()-5)<.03 && b.max.z()>base.max.z()+.1 && b.min.z()<=base.max.z()+.01 && (b.center().head<2>()-base.center().head<2>()).norm()<1.;}
            if(base.defined)for(double z:{.25,1.5,2.75}){bool found=false;const auto section=model_section(*o,std::to_string(i->id().id),Vec3d(base.center().x(),base.center().y(),z),Vec3d::UnitZ());for(const auto& p:section){const auto b=p.contour.bounding_box();for(const auto& h:p.holes){const auto hb=h.bounding_box();found|=std::abs(unscale<double>(hb.size().x())-3)<.03 && std::abs(unscale<double>(hb.size().y())-3)<.03 && std::abs(unscale<double>(hb.center().x())-(unscale<double>(b.min.x())+4))<.03 && std::abs(unscale<double>(hb.center().y())-unscale<double>(b.center().y()))<.03;}}if(z==.25)hole|=found;else hole&=found;}
        }
        passed&=lettering&&hole&&body;report["centered_raised_lettering_verified"]=lettering;report["hole_verified_at_three_depths"]=hole;report["body_verified"]=body;
    }
    report["passed"]=passed;return report;
}
inline Json run(Client& client,const Json& account,const std::string& model_name,const std::string& scenario,const std::filesystem::path& root)
{
    State state;state.output=root;std::filesystem::create_directories(root);std::filesystem::permissions(root,std::filesystem::perms::owner_all);set_resources_dir(PROJECT_RESOURCES);set_data_dir(root.string());set_temporary_dir(root.string());
    const auto prompt=prepare_fixture(state,scenario);
    const Model initial_model=state.model;const auto before=state.snapshot();std::ofstream events(root/"events.jsonl");std::atomic<bool> canceled{false};std::mutex mutex;std::condition_variable condition;bool done=false;
    std::thread deadline([&]{std::unique_lock<std::mutex> lock(mutex);if(!condition.wait_for(lock,std::chrono::minutes(5),[&]{return done;}))canceled=true;});
    const auto started=std::chrono::steady_clock::now();Json history,report;size_t calls=0;
    try {
        SessionCallbacks callbacks;callbacks.respond=[&](const Json& request){return client.respond(account,request,canceled,{});};callbacks.cancelled=[&]{return canceled.load();};
        callbacks.tool=[&](const std::string& name,const Json& arguments){++calls;return state.execute(name,arguments);};
        callbacks.trace=[&](const std::string& event,const Json& data){auto safe=data;if(safe.is_object()&&safe.contains("result")&&safe.at("result").is_object())safe["result"].erase("image_data");events<<Json({{"event",event},{"data",safe}}).dump()<<'\n';events.flush();};
        history=run_turn(Json::array(),state.snapshot(),prompt,model_name,callbacks);report={{"completed",true}};
    }catch(const std::exception& error){report={{"completed",false},{"error",error.what()}};}
    {std::lock_guard<std::mutex> lock(mutex);done=true;}condition.notify_one();deadline.join();
    report["scenario"]=scenario;report["model"]=model_name;report["tool_calls"]=calls;report["elapsed_seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();report["prompt"]=prompt;report["before"]=before;report["after"]=state.snapshot();
    const auto checked=verify(state,initial_model,scenario,report.value("completed",false));for(const auto& item:checked.items())report[item.key()]=item.value();const bool passed=checked.at("passed");report["fixture_limitations"]=state.fixture_limitations;report["comparison_eligible"]=state.fixture_limitations.empty();report["evaluation_status"]=passed?"native_goals_verified":state.fixture_limitations.empty()?"failed":"inconclusive_fixture_limitation";
    auto compact=[](Json& value,auto&& self)->void{if(value.is_string() && value.get_ref<const std::string&>().rfind("data:image/",0)==0)value="[native view saved in the PNG artifacts]";else if(value.is_structured())for(auto& child:value)self(child,self);};compact(history,compact);
    report["passed"]=passed;std::ofstream(root/"report.json")<<report.dump(2)<<'\n';std::ofstream(root/"history.json")<<history.dump(2)<<'\n';
    StoreParams saved;const auto target=(root/"Result.3mf").string();saved.path=target.c_str();saved.model=&state.model;saved.config=&state.config;store_bbs_3mf(saved);
    return {{"scenario",scenario},{"model",model_name},{"passed",passed},{"completed",report.at("completed")},{"tool_calls",calls},{"elapsed_seconds",report.at("elapsed_seconds")},{"report",(root/"report.json").string()}};
}
}
#endif
