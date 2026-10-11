// c++ -std=c++17 -Isrc tests/plate_vector_preview.test.cpp -o /tmp/vector-preview-test
#include "../src/slic3r/GUI/PlateVectorPreview.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <set>

using Slic3r::GUI::PlateVectorPreview;
using nlohmann::json;

int main(int argc, char** argv)
{
    const auto ring = [](double x0, double y0, double x1, double y1) {
        return json::array({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    };
    json doc = {{"version", 1}, {"plate_index", 5}, {"width", 30.}, {"height", 20.},
        {"source_md5", std::string(32, 'a')}, {"regions", json::array({
            {{"filament", 0}, {"color", "#FF0000"}, {"contours", json::array({ring(1, 1, 10, 10), ring(3, 3, 8, 8)})}},
            {{"filament", 1}, {"color", "#FFFFFF"}, {"contours", json::array({ring(10, 1, 10.1, 10)})}},
            {{"filament", 4}, {"color", "#FF0000"}, {"contours", json::array({ring(20, 1, 29, 10)})}}
        })}};
    auto p = PlateVectorPreview::parse(doc);
    assert(p.hit_test({2, 2}) == 0);
    assert(p.hit_test({5, 5}) == -1); // lens opening is not clickable
    assert(p.hit_test({10.05, 5}, .2) == 1); // exact thin detail beats forgiving neighbor
    assert(p.hit_test({10.15, 5}, .1) == 1);
    assert(p.hit_test({25, 5}) == 4); // identical RGB, different sliced filament
    assert(p.hit_test({15, 15}, .1) == -1);
    assert(p.svg().find("fill-rule=\"evenodd\"") != std::string::npos);
    for (const auto& change : std::vector<json>{
        {{"version", 2}}, {{"width", 0}}, {{"height", -1}}, {{"source_md5", "stale"}},
        {{"regions", json::array()}}, {{"width", std::numeric_limits<double>::infinity()}}
    }) {
        auto bad = doc;
        bad.update(change);
        bool rejected = false;
        try { PlateVectorPreview::parse(bad); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
    }
    auto bad = doc;
    bad["regions"][0]["color"] = "url(https://example.com)";
    bool rejected = false;
    try { PlateVectorPreview::parse(bad); } catch (const std::exception&) { rejected = true; }
    assert(rejected);

    for (int i = 1; i < argc; ++i) {
        std::ifstream stream(argv[i]);
        const auto pilot = PlateVectorPreview::parse(json::parse(stream));
        std::set<int> wanted, picked;
        for (const auto& r : pilot.regions) wanted.insert(r.filament);
        for (int y = 0; y < 500; ++y)
            for (int x = 0; x < 1000; ++x) {
                const int id = pilot.hit_test({pilot.width * (x + .5) / 1000, pilot.height * (y + .5) / 500});
                if (id >= 0) picked.insert(id);
            }
        assert(wanted == picked);
        std::cout << argv[i] << ": all " << wanted.size() << " filament regions pickable\n";
    }
    std::cout << "Vector preview checks passed\n";
}
