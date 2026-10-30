#ifndef slic3r_StudioAssistantSurfaces_hpp_
#define slic3r_StudioAssistantSurfaces_hpp_
#include "StudioAssistantGeometry.hpp"
#include <queue>
#include <numeric>

namespace Slic3r { namespace GUI { namespace Assistant {
struct SurfacePatch {
    int seed{-1};
    std::vector<int> faces;
    Vec3d center{Vec3d::Zero()},normal{Vec3d::Zero()},sample{Vec3d::Zero()},sample_normal{Vec3d::Zero()};
    BoundingBoxf3 bounds;
    double area{0};
};
inline std::vector<SurfacePatch> surface_patches(const ModelVolume& part,const Transform3d& world,double angle_degrees=12)
{
    if (!part.is_model_part()) throw std::runtime_error("Inspect printable parts for surface painting.");
    const auto& mesh=part.mesh().its;
    if (mesh.indices.size()>2000000) throw std::runtime_error("This part is too detailed for a surface survey. Simplify it first.");
    if (!world.matrix().allFinite() || std::abs(world.linear().determinant())<1e-9 || !std::isfinite(angle_degrees) || angle_degrees<1 || angle_degrees>45)
        throw std::runtime_error("Use a valid part transform and a surface angle between 1 and 45 degrees.");
    const auto neighbors=its_face_neighbors(mesh);
    std::vector<Vec3d> vertices;vertices.reserve(mesh.vertices.size());
    for (const auto& v:mesh.vertices){const Vec3d point=world*v.cast<double>();if(!point.allFinite())throw std::runtime_error("This mesh contains invalid coordinates. Repair the source geometry before surveying its surfaces.");vertices.push_back(point);}
    std::vector<Vec3d> normals(mesh.indices.size()),centers(mesh.indices.size());
    std::vector<double> areas(mesh.indices.size());
    const double handed=world.linear().determinant()<0?-1:1;
    for (size_t f=0;f<mesh.indices.size();++f) {
        const auto& ids=mesh.indices[f];const auto& a=vertices[ids[0]];const auto& b=vertices[ids[1]];const auto& c=vertices[ids[2]];
        const Vec3d cross=(b-a).cross(c-a);areas[f]=cross.norm()/2;if(!std::isfinite(areas[f]))throw std::runtime_error("This mesh has invalid or excessively large surface geometry. Check its scale and source.");
        normals[f]=areas[f]>1e-12?Vec3d(handed*cross.normalized()):Vec3d::Zero();centers[f]=(a+b+c)/3;
    }
    const double cosine=std::cos(angle_degrees*PI/180.);
    std::vector<char> seen(mesh.indices.size(),false);std::vector<SurfacePatch> result;
    for (size_t seed=0;seed<mesh.indices.size();++seed) {
        if (seen[seed] || areas[seed]<1e-12) continue;
        SurfacePatch patch;patch.seed=int(seed);double largest=0;std::queue<int> todo;todo.push(int(seed));seen[seed]=true;
        while (!todo.empty()) {
            const int face=todo.front();todo.pop();patch.faces.push_back(face);
            patch.area+=areas[face];patch.center+=areas[face]*centers[face];patch.normal+=areas[face]*normals[face];
            if(areas[face]>largest){largest=areas[face];patch.sample=centers[face];patch.sample_normal=normals[face];}
            for(int k=0;k<3;++k)patch.bounds.merge(vertices[mesh.indices[face][k]]);
            for(int n:neighbors[face]) if(n>=0 && !seen[n] && areas[n]>1e-12 && normals[seed].dot(normals[n])>=cosine) {
                seen[n]=true;todo.push(n);
            }
        }
        patch.center/=patch.area;patch.normal.normalize();std::sort(patch.faces.begin(),patch.faces.end());result.push_back(std::move(patch));
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.area>b.area;});return result;
}
inline FacetsAnnotation& paint_layer(ModelVolume& part,const std::string& layer)
{
    if(layer=="support")return part.supported_facets;
    if(layer=="seam")return part.seam_facets;
    if(layer=="color")return part.mmu_segmentation_facets;
    if(layer=="fuzzy")return part.fuzzy_skin_facets;
    throw std::runtime_error("Choose support, seam, color, or fuzzy painting.");
}
inline PaintKeepPrepared prepared_surface_paint(const ModelVolume& part,const FacetsAnnotation& previous,
    const std::vector<int>& faces,int state)
{
    if (state<0 || state>32 || faces.empty())throw std::runtime_error("Choose measured surfaces and a valid painting state.");
    TriangleSelector selector(part.mesh());selector.deserialize(previous.get_data());
    for(int face:faces) {
        if(face<0 || size_t(face)>=part.mesh().facets_count())throw std::runtime_error("A surface is stale. Inspect its geometry again.");
        selector.set_facet(face,EnforcerBlockerType(state));
    }
    PaintKeepPrepared result;result.supported.set(selector);return result;
}
// Distance from a point to a solid triangle, including edges and vertices.
inline double point_triangle_distance2(const Vec3d& p,const Vec3d& a,const Vec3d& b,const Vec3d& c)
{
    const Vec3d ab=b-a,ac=c-a,ap=p-a;const double d1=ab.dot(ap),d2=ac.dot(ap);
    if(d1<=0 && d2<=0)return ap.squaredNorm();
    const Vec3d bp=p-b;const double d3=ab.dot(bp),d4=ac.dot(bp);
    if(d3>=0 && d4<=d3)return bp.squaredNorm();
    const double vc=d1*d4-d3*d2;
    if(vc<=0 && d1>=0 && d3<=0)return (p-(a+(d1/(d1-d3))*ab)).squaredNorm();
    const Vec3d cp=p-c;const double d5=ab.dot(cp),d6=ac.dot(cp);
    if(d6>=0 && d5<=d6)return cp.squaredNorm();
    const double vb=d5*d2-d1*d6;
    if(vb<=0 && d2>=0 && d6<=0)return (p-(a+(d2/(d2-d6))*ac)).squaredNorm();
    const double va=d3*d6-d5*d4;
    if(va<=0 && d4-d3>=0 && d5-d6>=0)return (p-(b+((d4-d3)/((d4-d3)+(d5-d6)))*(c-b))).squaredNorm();
    const double denominator=va+vb+vc;
    if(std::abs(denominator)<1e-20)return std::min({ap.squaredNorm(),bp.squaredNorm(),cp.squaredNorm()});
    return (p-(a+ab*(vb/denominator)+ac*(vc/denominator))).squaredNorm();
}
struct PreparedBrush { PaintKeepPrepared paint; size_t touched{0};bool limited{false}; };
inline PreparedBrush prepared_surface_brush(const ModelVolume& part,const FacetsAnnotation& previous,
    const Transform3d& world,const std::vector<int>& faces,int state,const Vec3d& center,double radius)
{
    geometry_vector(center);
    if(!std::isfinite(radius) || radius<.1 || radius>100 || state<0 || state>32 || faces.empty())
        throw std::runtime_error("Use measured surfaces and a brush radius between 0.1 and 100 mm.");
    std::vector<char> mask(part.mesh().facets_count(),false);
    for(int face:faces) {if(face<0 || size_t(face)>=mask.size())throw std::runtime_error("A surface is stale.");mask[face]=true;}
    TriangleSelector selector(part.mesh());selector.deserialize(previous.get_data());
    auto coverage=[&](const std::array<Vec3f,3>& local) {
        const Vec3d a=world*local[0].cast<double>(),b=world*local[1].cast<double>(),c=world*local[2].cast<double>();
        const double r2=radius*radius;
        if((a-center).squaredNorm()<=r2 && (b-center).squaredNorm()<=r2 && (c-center).squaredNorm()<=r2)return TriangleSelector::RegionCoverage::Full;
        return point_triangle_distance2(center,a,b,c)<=r2?TriangleSelector::RegionCoverage::Partial:TriangleSelector::RegionCoverage::None;
    };
    const Transform3f physical=world.cast<float>();
    const auto work=selector.paint_region(faces,coverage,EnforcerBlockerType(state),.1f,50000,12,nullptr,&mask,&physical);
    PreparedBrush result;result.paint.supported.set(selector);result.touched=work.facets_touched;result.limited=work.node_budget_exhausted;
    if(!result.touched)throw std::runtime_error("The brush missed the measured surfaces. Check its physical center/radius.");
    return result;
}
}}}
#endif
