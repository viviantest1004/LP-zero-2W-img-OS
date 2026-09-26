/*
 * lp-sheet.h - a layer-shell panel that slides in from a screen edge.
 *
 * Quick settings, the app grid and notifications are all the same shape:
 * a surface anchored to one edge that slides into view and back out. They
 * were each about to grow their own copy of the slide, and the slide is
 * the one piece of this that is easy to get subtly wrong, so it lives
 * here once.
 *
 * ── why cairo_translate and not layer-shell margins ──
 *
 * The obvious way to slide a layer-shell surface is to animate its margin
 * from -height to 0. That is wrong on this machine: every margin change
 * is a layer-shell configure round-trip to the compositor, so a 260 ms
 * slide at 60 Hz is ~16 round-trips, each of which can miss a frame. The
 * surface here is instead created once at its full open size and never
 * resized; the content inside it is drawn shifted by cairo_translate, and
 * only the input region (cheap, no round-trip) tracks the visible part.
 * This is the rule COMMON.md's Motion section states for every panel.
 *
 * The travel is an LpSpring (LP_SPRING_SHEET): interruptible, so a sheet
 * caught half-open on its way out turns round from where it is, and
 * flingable, so a finger that lets go with speed carries it. When the
 * spring settles closed the window is hidden, so a shut sheet costs no
 * input capture and no wakeups. Under reduced motion the travel becomes a
 * short opacity crossfade with no movement (lp_motion_reduced()).
 */
#ifndef LP_SHEET_H
#define LP_SHEET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _LpSheet LpSheet;

/* A sheet anchored to `edge` (one of LP_EDGE_TOP/BOTTOM/LEFT/RIGHT from
 * lp-shell.h), in layer-shell namespace `ns`, showing `content`. The
 * surface takes content's natural size along the slide axis and spans the
 * screen across it. Reference-owns `content`. */
LpSheet   *lp_sheet_new(const char *ns, int edge, GtkWidget *content);

/* The underlying layer-shell window, for anchoring/margins the caller
 * wants to set once at creation (never per frame). */
GtkWindow *lp_sheet_window(LpSheet *sh);

void       lp_sheet_show(LpSheet *sh);    /* slide in  (entrance timing) */
void       lp_sheet_hide(LpSheet *sh);    /* slide out (0.7x the time)   */
void       lp_sheet_toggle(LpSheet *sh);
/* TRUE when the sheet is open or opening (its target is shown). */
gboolean   lp_sheet_shown(LpSheet *sh);

/* Follow a finger: fraction is 0 (hidden) .. 1 (open). Call on every drag
 * event; it moves the sheet to that fraction without any spring. */
void       lp_sheet_drag(LpSheet *sh, double fraction);
/* Let go: `velocity` is the finger's speed in fraction-per-second (from
 * lp_velocity_get scaled by the extent). The sheet flings and settles
 * open or closed depending on where the fling would land. */
void       lp_sheet_release(LpSheet *sh, double velocity);

/* Called once each time the sheet has finished sliding closed and its
 * window has been hidden - the moment to drop a scrim or exit. */
typedef void (*LpSheetClosed)(LpSheet *sh, gpointer data);
void       lp_sheet_on_closed(LpSheet *sh, LpSheetClosed cb, gpointer data);

void       lp_sheet_free(LpSheet *sh);

G_END_DECLS

#endif /* LP_SHEET_H */
