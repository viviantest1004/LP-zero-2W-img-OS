# desktop/compositor

LP's two compositors are Debian's: wayfire 0.7.4 where there is 3D
graphics, sway 1.7 where there is not (desktop/session/start-desktop
decides). Both sit on wlroots 0.15.1. Two of the three are patched here;
the build scripts rebuild each from Debian's own source package in a
throwaway overlay of the image's base, and mkdesktop.sh installs the
result over Debian's file.

## sway-1.7-lp.patch -> `sway` (build-sway.sh)

LP's session floats every window (sway.config), and stock sway ignores a
window's request to be maximized or minimized - so under sway the
maximize and minimize buttons on every title bar did nothing, and a
window that asked for the whole screen (LibreOffice) opened at 0,0 with
its title bar under the top bar. The patch:

- **maximize**: a floating window asking to be maximized (its title bar
  button, a double click on it, the dock through wlr-foreign-toplevel,
  or at start - LibreOffice reopens maximized) is fitted to its output's
  usable area, the output minus the top bar's and the dock's exclusive
  zones, and told so (GTK drops its shadow and rounded corners and shows
  the restore glyph). Its floating geometry is kept and given back on
  unmaximize. `arrange_workspace` keeps it fitted when the usable area
  changes - the dock gives its room back while a maximized window has
  the focus (desktop/dock/dock.c), and the window grows to the edge.
  Dragging a maximized window by its title bar restores its size first,
  under the pointer at the same place across its width.
- **minimize**: the window is hidden (in sway's scratchpad) with its
  foreign-toplevel handle kept and marked minimized, so the dock still
  shows it; activating it from the dock, or un-minimizing it, brings it
  back on the current workspace where it was, focused and raised, as an
  ordinary window again.
- **placement**: a new floating window is never bigger than the usable
  area and is centred in it, not on the whole output.

xwayland windows get the same through their `_NET_WM_STATE` requests.

## wlroots-0.15.1-lp.patch -> `libwlroots.so.10` (build-wlroots.sh)

The pointer on the GPU's cursor plane, where stock wlroots drew it into
every frame. Linux 6.8+ hides a virtual GPU's cursor plane (virtio-gpu:
QEMU, UTM) from atomic clients that do not promise to send the cursor's
hotspot, so every pointer movement was a whole-screen redraw and flush -
the pointer stuttered, badly with UTM's OpenGL on. With the patch (details
in its header): the client cap and HOTSPOT_X/Y (backport of upstream
0.18), the pixman renderer offering LINEAR (upstream 0.16), a dumb buffer
asked for LINEAR saying LINEAR, and FB_DAMAGE_CLIPS clipped to the FB
(upstream 0.17/0.19). Checked in QEMU (virtio-vga, sway/pixman): the
cursor plane carries sway's cursor buffer (debugfs dri state: plane 33
on crtc-0 with an fb), no fallback to a software cursor, and clicks land
where the pointer is. Same exported symbols as Debian's library.
