#ifndef slic3r_StudioAssistantText_hpp_
#define slic3r_StudioAssistantText_hpp_
#include "StudioAssistantGeometry.hpp"
#include "libslic3r/Emboss.hpp"
#include "imgui/imstb_truetype.h"
#include <boost/locale/encoding_utf.hpp>

namespace Slic3r { namespace GUI { namespace Assistant {
inline std::vector<std::string> font_collection_names(const ::Slic3r::Emboss::FontFile& file)
{
    std::vector<std::string> result;
    for(size_t index=0;index<file.infos.size();++index) {
        stbtt_fontinfo info{};if(!stbtt_InitFont(&info,file.data->data(),stbtt_GetFontOffsetForIndex(file.data->data(),int(index))))throw std::runtime_error("This installed font cannot be read.");
        int length=0;const auto* name=stbtt_GetFontNameString(&info,&length,3,1,0x409,4);std::string text="Font "+std::to_string(index);
        if(name && length>0 && length<=2048 && length%2==0){std::u16string decoded;for(int i=0;i<length;i+=2)decoded.push_back(char16_t((static_cast<unsigned char>(name[i])<<8)|static_cast<unsigned char>(name[i+1])));text=boost::locale::conv::utf_to_utf<char>(decoded);}
        result.push_back(std::move(text));
    }
    return result;
}
struct PreparedTextMesh {TriangleMesh mesh;EmbossStyle style;double height_mm{0},depth_mm{0};};
inline PreparedTextMesh prepare_text_mesh(::Slic3r::Emboss::FontFileWithCache& font,EmbossStyle style,const std::string& text,
    double visible_height,double depth,const std::function<bool()>& canceled={})
{
    if(!font.has_value() || text.empty() || text.size()>512 || text.find('\0')!=std::string::npos || !std::isfinite(visible_height) || visible_height<.5 || visible_height>200 || !std::isfinite(depth) || depth<.1 || depth>20)
        throw std::runtime_error("Choose printable text (up to 512 UTF-8 bytes), an installed outline font, 0.5–200 mm visible height and 0.1–20 mm thickness.");
    const unsigned collection=style.prop.collection_number.value_or(0);if(collection>=font.font_file->infos.size())throw std::runtime_error("Choose a font collection index returned by list_fonts.");
    stbtt_fontinfo info{};if(!stbtt_InitFont(&info,font.font_file->data->data(),stbtt_GetFontOffsetForIndex(font.font_file->data->data(),int(collection))))throw std::runtime_error("This font face cannot be read.");
    for(char32_t cp:boost::locale::conv::utf_to_utf<char32_t>(text))if(cp!='\n' && cp!='\r' && cp!='\t' && !stbtt_FindGlyphIndex(&info,int(cp)))throw std::runtime_error("This font does not contain every requested character. Choose another installed font; no letters were silently omitted.");
    style.prop.size_in_mm=float(visible_height);EmbossShape shape;const double standard=::Slic3r::Emboss::get_text_shape_scale(style.prop,*font.font_file);
    const auto polygons=::Slic3r::Emboss::text2shapes(shape,font,text.c_str(),style.prop,standard,canceled?canceled:[](){return false;});
    if(canceled && canceled())throw std::runtime_error("Text generation canceled; no changes applied.");
    if(polygons.expolygons.empty() || !polygons.is_healed)throw std::runtime_error("This text has no usable closed outlines in the chosen font.");
    BoundingBox bounds;for(const auto& p:polygons.expolygons)bounds.merge(p.contour.bounding_box());
    const double initial_height=double(bounds.size().y())*standard;
    if(initial_height<1e-6)throw std::runtime_error("This text has no visible height.");
    const double scale=standard*visible_height/initial_height;style.prop.size_in_mm*=float(visible_height/initial_height);
    ::Slic3r::Emboss::ProjectScale projection(std::make_unique<::Slic3r::Emboss::ProjectZ>(depth/scale),scale);
    TriangleMesh mesh(::Slic3r::Emboss::polygons2model(polygons.expolygons,projection));if(mesh.empty())throw std::runtime_error("Studio could not extrude this text.");
    mesh.translate((-mesh.bounding_box().center()).cast<float>().eval());
    PreparedTextMesh result;result.height_mm=visible_height;result.depth_mm=depth;result.mesh=std::move(mesh);result.style=std::move(style);return result;
}
}}}
#endif
