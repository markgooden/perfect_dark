#ifndef RT64_CAPTURE_H
#define RT64_CAPTURE_H

/*
 * C entry points for display-list capture (.pddl files).
 *
 * Capture records, per frame, every root display list submitted to gfx_run
 * plus the contents of every memory block that walk references, at the
 * original host addresses. A capture is self-contained: the translator and
 * its tests can be replayed against one with no game running. Format is
 * documented in rt64_capture.cpp.
 *
 * Everything here is a no-op unless capture has been armed, so the hooks can
 * sit unconditionally in the dispatch layer.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <PR/gbi.h>

/* Per-command hook, called by fast3d's gfx_run_dl for every command it
 * executes, with its live segment table (16 entries) as seg_addr is about to
 * use it. Non-null only while capture is armed, so the interpreter pays one
 * pointer test per command otherwise. Capture records from here rather than
 * walking the list itself: the interpreter is the definition of which
 * commands a frame contains, and any independent walk can only be checked
 * against it, never trusted over it. */
extern void (*pdCaptureCommandHook)(const Gfx *cmd, const uintptr_t *segments);

/* Arms capture for the next `frames` frames. Files are written as
 * <prefix>.NNNN.pddl, with a golden image alongside as <prefix>.NNNN.png.
 * Called from videoInit when --capture-frames is passed. */
void pdCaptureArm(const char *pathPrefix, int frames);

/* Arms capture on an F9 keypress instead of a fixed frame count. Far easier
 * than timing --capture-skip against a frame counter: get where you want in
 * the game, press F9. Set up from videoInit when --capture-key is passed. */
void pdCaptureSetupHotkey(const char *pathPrefix, int frames);

/* Called by the dispatch layer for every gfx_run, before the backend sees
 * the list. Records the display list and everything it references. */
void pdCaptureOnRun(const Gfx *rootDl);

/* Called by the dispatch layer at the end of each frame; closes the current
 * .pddl file and decrements the armed frame counter. */
void pdCaptureEndFrame(void);

/* True while capture is armed. Used by the fast3d path to decide whether to
 * do the (expensive) golden-image readback. */
int pdCaptureActive(void);

/* Called from the fast3d path with the rendered native-resolution image,
 * immediately before the buffer swap. `pixels` is tightly packed RGB8,
 * bottom-up as glReadPixels returns it. No-op unless armed. */
void pdCaptureGoldenImage(const unsigned char *pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* RT64_CAPTURE_H */
