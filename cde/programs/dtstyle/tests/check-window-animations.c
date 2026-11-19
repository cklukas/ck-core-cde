/*
 * Standalone ASan/UBSan unit tests for backend-aware window minimize & maximize
 * animation style resolution and bounded Xresource serialization across
 * dtstyle (Dtwm.c) and classic X11 dtwm (WmResource.c).
 */
/* @@PROLOGUE@@ */
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Boolean;
#define True 1
#define False 0

#define DTWM_UI_ANIM_NONE      0
#define DTWM_UI_ANIM_BEVEL     1
#define DTWM_UI_ANIM_TITLEBAR  2
#define DTWM_UI_ANIM_WIREFRAME 3
#define DTWM_UI_ANIM_COUNT     4

#define WM_ANIM_NONE      0u
#define WM_ANIM_WIREFRAME 1u
#define WM_ANIM_TITLEBAR  2u

/* @@EXTRACTED@@ */

static void test_wayland_defaults_and_choices(void)
{
    /* Unset / auto on Wayland defaults to Bevel for both Minimize and Maximize */
    assert(DtstyleResolveWindowAnimStyle(nullptr, True, False, False, True) == DTWM_UI_ANIM_BEVEL);
    assert(DtstyleResolveWindowAnimStyle("", True, False, False, True) == DTWM_UI_ANIM_BEVEL);
    assert(DtstyleResolveWindowAnimStyle("auto", True, False, False, True) == DTWM_UI_ANIM_BEVEL);
    assert(DtstyleResolveWindowAnimStyle("auto", True, True, False, True) == DTWM_UI_ANIM_BEVEL);

    /* Legacy animateMinimizeRestore: False disables unset/auto minimize animation on Wayland */
    assert(DtstyleResolveWindowAnimStyle("auto", True, False, True, False) == DTWM_UI_ANIM_NONE);
    /* Maximize default on Wayland remains Bevel even if legacy minimize was False */
    assert(DtstyleResolveWindowAnimStyle("auto", True, True, True, False) == DTWM_UI_ANIM_BEVEL);

    /* Explicit Wayland choices & aliases */
    assert(DtstyleResolveWindowAnimStyle("none", True, False, True, True) == DTWM_UI_ANIM_NONE);
    assert(DtstyleResolveWindowAnimStyle("  Bevel_Morph  ", True, False, True, False) == DTWM_UI_ANIM_BEVEL);
    assert(DtstyleResolveWindowAnimStyle("bevel_glide", True, True, False, True) == DTWM_UI_ANIM_BEVEL);
    assert(DtstyleResolveWindowAnimStyle("Titlebar_Shuttle", True, False, False, True) == DTWM_UI_ANIM_TITLEBAR);
    assert(DtstyleResolveWindowAnimStyle("wireframe", True, True, False, True) == DTWM_UI_ANIM_WIREFRAME);
}

static void test_x11_defaults_and_roaming_fallback(void)
{
    /* Unset / auto on X11 defaults to Wireframe for Minimize and None for Maximize */
    assert(DtstyleResolveWindowAnimStyle(nullptr, False, False, False, True) == DTWM_UI_ANIM_WIREFRAME);
    assert(DtstyleResolveWindowAnimStyle("auto", False, False, False, True) == DTWM_UI_ANIM_WIREFRAME);
    assert(DtstyleResolveWindowAnimStyle("auto", False, True, False, True) == DTWM_UI_ANIM_NONE);

    assert(WmResolveX11AnimMode(nullptr, False, True) == WM_ANIM_WIREFRAME);
    assert(WmResolveX11AnimMode("auto", False, False) == WM_ANIM_NONE);
    assert(WmResolveX11AnimMode("auto", True, True) == WM_ANIM_NONE);

    /* Roaming $HOME from Wayland ("bevel") into X11 degrades gracefully to X11 native defaults */
    assert(DtstyleResolveWindowAnimStyle("bevel", False, False, False, True) == DTWM_UI_ANIM_WIREFRAME);
    assert(DtstyleResolveWindowAnimStyle("bevel", False, False, True, False) == DTWM_UI_ANIM_NONE);
    assert(DtstyleResolveWindowAnimStyle("bevel", False, True, False, True) == DTWM_UI_ANIM_NONE);

    assert(WmResolveX11AnimMode("bevel", False, True) == WM_ANIM_WIREFRAME);
    assert(WmResolveX11AnimMode("bevel", False, False) == WM_ANIM_NONE);
    assert(WmResolveX11AnimMode("bevel", True, True) == WM_ANIM_NONE);

    /* Explicit X11 choices */
    assert(WmResolveX11AnimMode("none", False, True) == WM_ANIM_NONE);
    assert(WmResolveX11AnimMode("wireframe", True, False) == WM_ANIM_WIREFRAME);
    assert(WmResolveX11AnimMode("  Titlebar  ", False, False) == WM_ANIM_TITLEBAR);
    assert(WmResolveX11AnimMode("titlebar_shuttle", True, True) == WM_ANIM_TITLEBAR);
}

static void test_serialization_and_bounds(void)
{
    assert(strcmp(anim_style_to_token(DTWM_UI_ANIM_NONE), "none") == 0);
    assert(strcmp(anim_style_to_token(DTWM_UI_ANIM_BEVEL), "bevel") == 0);
    assert(strcmp(anim_style_to_token(DTWM_UI_ANIM_TITLEBAR), "titlebar") == 0);
    assert(strcmp(anim_style_to_token(DTWM_UI_ANIM_WIREFRAME), "wireframe") == 0);

    char small[48] = "";
    append_dtwm_res(
        small,
        sizeof(small),
        "Dtwm*minimizeAnimation: %s\nDtwm*animateMinimizeRestore: %s\n",
        anim_style_to_token(DTWM_UI_ANIM_BEVEL),
        "True"
    );
    assert(strlen(small) == sizeof(small) - 1);
    assert(strncmp(small, "Dtwm*minimizeAnimation: bevel\n", 30) == 0);

    /* Subsequent appends onto a full buffer must be safe no-ops */
    append_dtwm_res(small, sizeof(small), "Dtwm*maximizeAnimation: wireframe\n");
    assert(strlen(small) == sizeof(small) - 1);
}

int main(void)
{
    test_wayland_defaults_and_choices();
    test_x11_defaults_and_roaming_fallback();
    test_serialization_and_bounds();
    return 0;
}
