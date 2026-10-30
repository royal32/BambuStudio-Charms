// Offline diagnosis of a valid cut rejected during the model benchmark.
// No account access or network requests. Run after timed trials, not alongside.
#include "assistant_workbench_eval.hpp"
#include <iostream>
using namespace CharmsWorkbenchEval;
int main()
{
    const Vec3d normal(-.5,.8660254037844386,0);
    const std::vector<Vec3d> points={Vec3d(35,85.98076211353316,0),Vec3d(43.66025403784438,90.98076211353316,1.5)};
    bool all=true;Json cases=Json::array();
    for(const auto& point:points){State state;prepare_fixture(state,"challenge");auto* object=state.model.objects[0];Json result={{"point_mm",vec(point)},{"normal",vec(normal)}};
        try{auto cut=prepare_plane_cut(*object,std::to_string(object->instances[0]->id().id),{std::to_string(object->volumes[0]->id().id)},point,normal,false);result["removed_mm3"]=cut.removed_mm3;cut.apply(*object);BoundingBoxf3 bounds;for(const auto* p:object->volumes)bounds.merge(p->mesh().transformed_bounding_box(p->get_matrix()));result["retained_size_mm"]=vec(bounds.size());result["passed"]=(bounds.size()-Vec3d(20,30,3)).cwiseAbs().maxCoeff()<.002;}
        catch(const std::exception& error){result["passed"]=false;result["error"]=error.what();}
        all&=result["passed"].get<bool>();cases.push_back(result);
    }
    const double difference=std::abs(normal.dot(points[0]-points[1]));
    std::cout<<Json({{"plane_offset_difference_mm",difference},{"equivalent_planes",difference<1e-10},{"all_passed",all},{"cases",cases}}).dump(2)<<'\n';
    return 0; // Diagnostic reports observations; neither outcome is presumed.
}
