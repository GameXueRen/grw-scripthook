# Native UI {#ui}

The UI calls build interfaces out of the engine's own widget
classes, drawn by the game's UI renderer, inside scenes the
ScriptHook DLL owns. Nothing is borrowed from the game's HUD:
no template, no font name, no scene of the game's. A plugin
gets a layer of its own, decides where that layer sits
relative to the game's UI, and talks to widgets through the
same property system the engine uses.

`ui_sample.c` is the working example: a window with a title,
four rows, a highlight bar, keys through the input callback
and a rebuild after a world reload, in about a hundred lines.

## Scenes

```c
uint32_t scene = ShUiSceneCreate("my plugin", 10);
```

A scene is a layer. `order` decides where it draws: below 0
is under the game's UI (the minimap draws over it), 0 and up
is over the game's UI, lowest first among ours. Scene 1 is a
shared default that the F4 menu and the overlay slots use;
`ShUiCreate` without a scene goes there. `ShUiSceneShow`
hides a whole layer (hidden scenes cost nothing),
`ShUiSceneSetOrder` moves it, `ShUiSceneDestroy` frees every
widget in it and the scene.

The engine scene behind a slot is made on first use, once
the world is up. `ShUiReady()` says when that is; poll it
from your thread before building.

## Widgets

```c
uint32_t panel = ShUiCreateIn(scene, 0, SH_W_PANEL, 700, 300, 520, 220);
uint32_t row   = ShUiCreateIn(scene, panel, SH_W_LABEL, 16, 44, 480, 34);
```

Four classes: `SH_W_CONTAINER` (bare, holds children),
`SH_W_PANEL` (a container with a translucent quad behind it),
`SH_W_LABEL` (text in the HUD font) and `SH_W_IMAGE` (a
tinted quad, or a texture of yours). Parent 0 is the scene's
root. Containers nest to any depth; a child's position is
relative to its parent. Sibling order is draw order:
`ShUiReparent(id, parent, index)` moves a widget and sets it,
`ShUiChildCount` and `ShUiChildAt` read it back.

Coordinates are a 1920 by 1080 reference space that the
engine scales to the screen. Colours are `0xRRGGBB`; alpha is
0 to 1. `ShUiShow` on a panel hides its children with it.

## Properties

Every widget property the engine exposes is reachable by its
id, typed from the engine's own property tables at runtime:

```c
float pos[3] = { 16, 44, 0 };
ShUiSetV(bar, SH_P_POSITION, pos, 3);
ShUiSetS(row, SH_P_TEXT, "Hello");
ShUiSetF(row, SH_P_ALPHA, 0.5f);
ShUiGetV(row, SH_P_COLOUR, rgb, 3);
```

`ShUiPropType(id, prop)` tells the type (`SH_PT_FLOAT`,
`SH_PT_BOOL`, `SH_PT_UINT`, `SH_PT_VEC2`, `SH_PT_VEC3`,
`SH_PT_STRING`) or 0 when the class has no such property.
The `SH_P_` constants name the ones verified so far; any
other id the class has works the same way. `ShUiPropCount`
and `ShUiPropAt` enumerate what the resolver found.

Labels size themselves to their text once an axis is
automatic: `ShUiSetAutoSize(id, 1, 0)`, then `ShUiMeasure`
returns the laid out width and height.

## Batches

Every edit is one job on the game thread, and a plain call
waits for it (one physics tick). For many edits at once:

```c
ShUiBegin();
/* any number of Set calls, Show, Destroy */
ShUiCommit();            /* one job */
```

Batches are per thread. Creates and reads still run at once;
`ShUiCommitAsync(done, user)` commits from a worker and calls
you back.

## Input

```c
static int OnInput(uint32_t scene, const ShUiEvent *e, void *user) {
    if (e->type == SH_UI_EV_DOWN && e->key == VK_RETURN) {
        ...
        return 1;        /* the game never sees this key */
    }
    return 0;
}
ShUiSetInput(scene, OnInput, NULL);
ShUiFocus(scene, 1);
```

The focused scene receives key down, key up and pointer moves
(`e->x`, `e->y` in the 1920 by 1080 space), and while it
holds focus the keyboard is captured: the game's DirectInput
keyboard device, which is where it reads WASD and the rest,
reports nothing pressed. Escape, Alt, Tab, F4 and the Windows
keys are never hidden, so the player can always reach the
game's menu. Release focus when your window closes. The
callback's return value still marks a key as consumed.
`ShBlockKey(vk, on)` hides one key without focus, and
`ShCaptureKeys(on)` is the capture without a scene.

If you poll a hotkey yourself, test the held bit
(`GetAsyncKeyState(vk) & 0x8000`) and detect the edge; the
"pressed since last call" bit is consumed by whichever thread
polls first, and the UI input thread polls every key.

## Lifecycle

A world reload (death, fast travel, mission restart) destroys
every widget the engine had. Scene ids stay valid; the
widgets are gone. Register a reset callback and rebuild:

```c
ShUiSetReset(scene, OnReset, NULL);
```

It fires from the thread that polls `ShUiReady()` once the
world is back. Widget ids from before the reload are dead and
every call on them fails with `SH_ERR_NO_CANDIDATE`.

## Fonts and textures

New labels use the HUD font and new plates the white 16x16
texture, both found by asset GUID, never by name.
`ShUiSetDefaultFont(guid)` and `ShUiSetDefaultImage(guid)`
change that for widgets made afterwards. `ShUiTextureCreate`
uploads RGBA pixels of your own into an engine texture and
`ShUiImageSet` puts it on an image widget or a panel's plate.

## Errors

`SH_ERR_UI_NOT_READY` means in game but the world is not up
or the scene is not built yet; `SH_ERR_UI_PROP` means the
class has no such property or the value type is wrong;
`SH_ERR_UI_ASSET` means the default font or texture is not
loaded (wrong GUID, or not in Playing yet).

## What it costs

A widget is three engine allocations (handle, private, and
its instance); every byte comes from the engine's pool and
goes back to it on destroy. A scene is ticked once per frame
on the render thread and drawn after each UI pass the game
makes; an empty scene is a few microseconds. There is no
Win32 window anywhere.
