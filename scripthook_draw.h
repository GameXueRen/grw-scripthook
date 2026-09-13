/* The drawing layer's internal boundary.
 *
 * Plugins never include this file - they only ever see the SH_API
 * functions declared in scripthook.h under @defgroup draw.  This header
 * exists because the drawing layer is split in two on purpose:
 *
 *   scripthook_draw.c   owns the drawer registry, the input session and
 *                       every exported function.  It is plain C and
 *                       builds in BOTH toolchains (the MinGW Makefile has
 *                       no C++/ImGui/D3D11 at all).
 *   scripthook_ovl.cpp  owns the primitives, because they are ImGui.
 *                       It fills in the vtable below once ImGui is up.
 *
 * The C side forwards every primitive call to that vtable.  With no
 * renderer attached - the GCC build, or any moment before ImGui's first
 * frame - the table is empty and every primitive is a safe no-op, so a
 * plugin never has to branch on how the framework was built.
 *
 * The drawer callback itself runs on the RENDER thread, inside the
 * frame, wrapped in one ImGui window by the renderer.  See the
 * @defgroup draw comment in scripthook.h for the contract.
 */
#ifndef GRW_SCRIPTHOOK_DRAW_H
#define GRW_SCRIPTHOOK_DRAW_H

#include "scripthook.h"

/* The C registry defines these and the C++ renderer calls them.  Without
 * this the two halves would link under different names: the declarations
 * would be C++ linkage in scripthook_ovl.cpp and C linkage in
 * scripthook_draw.c, and the linker says so very loudly. */
#ifdef __cplusplus
extern "C" {
#endif

#define SH_DRAW_MAX       16   /* drawers alive at once                 */
#define SH_DRAW_NAME_MAX  48   /* name = window title = ShDrawDel key   */

/** One drawer, as the registry holds it.  Copied out under the lock for
 *  each frame so a callback may register or drop drawers while the frame
 *  that is already running keeps its own snapshot. */
typedef struct ShDrawItem {
    char       name[SH_DRAW_NAME_MAX];
    ShDrawFn   fn;
    void      *user;
    ShDrawOpts opts;
    int        used;
    int        shown;              /**< 0 after the user closed its window */
} ShDrawItem;

/** The renderer's half of the layer.  Every entry is filled in by
 *  scripthook_ovl.cpp and published with ShDrawSetVtbl(); the C side
 *  checks the pointer before every call, so a NULL table (GCC build, or
 *  ImGui not up yet) turns the whole layer into no-ops.
 *
 *  All of these run on the render thread, inside a drawer's callback,
 *  except `composing` which any thread may call. */
typedef struct ShDrawVtbl {
    /** Open this drawer's window; returns 1 when the callback should
     *  draw into it (0 when the window is collapsed or clipped). */
    int   (*begin)(const char *name, const ShDrawOpts *opts);
    /** Close the window opened by begin(). */
    void  (*end)(void);
    /** Current UI scale (1.0 = the 1080p baseline). */
    float (*scale)(void);
    /* text */
    void  (*text)(const char *utf8);
    void  (*text_colored)(const char *utf8, unsigned rgb, int a);
    void  (*text_wrapped)(const char *utf8);
    void  (*hint)(const char *utf8);
    /* layout */
    void  (*spacing)(void);
    void  (*same_line)(void);
    void  (*separator)(void);
    void  (*panel)(float w, float h, unsigned rgb, int a);
    void  (*rect)(float w, float h, unsigned rgb, int a);
    /* widgets */
    int   (*button)(const char *label);
    int   (*toggle)(const char *label, int *v);
    int   (*number)(const char *label, int *v, int step, int mn, int mx);
    int   (*slider)(const char *label, float *v, float mn, float mx);
    int   (*list)(const char *label, int *idx, const char *const *items, int n);
    void  (*push_font)(int which);
    void  (*pop_font)(void);
    /** The input box: draws the box, its composition string, its caret,
     *  its candidate list and its hint at the current cursor position.
     *  `focused` is decided by the registry (the session's own box id)
     *  and `out` comes back with focus / composing / mode. */
    int   (*input_box)(const char *id, const char *text, const char *hint,
                       int focused, ShDrawInput *out);
    /** True while an IME composition (pinyin) or its candidate list is
     *  live: Enter / Esc / Backspace belong to the IME then, not to the
     *  text buffer.  Callable from any thread. */
    int   (*composing)(void);
} ShDrawVtbl;

/* ---- renderer side -------------------------------------------------- */

/** Publish the vtable.  Called once, from the render thread, right after
 *  ImGui comes up. */
void ShDrawSetVtbl(const ShDrawVtbl *vt);
/** Hide or show a drawer (the renderer calls this when the user clicks a
 *  window's close box).  Same as ShDrawShow(), spelled short. */
void ShDrawSetShown(const char *name, int shown);

/* ---- the frame ------------------------------------------------------ */

/** One frame: for every visible drawer, begin its window, call it, end
 *  the window.  Called by the renderer after NewFrame() and before
 *  Render(). */
void ShDrawFrame(void);
/** Anything to draw?  The renderer's NewFrame gate and its window-message
 *  routing both read this; with no drawers it is a single loop over an
 *  empty table and nothing else costs a thing. */
int  ShDrawWantFrame(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* GRW_SCRIPTHOOK_DRAW_H */
