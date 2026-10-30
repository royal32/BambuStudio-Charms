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
        const Transform3d to_cut=geometry_frame(point,normal).inverse()*object->instances[0]->get_matrix()*object->volumes[0]->get_matrix();
        Json variants=Json::array();for(bool snap:{false,true}){TriangleMesh transformed=object->volumes[0]->mesh();transformed.transform(to_cut,true);if(snap)for(auto& v:transformed.its.vertices)if(std::abs(v.z())<1e-5)v.z()=0;indexed_triangle_set kept;cut_mesh(transformed.its,0,nullptr,&kept);its_compactify_vertices(kept);variants.push_back({{"snapped",snap},{"before_volume",its_volume(transformed.its)},{"after_volume",its_volume(kept)},{"open_edges",its_num_open_edges(kept)}});}result["variants"]=variants;
        try{auto cut=prepare_plane_cut(*object,std::to_string(object->instances[0]->id().id),{std::to_string(object->volumes[0]->id().id)},point,normal,false);result["removed_mm3"]=cut.removed_mm3;cut.apply(*object);BoundingBoxf3 bounds;for(const auto* p:object->volumes)bounds.merge(p->mesh().transformed_bounding_box(p->get_matrix()));result["retained_size_mm"]=vec(bounds.size());result["passed"]=(bounds.size()-Vec3d(20,30,3)).cwiseAbs().maxCoeff()<.002;}
        catch(const std::exception& error){result["passed"]=false;result["error"]=error.what();}
        all&=result["passed"].get<bool>();cases.push_back(result);
    }
    const double difference=std::abs(normal.dot(points[0]-points[1]));
    std::cout<<Json({{"plane_offset_difference_mm",difference},{"equivalent_planes",difference<1e-10},{"all_passed",all},{"cases",cases}}).dump(2)<<'\n';
    return all?0:1; // Regression: equivalent valid planes must both succeed.
}
