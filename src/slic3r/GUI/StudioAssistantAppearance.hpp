#ifndef slic3r_StudioAssistantAppearance_hpp_
#define slic3r_StudioAssistantAppearance_hpp_

namespace Slic3r { namespace GUI {
#ifdef __APPLE__
// Keep native selectable text, with the enclosing chat card supplying its fill.
void assistant_transparent_text_background(void* native_view);
#else
inline void assistant_transparent_text_background(void*) {}
#endif
}}
#endif
