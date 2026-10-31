#ifndef slic3r_StudioAssistantAppearance_hpp_
#define slic3r_StudioAssistantAppearance_hpp_

namespace Slic3r { namespace GUI {
#ifdef __APPLE__
// Keep native selectable text, with the enclosing chat card supplying its fill.
void assistant_transparent_text_background(void* native_view);
void assistant_clip_children(void* native_view);
void assistant_uniform_button(void* native_view);
bool assistant_children_clipped(void* native_view);
bool assistant_visible_text_contained(void* native_text,void* native_viewport);
#else
inline void assistant_transparent_text_background(void*) {}
inline void assistant_clip_children(void*) {}
inline void assistant_uniform_button(void*) {}
inline bool assistant_children_clipped(void*) { return true; }
inline bool assistant_visible_text_contained(void*,void*) { return true; }
#endif
}}
#endif
