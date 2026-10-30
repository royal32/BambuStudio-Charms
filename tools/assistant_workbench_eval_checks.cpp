// Scripted fixture-driver checks. These make no network/model requests and do
// not assert that a language model chose the actions or that GUI wiring passed.
#include "assistant_workbench_eval.hpp"
#include "../tests/assistant_test_paths.hpp"
#include <cassert>
#include <iostream>

using namespace CharmsWorkbenchEval;
int main()
{
    configure_assistant_test_paths();Json reports=Json::array();
    for(const std::string scenario:{"supports","loop","keychain"}) {
        State state;state.output=std::filesystem::path(assistant_test_tmp())/scenario;
        std::filesystem::create_directories(state.output);const auto prompt=prepare_fixture(state,scenario);const Model before=state.model;
        auto mutate=[&](const std::string& name,Json args){args["revision"]=state.revision();return state.execute(name,args);};
        if(scenario=="supports") {
            auto* object=state.model.objects.front();auto* instance=object->instances.front();auto* part=object->volumes.front();
            const auto survey=state.execute("inspect_surfaces",{{"object_id",std::to_string(object->id().id)},{"instance_id",std::to_string(instance->id().id)},{"part_ids",{std::to_string(part->id().id)}},{"filter","downward"}});
            Json surfaces=Json::array();for(const auto& s:survey.at("surfaces"))if(s.at("area_mm2").get<double>()>100)surfaces.push_back(s.at("surface_id"));assert(!surfaces.empty());
            state.execute("surface_view",{{"surface_ids",surfaces},{"direction","bottom"}});
            mutate("paint_surfaces",{{"surface_ids",surfaces},{"layer","support"},{"action","enforce"}});
            mutate("set_settings",{{"scope","global"},{"settings",Json::array({{{"key","enable_support"},{"value","1"}},{{"key","support_type"},{"value","normal(manual)"}},{{"key","support_on_build_plate_only"},{"value","1"}}})}});
        }else if(scenario=="loop") {
            auto* object=state.model.objects.front();auto* instance=object->instances.front();const auto oid=std::to_string(object->id().id),iid=std::to_string(instance->id().id);
            mutate("plane_cut",{{"object_id",oid},{"instance_id",iid},{"part_ids",{std::to_string(object->volumes.front()->id().id)}},{"point_mm",{90,105,0}},{"normal",{0,1,0}},{"keep","negative"}});
            mutate("add_primitive",{{"object_id",oid},{"instance_id",iid},{"shape","cylinder"},{"type","negative"},{"center_mm",{90,102,1.5}},{"size_mm",{2,2,8}},{"axis",{0,0,1}}});
        }else {
            mutate("create_object",{{"shape","box"},{"name","Keychain"},{"size_mm",{30,20,3}}});auto* object=state.model.objects.front();const auto oid=std::to_string(object->id().id),iid=std::to_string(object->instances.front()->id().id);
            mutate("add_primitive",{{"object_id",oid},{"instance_id",iid},{"shape","cylinder"},{"type","negative"},{"center_mm",{79,90,1.5}},{"size_mm",{3,3,8}},{"axis",{0,0,1}}});
            mutate("add_text",{{"object_id",oid},{"instance_id",iid},{"text","Mom"},{"height_mm",5},{"depth_mm",.6},{"center_mm",{90,90,3.2}}});
        }
        state.execute("slice",Json::object());const auto report=verify(state,before,scenario,true);
        std::ofstream(state.output/"scripted-report.json")<<report.dump(2)<<'\n';
        state.execute("slice_layer_view",{{"print_z_mm",1.5}});
        reports.push_back({{"scenario",scenario},{"verification",report}});
        std::cout<<scenario<<": "<<report.dump()<<'\n';assert(report.at("passed").get<bool>());
        save_geometry_fixture(state,state.output/"Result.3mf");assert(std::filesystem::file_size(state.output/"Result.3mf")>0);
    }
    std::ofstream(std::filesystem::path(assistant_test_tmp())/"scripted-summary.json")<<reports.dump(2)<<'\n';
    std::cout<<"Scripted native workbench fixture driver and outcome checks passed; no real-model or GUI trial was performed.\n";
}
