#include "webview/webview_x11_parent.h"

#if defined(GDK_WINDOWING_X11) && defined(MOCKTAIL_WEBVIEW_X11_PARENT)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#endif

namespace mocktail {
namespace webview {

bool SetX11TransientForForeignWindow(GdkSurface* surface,
                                     unsigned long parent_window) {
#if defined(GDK_WINDOWING_X11) && defined(MOCKTAIL_WEBVIEW_X11_PARENT)
  if (surface == nullptr || parent_window == 0 || !GDK_IS_X11_SURFACE(surface)) {
    return false;
  }
  // The GDK X11 backend is deprecated as a whole since GTK 4.18, not replaced.
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  Display* display =
      gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
  const Window window = gdk_x11_surface_get_xid(surface);
  G_GNUC_END_IGNORE_DEPRECATIONS
  if (display == nullptr || window == 0) {
    return false;
  }
  // WM_TRANSIENT_FOR plus the dialog window type: X11 window managers, and
  // Wayland compositors for XWayland clients, float such a window over its
  // parent instead of tiling it.
  XSetTransientForHint(display, window, static_cast<Window>(parent_window));
  const Atom window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  const Atom dialog_type =
      XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
  XChangeProperty(display, window, window_type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&dialog_type), 1);
  XFlush(display);
  return true;
#else
  (void)surface;
  (void)parent_window;
  return false;
#endif
}

}  // namespace webview
}  // namespace mocktail
