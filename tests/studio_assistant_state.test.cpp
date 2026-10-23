#include "slic3r/GUI/StudioAssistantState.hpp"
#include <cassert>
#include <iostream>

using namespace Slic3r::GUI::Assistant;
int main()
{
    const std::map<int,std::set<int>> instance={{3,{0}}};
    const std::set<unsigned int> part_a={7},part_b={8},whole_instance={7,8};
    // Negative control: the previous fingerprint was identical for different
    // parts in one instance, allowing a stale delete to target the new part.
    const Json previous_a=instance,previous_b=instance;
    assert(previous_a==previous_b);
    const auto selected_a=selection_revision(0,instance,part_a);
    const auto selected_b=selection_revision(0,instance,part_b);
    assert(selected_a.dump()!=selected_b.dump());
    assert(selected_a==selection_revision(0,instance,part_a));
    assert(selection_revision(0,instance,whole_instance)!=selection_revision(1,instance,whole_instance));
    assert(selection_revision(0,instance,part_a)!=selection_revision(0,{{3,{1}}},part_a));
    assert(selected_a.at("instances")==previous_a);
    assert(selected_a.at("volumes")==Json(part_a));
    // A failed/empty load and an apparently populated load both lack a native
    // success signal. Preserve the inspected state without claiming success.
    for (const auto& objects : {Json::array(),Json::array({{{"id","present-object"}}})}) {
        const Json state={{"revision","r1"},{"objects",objects},{"can_undo",true}};
        const auto result=unconfirmed_project_open(state);
        assert(result.at("outcome")=="unconfirmed");
        assert(result.at("revision")==state.at("revision") && result.at("objects")==objects);
        assert(!result.contains("cancelled") && !result.contains("error"));
        assert(result.at("status").get<std::string>().find("does not confirm")!=std::string::npos);
        assert(result.at("instruction").get<std::string>().find("new request")!=std::string::npos);
    }
    std::cout<<"Exact selection fingerprints and unconfirmed native project load outcomes passed.\n";
}
