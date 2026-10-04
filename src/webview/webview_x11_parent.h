#ifndef MOCKTAIL_WEBVIEW_WEBVIEW_X11_PARENT_H_
#define MOCKTAIL_WEBVIEW_WEBVIEW_X11_PARENT_H_

#include <gdk/gdk.h>

namespace mocktail {
namespace webview {

// Xlib defines macros (Status, None, Bool) that collide with Mocktail's own
// names, so the X11 parenting lives in its own translation unit.
//
// Marks an X11 helper surface as a dialog transient for `parent_window` (an
// X11 window id of another process). False when the surface is not an X11
// surface or Xlib support was not built.
bool SetX11TransientForForeignWindow(GdkSurface* surface,
                                     unsigned long parent_window);

}  // namespace webview
}  // namespace mocktail

#endif  // MOCKTAIL_WEBVIEW_WEBVIEW_X11_PARENT_H_
