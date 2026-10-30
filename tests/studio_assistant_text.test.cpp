#include "slic3r/GUI/StudioAssistantText.hpp"
#include "libslic3r/Utils.hpp"
#include "assistant_test_paths.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <cassert>
#include <iostream>
using namespace Slic3r;using namespace Slic3r::GUI::Assistant;
template<class F>void rejects(F&& fn){bool rejected=false;try{fn();}catch(...){rejected=true;}assert(rejected);}
int main(int argc,char** argv){
    configure_assistant_test_paths();
    const char* path=argc>1?argv[1]:"/System/Library/Fonts/Supplemental/Arial.ttf";
    auto file=Emboss::create_font_file(path);assert(file && !file->infos.empty());assert(!font_collection_names(*file).empty());
    Emboss::FontFileWithCache font(std::move(file));EmbossStyle style;style.name="Arial";style.prop=FontProp(10);
    auto word=prepare_text_mesh(font,style,"Mom",5,.6);assert(word.mesh.facets_count()>50);assert(std::abs(word.mesh.bounding_box().size().y()-5)<.002);assert(std::abs(word.mesh.bounding_box().size().z()-.6)<.002);assert(word.mesh.bounding_box().center().norm()<.002);
    assert(std::abs(its_volume(word.mesh.its))>1);assert(word.style.prop.size_in_mm>5);
    auto accent=prepare_text_mesh(font,style,"Café",7,1.2);assert(std::abs(accent.mesh.bounding_box().size().y()-7)<.002);
    auto multiline=prepare_text_mesh(font,style,"Love\nMom",12,1);assert(std::abs(multiline.mesh.bounding_box().size().y()-12)<.002);
    rejects([&]{prepare_text_mesh(font,style," ",5,1);});rejects([&]{prepare_text_mesh(font,style,"Mom",0,1);});
    rejects([&]{prepare_text_mesh(font,style,"Mom",5,0);});rejects([&]{prepare_text_mesh(font,style,"Mom",5,1,[]{return true;});});
    auto bad=style;bad.prop.collection_number=999;rejects([&]{prepare_text_mesh(font,bad,"Mom",5,1);});
    // A negative text part creates the actual letter voids in a solid body.
    Model model;auto* object=model.add_object("Label body","",make_cube(30,15,3));auto* instance=object->add_instance();instance->set_offset(Vec3d(60,60,3));const auto iid=std::to_string(instance->id().id);
    const auto body=object->volumes.front()->mesh().transformed_bounding_box(instance->get_matrix()*object->volumes.front()->get_matrix());const auto center=body.center();
    auto* text=object->add_volume(std::move(word.mesh),ModelVolumeType::NEGATIVE_VOLUME);text->set_transformation(Geometry::Transformation(instance->get_matrix().inverse()*geometry_frame(center,Vec3d::UnitZ())));object->invalidate_bounding_box();
    const auto section=model_section(*object,iid,center,Vec3d::UnitZ());size_t holes=0;for(const auto& polygon:section)holes+=polygon.holes.size();assert(holes>=3);
    std::cout<<"Assistant native font outlines, exact visible text sizing, Unicode, multiline extrusion, negative lettering, and invalid inputs passed.\n";
}
