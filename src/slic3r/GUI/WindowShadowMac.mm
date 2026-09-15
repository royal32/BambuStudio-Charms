#import <Cocoa/Cocoa.h>

namespace Slic3r { namespace GUI {

// Turn on the native macOS window shadow for the borderless top-level window that hosts
// `nsview` (wxWindow::GetHandle() returns the NSView on wxOSX). Called each time the window
// is shown or moved so the shadow re-derives from the current shaped/resized content.
void mac_enable_window_shadow(void *nsview)
{
    if (nsview == nullptr) return;
    NSView   *view = (__bridge NSView *) nsview;
    NSWindow *win  = [view window];
    if (win == nil) return;
    [win setHasShadow:YES];
    [win invalidateShadow]; // recompute from the current content alpha (rounded silhouette)
}

// A popup's content view can be hidden while Cocoa still has its floating
// NSWindow on screen. Keep a dismissed popup invisible and noninteractive even
// if a delayed native ordering operation brings that empty window back.
// Restore both properties when wx shows the same picker again.
void mac_set_popup_visible(void *nsview, bool shown)
{
    if (nsview == nullptr) return;
    NSView *view = (__bridge NSView *) nsview;
    NSWindow *win = [view window];
    if (win == nil) return;
    [win setIgnoresMouseEvents:!shown];
    [win setAlphaValue:shown ? 1.0 : 0.0];
    if (!shown) {
        [[win parentWindow] removeChildWindow:win];
        [win orderOut:nil];
    }
}

}} // namespace Slic3r::GUI
