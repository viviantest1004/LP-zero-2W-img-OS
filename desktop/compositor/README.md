# desktop/compositor

LP's two compositors are Debian's: wayfire 0.7.4 where there is 3D
graphics, sway 1.7 where there is not (desktop/session/start-desktop
decides). Both sit on wlroots 0.15.1. All three are patched here; the
build scripts rebuild each from Debian's own source package in a
throwaway overlay of the image's base, and mkdesktop.sh installs the
result over Debian's file.

## wayfire-0.7.4-lp.patch -> `wayfire` (build-wayfire.sh)

Korean from the keyboard in Chromium, Electron (Claude, VS Code, Discord
...) and foot. Those type through text-input-v3, so their Korean comes
from the seat's input method, lp-osk (desktop/osk/type.c), which composes
Hangul from the keys it gets through its keyboard grab. Debian's wayfire
relays text-input and accepts the grab but never sends it a key, so
under wayfire they typed English only. With the patch, as in sway 1.7: a
key that no binding takes goes to the grab while an input method holds
it, except from the input method's own virtual keyboard (that is how it
sends back the keys it does not want); a key the application saw go
down is let go to the application; the seat's keyboard is not switched
for a key that goes to the grab (every switch resends every client the
keymap); and modifiers follow the keys. GTK and Qt windows keep typing
through fcitx5, which never asks for the grab.

The same patch, for Chrome's windows:

- **Pop-ups get a frame.** A window gets the xdg-decoration mode it asks
  for; `preferred_decoration_mode` (LP's is `client`) only answers a
  window that asks for none. 0.7.4 read its own pending answer instead
  of the request, so Chrome's pop-ups (`window.open`, the Google sign-in
  window), which ask for a server frame and draw none, had no title bar
  and no close button (src/core/core.cpp).
- **That frame** (the decoration plugin, also every X11 window's): the
  title at a header bar's size with a margin and an ellipsis instead of
  80% of the bar's height from its very edge, and the three buttons as
  light marks on the dark bar, drawn at twice the size for scale 2
  (plugins/decor).
- **A window bigger than the room for it** - Chrome opens 1018 pixels tall
  on a 1080 screen - is made to fit the work area, and nothing is placed
  above or left of it: centred as it was, Chrome's top and its tab
  strip's buttons went under the top bar (plugins/single_plugins/place.cpp).

So the program and two plugins (`libplace.so`, `libdecoration.so`) are
rebuilt, and Debian's other plugins load into the program: build-wayfire.sh
checks that every symbol Debian's program exports is still there (the
patch adds one, and takes none) and that the two plugins export what
Debian's do.
Chromium and Electron speak text-input only when started with
`--enable-wayland-ime --wayland-text-input-version=3`, which the dock,
the app grid and the tray add for them (desktop/common/lp-apps.c).
Checked in QEMU under wayfire: 한글 typed with Right Alt and two-beolsik
keys in foot and in an Electron 38 window started from the dock, and the
top bar's 한/EN kept the same between them and gedit (fcitx5).

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

Also in the patch: one output buffer on virtual GPUs drawn by pixman
(sway). The swapchain handed out two dumb buffers in turn, so every
commit carried a new FB, and virtio-gpu uploads the whole plane when the
FB changes - a 1920x1080 TRANSFER_TO_HOST_2D + RESOURCE_FLUSH per pointer
movement. On virtio_gpu, vmwgfx, qxl and cirrus, when the buffers are
dumb buffers, the swapchain now keeps drawing into the buffer committed
last: the FB stays the same and the driver uploads FB_DAMAGE_CLIPS only.
That takes LP's kernel fix too (kernel/patches/0001-drm-clear-
ignore_damage_clips-on-duplicate.patch): Linux 6.12 kept
`drm_plane_state.ignore_damage_clips` across
`__drm_atomic_helper_plane_duplicate_state()`, so after the first FB
change (fbcon -> sway) virtio-gpu ignored damage clips for good, and the
single buffer alone changed nothing. Measured in QEMU (virtio-vga,
sway/pixman, 60 pointer movements a second): RESOURCE_FLUSH went from
1920x1080 every time to about 32x30 (the cursor's old and new place),
and QEMU's main thread from about 12% to 4-5% of a core; window drags,
menus, the launcher and a scrolling terminal left nothing behind.

wayfire (GLES2 on virgl) keeps double buffering: its GBM buffers are what
the host displays, not a copy of it, and there a single buffer brought the
flushes down the same way but QEMU's CPU time not at all.
