#include "StudioAssistantAppearance.hpp"
#import <Cocoa/Cocoa.h>

namespace Slic3r { namespace GUI {
void assistant_transparent_text_background(void* native_view)
{
    NSView* view=static_cast<NSView*>(native_view);
    if (![view isKindOfClass:[NSScrollView class]]) return;
    auto* scroll=static_cast<NSScrollView*>(view);
    if (![[scroll documentView] isKindOfClass:[NSTextView class]]) return;
    [scroll setDrawsBackground:NO];
    [[scroll contentView] setDrawsBackground:NO];
    [static_cast<NSTextView*>([scroll documentView]) setDrawsBackground:NO];
}
}}
