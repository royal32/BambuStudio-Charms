#ifndef slic3r_GUI_PlateVectorPreview_hpp_
#define slic3r_GUI_PlateVectorPreview_hpp_

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

// App-only, geometry-derived artwork. Filament IDs are zero based and do not
// depend on display RGB, so identical spool colors remain independently pickable.
struct PlateVectorPreview {
    struct Point { double x, y; };
    struct Region {
        int filament;
        std::string color;
        std::vector<std::vector<Point>> contours; // even-odd: outer rings and holes
    };
    int plate_index{0}; // one based, as stored in the sliced archive
    double width{0}, height{0};
    std::string source_md5;
    std::vector<Region> regions;

    static PlateVectorPreview parse(const nlohmann::json& j)
    {
        PlateVectorPreview result;
        if (j.at("version").get<int>() != 1) throw std::runtime_error("Unsupported vector preview");
        result.plate_index = j.at("plate_index").get<int>();
        result.width = j.at("width").get<double>();
        result.height = j.at("height").get<double>();
        result.source_md5 = j.at("source_md5").get<std::string>();
        const auto is_hex = [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        if (result.plate_index < 1 || !std::isfinite(result.width) || !std::isfinite(result.height) ||
            result.width <= 0 || result.height <= 0 || result.width > 10000 || result.height > 10000 ||
            result.source_md5.size() != 32 || !std::all_of(result.source_md5.begin(), result.source_md5.end(), is_hex))
            throw std::runtime_error("Invalid vector preview bounds or identity");
        size_t point_count = 0;
        const auto& regions = j.at("regions");
        if (!regions.is_array() || regions.empty() || regions.size() > 256) throw std::runtime_error("Invalid vector regions");
        for (const auto& value : regions) {
            Region region;
            region.filament = value.at("filament").get<int>();
            region.color = value.at("color").get<std::string>();
            if (region.filament < 0 || region.filament > 255 || region.color.size() != 7 || region.color[0] != '#' ||
                !std::all_of(region.color.begin() + 1, region.color.end(), is_hex)) throw std::runtime_error("Invalid vector filament");
            const auto& contours = value.at("contours");
            if (!contours.is_array() || contours.empty()) throw std::runtime_error("Empty vector region");
            for (const auto& ring : contours) {
                if (!ring.is_array() || ring.size() < 3 || (point_count += ring.size()) > 200000)
                    throw std::runtime_error("Invalid vector contour");
                std::vector<Point> points;
                for (const auto& xy : ring) {
                    if (!xy.is_array() || xy.size() != 2) throw std::runtime_error("Invalid vector point");
                    const Point p{xy[0].get<double>(), xy[1].get<double>()};
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -0.01 || p.y < -0.01 ||
                        p.x > result.width + 0.01 || p.y > result.height + 0.01) throw std::runtime_error("Vector point outside bounds");
                    points.push_back(p);
                }
                region.contours.push_back(std::move(points));
            }
            result.regions.push_back(std::move(region));
        }
        return result;
    }

    int hit_test(Point p, double tolerance = 0) const
    {
        // Match painter order: the last region is visible where parts overlap.
        // Exact hits always win over a neighboring region's forgiving edge.
        for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
            const auto& region = *it;
            bool inside = false;
            for (const auto& ring : region.contours)
                for (size_t i = 0, k = ring.size() - 1; i < ring.size(); k = i++) {
                    const auto a = ring[i], b = ring[k];
                    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                        inside = !inside;
                }
            if (inside) return region.filament;
        }
        double nearest = tolerance * tolerance;
        int filament = -1;
        if (tolerance > 0)
            for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
                const auto& region = *it;
                for (const auto& ring : region.contours)
                    for (size_t i = 0, k = ring.size() - 1; i < ring.size(); k = i++) {
                        const auto a = ring[i], b = ring[k];
                        const double dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
                        const double t = length > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0., 1.) : 0;
                        const double ex = p.x - a.x - t * dx, ey = p.y - a.y - t * dy;
                        if (ex * ex + ey * ey < nearest) { nearest = ex * ex + ey * ey; filament = region.filament; }
                    }
            }
        return filament;
    }

    // White portions of a black/white dashed outline, continuous across corners.
    static std::vector<std::pair<Point, Point>> marching_ants(const std::vector<Point>& ring, double dash_length, double phase)
    {
        std::vector<std::pair<Point, Point>> dashes;
        if (ring.size() < 2 || !std::isfinite(dash_length) || dash_length <= 0 || !std::isfinite(phase)) return dashes;
        const double period = 2 * dash_length;
        phase = std::fmod(std::fmod(phase, period) + period, period);
        double distance = 0;
        for (size_t i = 0; i < ring.size(); ++i) {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
            if (length == 0) continue;
            for (double white_start = std::floor((distance + phase) / period) * period - phase;
                 white_start < distance + length; white_start += period) {
                const double start = std::max(distance, white_start) - distance;
                const double end = std::min(distance + length, white_start + dash_length) - distance;
                if (end > start) dashes.push_back({{a.x + dx * start / length, a.y + dy * start / length},
                                                 {a.x + dx * end / length, a.y + dy * end / length}});
            }
            distance += length;
        }
        return dashes;
    }

    std::string svg() const
    {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s.precision(8);
        s << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-" << width * .04 << " -" << height * .04
          << ' ' << width * 1.08 << ' ' << height * 1.08 << "\">";
        for (const auto& region : regions) {
            s << "<path fill=\"" << region.color << "\" fill-rule=\"evenodd\" d=\"";
            for (const auto& ring : region.contours) {
                s << 'M' << ring.front().x << ',' << ring.front().y;
                for (size_t i = 1; i < ring.size(); ++i) s << 'L' << ring[i].x << ',' << ring[i].y;
                s << 'Z';
            }
            s << "\"/>";
        }
        return s.str() + "</svg>";
    }
};

// Returns null for missing, stale or malformed sidecars. Never changes a job.
std::unique_ptr<PlateVectorPreview> load_plate_vector_preview(const std::string& job, int plate_index = -1);

}}
#endif
