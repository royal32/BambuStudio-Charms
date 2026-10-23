#include "slic3r/GUI/StudioAssistantLibrarySearch.hpp"
#include <cassert>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

using namespace Slic3r::GUI::Assistant;
int main()
{
    LibrarySearch small("CHARM");
    for (const auto* filename : {"z charm.GCODE.3MF", "Charm box.gcode.3mf", "Charm.gcode.3mf", "charm.gcode.3mf", "A charm.gcode.3mf",
             "Charm.3mf", "Charm.gcode", "not a match.gcode.3mf", "Charm.gcode.3mf.bak"}) small.consider(filename);
    assert(small.total_matches()==5 && !small.truncated());
    assert(small.filenames()==std::vector<std::string>({"A charm.gcode.3mf","Charm.gcode.3mf","charm.gcode.3mf","Charm box.gcode.3mf","z charm.GCODE.3MF"}));
    std::vector<std::string> names;
    for (int i=0;i<137;++i) {
        std::ostringstream name;
        name<<"Charm "<<std::setw(3)<<std::setfill('0')<<i<<".gcode.3mf";
        names.push_back(name.str());
    }
    LibrarySearch forward("charm"), reversed("charm"), shuffled("charm");
    for (const auto& name:names) forward.consider(name);
    for (auto it=names.rbegin();it!=names.rend();++it) reversed.consider(*it);
    std::mt19937 random(42);
    std::shuffle(names.begin(),names.end(),random);
    for (const auto& name:names) shuffled.consider(name);
    assert(forward.total_matches()==137 && forward.truncated());
    assert(reversed.total_matches()==137 && shuffled.total_matches()==137);
    const auto expected=forward.filenames();
    assert(expected.size()==40 && expected.front()=="Charm 000.gcode.3mf" && expected.back()=="Charm 039.gcode.3mf");
    assert(reversed.filenames()==expected && shuffled.filenames()==expected);
    LibrarySearch exact_cap("charm");
    for (const auto& name:expected) exact_cap.consider(name);
    assert(exact_cap.total_matches()==40 && !exact_cap.truncated());
    LibrarySearch refined("charm 136");
    for (const auto& name:names) refined.consider(name);
    assert(refined.total_matches()==1 && !refined.truncated());
    assert(refined.filenames()==std::vector<std::string>({"Charm 136.gcode.3mf"}));
    LibrarySearch missing("missing");
    for (const auto& name:names) missing.consider(name);
    assert(missing.filenames().empty() && missing.total_matches()==0 && !missing.truncated());
    std::cout<<"Library filename filtering, deterministic bounded ordering, match counts, and query refinement passed.\n";
}
