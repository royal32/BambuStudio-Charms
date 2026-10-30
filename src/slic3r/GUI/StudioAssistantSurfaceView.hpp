#ifndef slic3r_StudioAssistantSurfaceView_hpp_
#define slic3r_StudioAssistantSurfaceView_hpp_
#include "StudioAssistantSurfaces.hpp"
#include <limits>

namespace Slic3r { namespace GUI { namespace Assistant {
struct SurfaceRenderPart {const ModelVolume* part;Transform3d world;std::map<int,int> labels;int default_label{-1};};
struct SurfaceRaster {
    int width{768},height{768};std::vector<unsigned char> rgb;std::vector<int> labels;
    std::map<int,std::pair<int,int>> anchors;std::map<int,size_t> visible_pixels;
};
inline SurfaceRaster render_surface_map(const std::vector<SurfaceRenderPart>& parts,const std::string& direction)
{
    Vec3d eye,up=Vec3d::UnitZ();
    if(direction=="top"){eye=Vec3d::UnitZ();up=Vec3d::UnitY();}
    else if(direction=="bottom"){eye=-Vec3d::UnitZ();up=Vec3d::UnitY();}
    else if(direction=="front")eye=-Vec3d::UnitY();else if(direction=="rear")eye=Vec3d::UnitY();
    else if(direction=="left")eye=-Vec3d::UnitX();else if(direction=="right")eye=Vec3d::UnitX();
    else if(direction=="iso")eye=Vec3d(1,-1,1).normalized();else throw std::runtime_error("Choose top, bottom, front, rear, left, right, or iso.");
    up=(up-eye*up.dot(eye)).normalized();const Vec3d right=up.cross(eye).normalized();
    std::vector<std::vector<Vec3d>> vertices;BoundingBoxf3 projected;size_t total=0;
    for(const auto& part:parts) {
        if(!part.world.matrix().allFinite() || std::abs(part.world.linear().determinant())<1e-9)throw std::runtime_error("A part has an invalid transform; fix its scale before rendering surfaces.");
        total+=part.part->mesh().facets_count();if(total>2000000)throw std::runtime_error("Survey fewer parts at a time for a surface image.");
        auto& points=vertices.emplace_back();points.reserve(part.part->mesh().its.vertices.size());
        for(const auto& vertex:part.part->mesh().its.vertices){const Vec3d world=part.world*vertex.cast<double>();if(!world.allFinite())throw std::runtime_error("This mesh contains invalid coordinates. Repair the source geometry before rendering a piece map.");points.emplace_back(world.dot(right),world.dot(up),world.dot(eye));projected.merge(points.back());}
    }
    if(!projected.defined || projected.size().head<2>().maxCoeff()<1e-8)throw std::runtime_error("No visible geometry in this view.");
    SurfaceRaster out;const size_t pixels=size_t(out.width)*out.height;out.rgb.resize(pixels*3,235);out.labels.resize(pixels,-1);
    std::vector<double> depths(pixels,-std::numeric_limits<double>::infinity());
    const double scale=720/projected.size().head<2>().maxCoeff();const Vec3d center=projected.center();
    for(size_t n=0;n<parts.size();++n) {
        const Eigen::Matrix3d normal_matrix=parts[n].world.linear().inverse().transpose();const Vec3d light=Vec3d(1,-1,2).normalized();
        auto points=std::move(vertices[n]);for(auto& point:points){point.x()=(point.x()-center.x())*scale+384;point.y()=384-(point.y()-center.y())*scale;}
        const auto& mesh=parts[n].part->mesh().its;
        for(size_t f=0;f<mesh.indices.size();++f) {
            const auto& face=mesh.indices[f];const auto& a=points[face[0]];const auto& b=points[face[1]];const auto& c=points[face[2]];
            const double denominator=(b.y()-c.y())*(a.x()-c.x())+(c.x()-b.x())*(a.y()-c.y());if(std::abs(denominator)<1e-12)continue;
            const int x0=std::max(0,int(std::floor(std::min({a.x(),b.x(),c.x()})))),x1=std::min(767,int(std::ceil(std::max({a.x(),b.x(),c.x()}))));
            const int y0=std::max(0,int(std::floor(std::min({a.y(),b.y(),c.y()})))),y1=std::min(767,int(std::ceil(std::max({a.y(),b.y(),c.y()}))));
            const auto found=parts[n].labels.find(int(f));const int label=found==parts[n].labels.end()?parts[n].default_label:found->second;
            const Vec3d local=(mesh.vertices[face[1]]-mesh.vertices[face[0]]).cross(mesh.vertices[face[2]]-mesh.vertices[face[0]]).cast<double>();const Vec3d normal=normal_matrix*local;
            const double shade=normal.squaredNorm()>1e-20?.45+.55*std::max(0.,normal.normalized().dot(light)):.6;
            const unsigned hash=unsigned(label+7)*2654435761u;const unsigned char r=static_cast<unsigned char>((label<0?180:80+(hash&127))*shade),g=static_cast<unsigned char>((label<0?185:80+((hash>>8)&127))*shade),bl=static_cast<unsigned char>((label<0?190:80+((hash>>16)&127))*shade);
            for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
                const double px=x+.5,py=y+.5;const double u=((b.y()-c.y())*(px-c.x())+(c.x()-b.x())*(py-c.y()))/denominator;
                const double v=((c.y()-a.y())*(px-c.x())+(a.x()-c.x())*(py-c.y()))/denominator,w=1-u-v;
                if(u< -1e-9 || v< -1e-9 || w< -1e-9)continue;const size_t index=size_t(y)*768+x;const double depth=u*a.z()+v*b.z()+w*c.z();
                if(depth<=depths[index]+1e-9)continue;depths[index]=depth;out.labels[index]=label;out.rgb[index*3]=r;out.rgb[index*3+1]=g;out.rgb[index*3+2]=bl;
            }
        }
    }
    std::map<int,Vec2d> sums;
    for(size_t i=0;i<pixels;++i)if(out.labels[i]>=0){sums.try_emplace(out.labels[i],Vec2d::Zero());sums[out.labels[i]]+=Vec2d(i%768,i/768);++out.visible_pixels[out.labels[i]];}
    std::map<int,double> distances;for(const auto& entry:sums)distances[entry.first]=std::numeric_limits<double>::infinity();
    for(size_t i=0;i<pixels;++i)if(out.labels[i]>=0){const int label=out.labels[i];const double d=(Vec2d(i%768,i/768)-sums[label]/double(out.visible_pixels[label])).squaredNorm();if(d<distances[label]){distances[label]=d;out.anchors[label]={int(i%768),int(i/768)};}}
    return out;
}
}}}
#endif
