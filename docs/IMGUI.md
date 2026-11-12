# `osgx::imgui` — Dear ImGui overlay

`osgx/ImGui.hpp` (gated behind `OSGX_IMGUI`; set via the `OSGX_WITH_IMGUI` CMake option, which
compiles Dear ImGui itself — vendored as the `ext/imgui` submodule, pinned to a plain release tag,
not the docking branch — into `libosgx` whenever that submodule is checked out) provides
`osgx::imgui::Widget`, a self-contained Dear ImGui overlay with pluggable sections
(`addStatsSection()`, `addProfilerSection()`, `addTextureSection()`, or a custom
`addSection(label, fn)`), and `osgx::imgui::Panel`, the same section machinery for an
application that already owns its own ImGui context/frame/window.

`addProfilerSection()` is the [`osgx::debug`](DEBUG.md) front-end — the only place `osgx::imgui`
reaches into that subsystem; everything else (`Panel`, the stats/texture browsers) has nothing to
do with `GL_KHR_debug` specifically.

See `examples/osgx-imgui.cpp` (self-owned context) and `examples/osgx-imgui-external.cpp`
(app-owned context/frame/window pattern) for worked examples.

## Docking (`osgx::imgui::Options`)

`Widget`'s window is docked to the left edge by default (`Options::dock`, `Dock::LEFT`/`RIGHT`/
`NONE`; `dockWidth` pixels wide, full window height). Dragging the docked window's inner edge
changes its width. The button at the right end of its title bar undocks it into a free, movable,
resizable window (its last floating position/size is restored) and docks it back, to
`Options::dock`'s edge (or the left one if that is `NONE`); `Widget::isDocked()/setDocked()`
(Python: `Widget.docked`) is the same state. The button follows Dear ImGui's own title-bar buttons
(`imgui_internal.h`'s `ItemAdd()`/`ButtonBehavior()`), so a Dear ImGui upgrade can touch it. With `Options::dockReserve` (default on), the
docked window also takes its strip away from the scene: every frame, the view's master camera
viewport is set to the rest of the window and its projection rescaled per the camera's
`ProjectionResizePolicy` (the same correction OSG applies on a window resize). Collapsing the window
via its title-bar triangle gives the strip back to the scene. Manipulators read viewport-relative
pointer coordinates, so they only respond inside the scene's area; code reading raw window
coordinates (`GUIEventAdapter::getX()/getY()`) for scene-space work, e.g. picking, has to offset
them by the viewport origin.

`dockReserve = false` draws the docked window over the full-window scene instead (the previous
behavior), and `Dock::NONE` gives a free-floating window. Python: `osgx.imgui.Options(dock=...,
dock_width=..., dock_reserve=...)`.

## Two ways to get Dear ImGui: vendored, or your own

`OSGX_WITH_IMGUI` has two build-level modes, matching whether the host application already builds
its own Dear ImGui:

- **You don't already use ImGui (the default)** — leave `OSGX_IMGUI_TARGET` unset. osgx compiles
  `ext/imgui` (a submodule pinned to a plain release tag — deliberately not the docking branch;
  `osgx::imgui`'s `Dock::LEFT/RIGHT` only pins a window to an edge, no drag-to-dock UI to justify
  tracking an untagged branch) straight into `libosgx`.
- **You already use ImGui** — set `-DOSGX_IMGUI_TARGET=your::imgui::target` (a CMake target
  providing Dear ImGui's headers and compiled `ImGui::` symbols), defined *before*
  `add_subdirectory(osgx)`. osgx then skips `ext/imgui` entirely and links against that target
  instead. This isn't optional politeness: Dear ImGui's context is process-global, non-inline
  symbols, so two independently-compiled copies linked into the same binary either fail to link
  (duplicate symbols) or, worse, silently pick one copy's globals via ELF symbol interposition
  (osgx's default shared-library build on Linux) instead of erroring cleanly.

Either way, `osgx::imgui::Panel` (see above) is the API-level half of "defer to your pipeline" —
it never creates a context/backend/window of its own, only `Widget` does.
