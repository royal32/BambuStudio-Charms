#include "StudioAssistantAppearance.hpp"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

namespace Slic3r { namespace GUI {
void assistant_clip_children(void* native_view)
{
    NSView* view=static_cast<NSView*>(native_view);
    [view setWantsLayer:YES];
    [view setClipsToBounds:YES];
    [[view layer] setMasksToBounds:YES];
    if([view isKindOfClass:[NSScrollView class]])
        [[static_cast<NSScrollView*>(view) contentView] setClipsToBounds:YES];
    if(auto* enclosing=[view enclosingScrollView]) {
        [enclosing setClipsToBounds:YES];
        [[enclosing contentView] setClipsToBounds:YES];
    }
}
bool assistant_visible_text_contained(void* native_text,void* native_viewport)
{
    NSView* text=static_cast<NSView*>(native_text);
    if([text isKindOfClass:[NSScrollView class]])text=[static_cast<NSScrollView*>(text) documentView];
    NSView* viewport=static_cast<NSView*>(native_viewport);
    const NSRect visible=[text convertRect:[text visibleRect] toView:viewport];
    return NSIsEmptyRect(visible)||NSContainsRect(NSInsetRect([viewport bounds],-1,-1),visible);
}
bool assistant_children_clipped(void* native_view)
{
    NSView* view=static_cast<NSView*>(native_view);
    return [view clipsToBounds]&&[[view layer] masksToBounds];
}
void assistant_transparent_text_background(void* native_view)
{
    NSView* view=static_cast<NSView*>(native_view);
    // wx ports may expose either the text view or its enclosing scroll view.
    NSScrollView* scroll=[view isKindOfClass:[NSScrollView class]]?
        static_cast<NSScrollView*>(view):[view enclosingScrollView];
    if(!scroll)return;
    if (![[scroll documentView] isKindOfClass:[NSTextView class]]) return;
    [scroll setDrawsBackground:NO];
    [[scroll contentView] setDrawsBackground:NO];
    [static_cast<NSTextView*>([scroll documentView]) setDrawsBackground:NO];
    assistant_clip_children(scroll);
}
}}
