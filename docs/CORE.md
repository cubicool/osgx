# `osgx` core — everything not under its own namespace

Reference for the always-available, non-namespaced parts of `osgx.hpp` — the classes and helpers
that live directly in `osgx::`, one section per header. For the opt-in, explicitly-`#include`d
subsystems (each with its own C++ namespace), see [DEBUG.md](DEBUG.md), [IMGUI.md](IMGUI.md),
[PLATFORM.md](PLATFORM.md), and [GLTF.md](GLTF.md) instead.

> [!NOTE]
> Most types below are built via a static `Type::create(...)` rather than `osgx::make_ref<Type>(...)`.
> `make_ref` is for a plain constructor call; `create()` is for a factory that assembles several
> real OSG objects — a camera, its FBO attachments, a depth-only `Program`, an SSBO and its
> uniforms — and hands back the finished bundle as one struct, work no single constructor could do.
> Every osgx-owned factory converges on this one call shape (`create()`, or a distinctly-named verb
> like `::load()`/`::prepare()` only when the operation genuinely isn't "build one" — see
> `PBRIBLEnvironment` in [GLTF.md](GLTF.md)) rather than a taxonomy of `create*`/`make*` free
> functions — one predictable shape to remember beats memorizing which verb applies where. A
> function returning a type osgx doesn't own (`osg::Camera`, `osg::TextureCubeMap`, ...) stays a
> free function, since there's no class to hang a static method on.

## `osgx/Core.hpp`

- `OSGX_DISABLE_WARNINGS` / `OSGX_ENABLE_WARNINGS` silence noisy OSG headers with compiler-specific diagnostic push/pop macros (`__clang__` branch first, then `__GNUC__`, then a no-op fallback).
- `ObjectPath` — a `std::list<std::string>` dotted-path accumulator (`.str()` joins with `.`), used by `DescribeSceneVisitor` to track scene-graph name hierarchies.
- `vec_t` (`osg::Vec3::value_type`) and the `_v`/`_sz`/`_z` literals provide compact OSG scalar and `std::size_t` literals.
- `OSGReferenced`, `OSGArray`, and `OSGDrawElements` are concepts that constrain helpers to the expected OSG base types.
- `make_ref<T>(args...)` / `make_ref<T>(nullptr)` and `make_nref<T>(name, args...)` create `osg::ref_ptr` objects, optionally naming the object immediately.
- `tick` and `call(func, args...)` provide lightweight `osg::Timer`-based timing around arbitrary callables, returning `std::pair<optional<Result>, delta_ticks>`.
- `getFirstParent<T>()` walks an OSG parent chain and returns the first parent matching a requested type.
- `ring_buffer<T, N>` and `aring_buffer<T, N>` keep fixed-size recent samples, with the arithmetic version adding `.average()` (all valid samples) and `.average(count)` (last N). `RING_BUFFER_T(T, N)` exposes protected members in subclasses via `using`.
- `findDataFile()` wraps the `osgDB` file utils, letting you specify multiple paths/suffixes in one call.

## `osgx/Visitors.hpp`

- `LambdaVisitor<Node>` — `NodeVisitor` wrapping `std::function<void(Node&)>`.
- `IndexedVisitor` — tracks traversal depth in `_i`; use `itraverse()` instead of `traverse()`.
- `NameVisitor` — auto-assigns `$ClassName_N` names to unnamed nodes (`CLASS`/`PATH`/`FORCE` options).
- `DescribeSceneVisitor` — prints the scene tree to stdout with indentation and dot-path tracking.
- `VisitorEventHandler<Visitor, Viewer>` — runs a visitor against the scene root when a key is pressed.
- `LambdaKeyHandler` — wires one or multiple keys to a lambda `(ea, aa[, key]) -> bool`.
- `FilterNotifyHandler` — an `osg::NotifyHandler` that suppresses known-noisy OSG messages by regex (draw()/cull() spam, BufferObject release messages) before writing the rest to stderr.

## `osgx/Array.hpp`

- `Array<BaseArray>` wraps `osg::Vec2Array`/`Vec3Array`/`Vec4Array`/`FloatArray` (aliased directly under those names in `osgx::`) with:
  - Constructors from initializer-list, an input range, variadic args (each convertible to the element type), or — since 2026-08-18 — an element *count* (`explicit Array(std::size_t count)`, preallocates default-constructed elements, matching `BaseArray`'s own sized constructor). For `osgx::FloatArray` specifically, a bare `int`/`unsigned` literal still prefers the variadic single-element constructor over the sized one (exact-match template deduction beats the `int→size_t` conversion) — pass an actual `std::size_t` (e.g. the `_sz` UDL) to select the sized constructor unambiguously there. Non-arithmetic element types (`Vec2/3/4Array`) have no such ambiguity.
  - `append_range()`, `append_n<N>()` (compile-time count), `append_n(value, n)` (runtime count).
  - `span()` / `span(start, count)` — `std::span` views (mutable or const).
  - `view()` / `view(start, count)` — `std::ranges::subrange` views.
  - `static create(...)` factory returning `osg::ref_ptr<Array>`.
  - Fully interchangeable with native `osg::*Array` — same layout, same serialization, `dynamic_cast`/`static_cast` compatible in both directions (see `examples/osgx-array.cpp`).
- `DrawElements<BaseElements>` wraps `osg::DrawElementsU{Byte,Short,Int}` (aliased as `DrawElementsUByte`/`UShort`/`UInt`) with:
  - Constructors from `GLenum mode` + initializer-list / range / variadic.
  - `append()`, `append_range()`, `checked_push()` (bounds-checked, throws on overflow).
  - Static factories: `triangles()`, `lines()`, `strip()`, `fan()`.
  - `span()`, `view()`, `static create(...)`.

## `osgx/Callbacks.hpp`

- `CallbacksGroup<Callback>` — composite callback that fans out to multiple registered callbacks, in list order, side by side (**not** chained via `Callback::setNestedCallback`). Aliased as `CameraDrawCallbacksGroup`, `NodeCallbacksGroup`, `DrawableDrawCallbacksGroup`.
  - `add(cb)`/`remove(cb)` — identity-based.
  - `size()`/`get(i)`/`set(i, cb)`/`removeAt(i)`/`insert(i, cb)` — index-based, added so bindings can expose a real sequence proxy instead of `py::dynamic_attr()`.
- `LambdaCallbackBase<Callback, Fn>` + concrete `CameraDrawLambdaCallback` / `NodeLambdaCallback` — adapt a lambda to the matching OSG callback type.
- `WriteTextureCallback` — a `Camera::DrawCallback` that asynchronously writes a texture to disk on demand (`.write(filename)`, atomic-`bool`-flag triggered).

## `osgx/Picking.hpp`

Texture-based object-ID picking: an RTT camera renders a flat "pick ID" shader (1-based; 0 =
background), encoded as 32-bit RGBA.

- `makePickCamera(w, h, image*)` / `makePickCamera(w, h, Texture2D*)` — assembles the pick camera (shader, `BlendFunc`, small-feature culling disabled, `ABSOLUTE_RF`).
- `decodePickID(px)` — decodes all 4 RGBA bytes into a 32-bit ID.
- `PickRule` (`std::function<uint32_t(const uint8_t*, int)>`) — `spiralPick` (default), `pickCenter`, `pickMostCoverage`, `pickNearestToCenter`.
- `PickReadback` — shared atomic state (`onPick`/`onEnter`/`onLeave`, mouse position, last ID); `PickReadbackSync`/`PickReadbackAsync` are the SYNC (`osg::Image` readback) and ASYNC (`Texture2D` + PBO + `glGetTexImage`) variants, each with `Mode::CLICK`/`Mode::CONTINUOUS`.
- `PickCameraSync` — syncs the pick camera's view/projection from the viewer camera every update traversal (with an optional 1×1 sub-frustum for continuous hover).
- `PickHoverCallback` — polls `lastID()` on the update thread and fires `onEnter`/`onLeave` on transitions; the correct way to trigger scene-graph mutation from a hover event.
- `PickHandler` — routes click/move events to the readback (`continuous=true` for hover, `consumeEvents=true` for exclusive picking).

See `examples/osgx-picking.cpp` (full SYNC/ASYNC × click/continuous matrix) and `examples/osgx-hover.cpp` (`onEnter`/`onLeave` driving real scene mutation).

## `osgx/Manipulators.hpp`

- `MultiCameraManipulator` — composite manipulator routing input to one active `Target` (name, manipulator, optional dedicated camera/scene, optional `setActive` callback); `addTarget()`, `activate(index)`/`next()`, `getActiveIndex()`/`getNumTargets()`, key-toggle via `setToggleKey()` (default `'x'`).
- `Ortho2DManipulator` — orthographic 2D camera manipulator owning both view *and* projection matrices. Pan (drag), geometric zoom (scroll), pixel-nudge zoom (Shift+scroll), optional Ctrl-drag 3D tilt (yaw/pitch tracked as independent angles, pitch clamped to ±89°), automatic near/far. Unrotated plane configurable via `setPlaneNormal()`/`setScreenUp()` (default XY, +Z normal).
- `OrbitAxisManipulator` — a "turntable" manipulator: orbits a fixed vertical guide line through the model's bounds, always looking level, dollying on zoom. Mouse move/drag is always active (no button needed), bounded like a trackpad by the screen edge unless composed with `osgx::CursorCapture` via `orbitByDelta()`/`setLiveOrbitEnabled(false)`. Orientation configurable via `setUpAxis()`/`setHomeDirection()` (default Z-up, from -Y).
- `CameraManipulator<Base=osgGA::TrackballManipulator>` — wraps any `osgGA::CameraManipulator` with `addUpdateCameraCallback(osg::Callback*, runOnce)` / `removeUpdateCameraCallback()`, an async-apply queue (safe to mutate mid-callback-iteration), and `currentTime()` sourced from the FRAME event (not a polled `osg::Timer`) for [`osgx::CameraIntents`](#osgxcameraintentshpp) to read.

## `osgx/CameraIntents.hpp`

Plain `osg::Callback` subclasses meant to be attached via `CameraManipulator<Base>::addUpdateCameraCallback()`, driven by real `osgAnimation::Motion`/`CompositeMotion` timing rather than hand-rolled elapsed/duration math.

- `Viewpoint` — `{eye, center, up}` (`up` defaults to `+Z`).
- `FlyToCallback` — animates the camera through one or more `Viewpoint` legs (eye lerped, orientation slerped — never lerping two `lookAt()` centers directly). `osgAnimation::Motion::CLAMP` (default): on arrival, writes the exact final pose, resyncs the manipulator via `setByMatrix()`, then goes permanently inert. `Motion::LOOP`: the whole path repeats forever (author a waypoint list whose ends coincide for a seamless loop — same convention as `osg::AnimationPath`). `ease` is any `float(float)` callable (`defaultEase` = `InOutCubicFunction`), shared across every leg.
- `ShakeCallback` — decaying rotational jitter, right-multiplied onto whatever's already in the camera's view matrix (never touches the manipulator's own state). `CLAMP` (default) decays once and goes inert; `LOOP` repeats for a persistent "idle rumble."

See `examples/osgx-manipulator.cpp` for patrol (`LOOP`) and arrival-latch usage, and the `osgx/Cursor.hpp` section below for composing `OrbitAxisManipulator` with `CursorCapture`.

## `osgx/Cursor.hpp`

Cursor tracking, visibility, and soft capture, built purely on portable `osgGA`/`osgViewer`
virtuals (`GraphicsWindow::useCursor()`, `View::requestWarpPointer()`, `GUIEventAdapter`). Used to
live under `osgx::platform` (grouped there because that's where the motivating need — picking —
first came from), but collapsed into plain `osgx::` since none of it actually needs X11; see
[PLATFORM.md](PLATFORM.md) for the one piece that's genuinely platform-specific
(`isCursorInWindow()`).

- `setCursorVisible(view, visible=true)` / `warpCursor(view, x, y)` — small standalone action
  helpers (not a get/set pair — OSG's `GraphicsWindow` has no visibility getter of its own).
- `CursorState` — thread-safe, event-driven cursor position (`x()`/`y()`) plus `inWindow()` and
  `yIncreasingDownwards()`, the generalized, picking-agnostic version of `osgx::PickReadback`'s
  positional half. `yIncreasingDownwards()` mirrors the real `GUIEventAdapter::
  getMouseYOrientation()` of whichever event last updated the state — pass it (not a hardcoded
  literal) as `windowToNDC()`/`unprojectToPlane()`'s own `yIncreasingDownwards` argument
  ([`osgx/Projection.hpp`](#osgxprojectionhpp) below documents why that value is real per-platform/
  per-event state and can't be safely guessed). Fail-safe default is `false` - `(0, 0)` at the
  bottom-left, matching GL/OSG's own native NDC/framebuffer convention - until the first refresh.
- `CursorHandler` — a `GUIEventHandler` that forwards MOVE/DRAG events into a `CursorState`.
  Always returns `false` so the active manipulator (or any other handler) still sees the event.
- `CursorCallback` — an `osg::NodeCallback` that fires a `std::function<void(int, int)>` every
  update traversal with the current cursor position; the consumer decides what "follow" means
  (reposition a HUD quad, unproject onto a world plane, drive a rendered software cursor).
  Optionally takes a second `std::function<bool()>` (`inWindowCheck`), polled every traversal to
  refresh `CursorState::inWindow()` — deliberately not a hard call into
  `osgx::platform::isCursorInWindow()` (this header is core; that function lives in the optional,
  X11-only `osgx::platform` module), so a caller built with `OSGX_PLATFORM` wires it in explicitly:
  `[cam]{ return osgx::platform::isCursorInWindow(cam); }`.
- `makeCursorUniformCallback(state, uniform, inWindowCheck={})` — convenience factory, not a new
  class: builds a `CursorCallback` whose `fn` pushes `state`'s live position + in-window flag into
  the caller-owned `uniform` (must be `FLOAT_VEC3`: x, y, `inWindow ? 1.0 : 0.0`) every update
  traversal. Deliberately a plain `Uniform` + update callback, not a `StateAttribute` pushing the
  uniform directly — see `osgx::LightSet::apply()`'s own history in [`osgx/PBR.hpp`](#osgxpbrhpp)
  for why that specific shortcut is unreliable under a sibling `Program`'s `StateAttribute::OVERRIDE`.
- `CursorCapture` — a `GUIEventHandler` implementing the standard hide+warp+accumulate trick for
  turntable/FPS-style relative-motion look controls: while `setCaptured(true)`, hides the cursor
  and re-centers it on every move, accumulating the delta for `consume()` to poll once per update
  traversal. **Not** true OS-level pointer confinement (nothing stops the cursor visibly darting to
  the screen edge for one frame between warps on some window managers) — that needs real
  `XGrabPointer` work, tracked in `ai/todo-platform.md`. Deliberately not wired into
  `OrbitAxisManipulator` directly — compose the two at the application level instead (add both as
  event handlers, feed `consume()`'d deltas into `orbitByDelta()`); see `examples/osgx-manipulator.cpp`.

## `osgx/Projection.hpp`

CPU-side screen/world projection helpers — the C++ twin of a shared GLSL snippet, and a general
primitive for cursor/screen-driven world-space interaction (a mouse-dragged gizmo, a ground-plane
pick, a HUD element following a 3D point). Not cursor-specific itself and has no dependency on
`osgx::CursorState`/`CursorCallback` — it earned its own header rather than living in `Cursor.hpp`
for that reason. Motivated by `~/dev/osgSlug/examples/python/pyosgslug-cone-widget.py`'s prototype,
whose own `make_unprojector()` reconstructed a camera basis from fovy/aspect by hand (a workaround
only needed because the Python bindings there have no bound `vec * matrix` operator); this instead
inverts `view*projection` directly, which also works for orthographic projections, unlike
fovy/aspect reconstruction.

- `windowToNDC(viewport, x, y, yIncreasingDownwards)` — window/event coordinates → NDC
  (`[-1, 1]^2`, Y-up). `(x, y)` must be window-absolute (same space as `GUIEventAdapter::getX()`/
  `getY()`); `yIncreasingDownwards` must match that same event's own
  `GUIEventAdapter::getMouseYOrientation()` — real per-platform/per-event state, not something safe
  to default.
- `Ray { osg::Vec3d origin, direction; }` — `direction` is unit length.
- `unprojectRay(camera, ndcX, ndcY)` — casts a world-space `Ray` from `camera` through an NDC
  point, via `inverse(view*projection)` evaluated at the near/far planes.
- `intersectRayPlane(ray, plane, outPoint)` — ray/plane intersection; `plane` must be normalized
  (`a²+b²+c²=1` — same precondition as `osg::Plane::distance()`, which this reuses directly).
  Returns `false` for a parallel ray or a hit behind the ray's origin.
- `unprojectToPlane(camera, viewport, x, y, yIncreasingDownwards, plane, outPoint)` — one-call glue
  of the three above; this is exactly what the cone-widget prototype's `make_unprojector()` +
  `ndc_from_event()` + its own z=0-plane solve did by hand, per MOVE event.
- `unprojectPoint(camera, ndcX, ndcY, ndcDepth)` — single-point unproject at an explicit depth,
  the CPU-side twin of the shared GLSL `osgx_Unproject(vec2 ndc, float depth)` below, mirroring
  its signature exactly (unlike `unprojectRay()`, which only ever exposes the near/far pair
  together). Motivated by the `osgx-aoe` example's Mode 2: reading a G-buffer depth sample back
  to the CPU at the cursor's pixel and turning that `(x, y, depth)` triple into a world point.
- `registerProjectionShaderLibs()` — publishes two entries under the `osgx::projection`
  shader-library catalog tag (not a C++ namespace — see [Namespaces](#namespaces)):
  - `#pragma osgx::projection UNPROJECT` → `vec3 osgx_Unproject(vec2 ndc, float depth)`, matching
    `unprojectRay()`'s own math exactly. Extracted from a GLSL function `examples/osgx-grid.cpp`
    and `examples/osgx-turntable.cpp` used to duplicate verbatim; both now use the shared
    `#pragma` instead.
  - `#pragma osgx::projection DEPTH` → `float osgx_LinearizeDepth(float depth, mat4
    projectionMatrix)` (depth-buffer sample → distance from the camera along the view axis),
    deriving near/far implicitly from `projectionMatrix`'s own entries instead of a separate
    `znear`/`zfar` uniform pair the caller must decompose by hand and keep in sync. Extracted
    from `OpenSceneGraph.py/examples/pyosg-rtt.py`'s and `pyosg-mrt.py`'s own identical,
    previously-duplicated `linearizeDepth(d, near, far)`. `projectionMatrix` is a REQUIRED
    parameter, deliberately not read from the ambient `osg_ProjectionMatrix` uniform the way
    `osgx_Unproject()` does: that ambient value is only ever the currently-drawing camera's own
    projection, correct inline in the same pass that produced the depth, but silently wrong the
    moment the depth sample came from a DIFFERENT camera — exactly the G-buffer-geometry-pass →
    separate-composite-pass shape both files actually use, which is also why their own
    `invProjectionMatrix`/`znear`/`zfar` uniforms need a `preDrawCallback` bridging the
    original camera's live matrix into the composite pass every frame in the first place (same
    shape as this repo's own `UpdateLightingPassCallback` in `examples/osgx-gbuffer.cpp`) — that
    bridge can't be eliminated (OSG's automatic per-camera uniforms have no way to carry a
    different camera's matrix across passes), only made unambiguous at the call site.

## `osgx/SDF.hpp`

A small catalog of pure, closed-form 2D signed-distance functions (negative = inside), published
only as a GLSL shader-library entry — there is no CPU-side counterpart, unlike `Projection.hpp`'s
CPU/GLSL twins, since these are pure per-fragment shape tests with no meaningful CPU-side
analogue. Extracted 2026-09-18 from `osgSlug`'s `Atlas.shaders.cpp`, which originally defined
these itself (as `osgSlug_SDF_*`) to back its own `slughorn::Mask` dispatch — every one of them
was already pure math with zero dependency on osgSlug's own state, a clean extraction boundary.
`osgSlug` now consumes the shared catalog instead of defining its own copies; only the
Mask-struct-specific dispatch/coverage/MSDF glue stayed behind in `osgSlug`, since that part IS
genuinely osgSlug-specific.

- `registerSDFShaderLibs()` — publishes ONE entry, `#pragma osgx::sdf SHAPES`, expanding to all
  nine functions below at once (not individually-selectable tags the way `Projection.hpp`'s
  `UNPROJECT`/`DEPTH` are): a `Mask`-style runtime dispatch needs every shape function compiled
  in regardless of which one is actually active per-fragment, so no real caller would ever want
  a subset.
- `float osgx_SDF_Circle(vec2 p, vec2 center, float r)`
- `float osgx_SDF_Rect(vec2 p, vec2 center, vec2 halfExtents)`
- `float osgx_SDF_Capsule(vec2 p, vec2 a, vec2 b, float r)`
- `float osgx_SDF_Arc(vec2 p, vec2 center, float r, float angleStart, float angleEnd)` — filled
  pie sector; angles in radians, standard math convention (0 = +X, CCW positive).
- `float osgx_SDF_ArcBand(vec2 p, vec2 center, float r, float angleStart, float angleEnd, float strokeHalfWidth)`
  — a stroked arc (annular band along an arc), not a filled sector.
- `vec2 osgx_SDF_Rotate(vec2 p, float angle)` — rotates `p` by `angle` (CCW, radians); every
  rotatable shape below pre-rotates its query point by `-rotation` into the shape's own local
  frame, same trick each time.
- `float osgx_SDF_Hexagon(vec2 p, vec2 center, float r, float rotation)` — flat-top at `rotation=0`.
- `float osgx_SDF_Octagon(vec2 p, vec2 center, float r, float rotation)`
- `float osgx_SDF_Star(vec2 p, vec2 center, float r, float points, float innerRatio, float rotation)`
  — `points` rounded to the nearest integer ≥ 3; `innerRatio` in `[0, 1]` (0 = sharpest spikes,
  1 = a regular n-gon).

Hexagon/Octagon/Star are Inigo Quilez's exact SDFs
([iquilezles.org/articles/distfunctions2d](https://iquilezles.org/articles/distfunctions2d/)).
Two names changed during the extraction to match `slughorn::Mask::Type`'s own vocabulary (the
actual public data-model API) rather than the GLSL functions' own previously-independent names:
`osgSlug_SDF_Box` → `osgx_SDF_Rect`, `osgSlug_SDF_Pie` → `osgx_SDF_Arc`. Every other name carried
over unchanged (just the `osgSlug_` → `osgx_` prefix swap).

## `osgx/Grid.hpp`

- `Grid` draws a procedurally generated, antialiased grid as either a screen-space overlay or a perspective ground plane. The shader is adapted from Ben Golus's [The Best Darn Grid Shader (Yet)](https://bgolus.medium.com/the-best-darn-grid-shader-yet-727f9278b9d8) (credited in `src/Grid.cpp`/`src/osgx/Grid.hpp` as well).

## `osgx/Shapes.hpp`

- `VertexLayout` — the generic-attribute locations (position/normal/uv) a generated `Polyhedron` installs, both through Geometry's conventional arrays (bounds/compatibility) and as explicit generic attributes (core-profile shaders).
- `Polyhedron` — an `osg::Geometry` built from `vertices` + `Face` list (each face: vertex indices + optional per-corner UVs). `rebuild()` mutates the existing backing arrays in place rather than replacing them (same "don't reallocate every frame" lesson `osgx::LightMarkers` reuses). Per-face custom attributes via `setFaceVertexAttribute()`/`setFaceAttribute()`/`removeAttribute()`; `faceNormal()`/`faceUp()`/`restingOffset()`/`faceRestingOffset()` for placement queries; `isometricFaceUV()` static helper.
- `Cube`, `Tetrahedron`, `Octahedron`, `Icosahedron`, `Dodecahedron`, `PentagonalTrapezohedron` — concrete `Polyhedron` subclasses, each constructible from `(center, radius, layout)` (`Cube` also takes `(center, size, layout)`).

## `osgx/Shader.hpp`

Generic, line-oriented GLSL library expansion — a reusable snippet catalog rather than a
project-specific search-and-replace pass. Register one or more catalogs (`osgx::registerShaderLibs()`),
then expand `#pragma` directives in shader source via `osgx::resolveShaderLibs()`. Registered
namespace/library names are case-insensitive; a pragma accepts comma-separated library names,
optional GLSL-function aliases, and `*` to expand an entire catalog in registration order. See the
worked example in the main [README](../README.md#osgxhpp--public-headers).

This header also holds `Hook`/`HookList`/`applyHooks()` — the shader-object *substitution*
counterpart to the text-splicing above. A Program-building call site (e.g. `osgx::gltf::pbribl::
PBRIBLScene::create()`, see [GLTF.md](GLTF.md)) declares which `Hook` slots it supports as a
`defaults` `HookList`; a caller overrides only the slots it cares about via its own `HookList`,
via one shared enum/mechanism instead of each call site growing its own `osg::Shader*
someHook=nullptr` parameter. `applyHooks()` guarantees exactly one shader ends up attached per
supported slot, always — never zero, never two (GLSL permits one body per function, so an override
substitutes the built-in rather than competing with it).

## `osgx/PBR.hpp`

Reusable BRDF GLSL snippets and typed direct lights, living flat in `osgx::` (not its own
namespace — `registerPBRShaderLibs()`'s `"osgx::pbr"` catalog tag is a conventional shader-lib key
only, unrelated to the C++ namespace; see [Namespaces](#namespaces) below).

- GLSL snippets — GGX distribution, Schlick Fresnel, Smith geometry — plain function-body
  snippets, concatenated into a consuming fragment shader via `registerPBRShaderLibs()`/
  `resolveShaderLibs()`, not full shaders of their own.
- `LightSet` — a `StateAttribute` owning a `std430`-SSBO-backed array of typed direct lights (`LightType::Point`/`Directional`/`Spot`; a sphere light is a `Point`/`Spot` with non-zero `sourceRadius`, not a fourth type) and its `osgx_lightCount` uniform. Construct it, then attach it through `StateSet::setAttributeAndModes()` (size `MAX_LIGHTS`, zero-initialized, everything off until `setCount()`+`setPoint()`/`setDirectional()`/`setSpot()`). Its `apply()` binds the SSBO and forwards the owned count uniform through OSG's shader-composition uniform path, so callers cannot desynchronize the two. Each slot also has an `enabled` flag, so `setEnabled()` can toggle a configured light without changing its count or packed data. Typed setters/getters (`getType()`, `getPosIntensity()`, `getColor()`, `getSourceRadius()`, `getDirection()`, `getSpotAngles()`, …) replace the old parallel-`osg::Uniform`-array contract.
- `OrbitLightRig` — the animated counterpart: an `osg::NodeCallback` that writes orbiting position/intensity into a `LightSet` every update traversal (for the subset of lights that should move; a `LightSet` can be shared between a static rig and an orbiting one).

## `osgx/Gizmos.hpp`

Debug visualization for `LightSet` lights — deliberately not part of `osgx::debug`
(that's `GL_KHR_debug` integration specifically, not scene gizmos).

- `LightMarkers` — an `osg::Group` of depth-tested, real scene-space markers for point/sphere/spot lights (up to `MAX_LIGHTS`), rebuilt in place every update traversal from the live `LightSet`. Three orthogonal wireframe circles for a point/sphere light (sized to `max(sourceRadius, minMarkerRadius)`); a wireframe cone (ring + spokes) for a spot light, sized by its outer cone angle and `spotConeLength`. A directional light has no position and is never drawn here.
- `LightGizmos` — bundles `LightMarkers` with a non-depth-tested `POST_RENDER` overlay camera for directional lights (a wireframe plane + direction arrow, sized off the target scene's bounding sphere) into one addable `osg::Group`:
  ```cpp
  auto gizmos = osgx::make_ref<osgx::LightGizmos>(lights, scene, minMarkerRadius, spotConeLength);
  root->addChild(gizmos);
  ```
  `getMarkers()`/`getOverlay()` give access to the two pieces individually, for the (rarer) case where they need different parents in the scene graph.

See `examples/osgx-lights.cpp` for one shaded object cycling through every light type with live gizmo feedback.

## `osgx/IBL.hpp`, `CaptureCubeMap.hpp`, `GGXPrefilter.hpp`, `LambertianBake.hpp`

Reusable IBL GLSL snippets (`registerIBLShaderLibs()`'s `"osgx::ibl"` catalog tag, same
flat-namespace/conventional-tag split as `PBR.hpp` above) plus environment-map loading, BRDF-LUT
baking, SH9/Lambertian diffuse irradiance, and cubemap readback helpers.

- `SharedBRDFLUT::create(lutSize)` — the process-wide BRDF-LUT cache. Deliberately named `create()`
  despite sometimes returning an *existing* cached LUT rather than baking a fresh one — see its own
  doc comment for the cache-or-create contract.
- `readCubeMapFaces()` / `BRDFLUTReadback` — CPU readback helpers for a baked cubemap/LUT.
- `CaptureCubeMap.hpp` — `CaptureCubeMapScene`, the low-level frame-driven reflection-probe primitive (six ordered FBO cameras capturing a caller-owned scene into a radiance cubemap). `CaptureCubeMapScene::create()`/`::recapture()`.
- `GGXPrefilter.hpp` — GPU GGX prefilter scene construction, rebaking, and readback (`GGXPrefilterScene::create()`/`::rebake()`, `GGXPrefilterReadback::finish()`).
- `LambertianBake.hpp` — frame-driven GPU Lambertian/diffuse cubemap baking and readback (`LambertianBakeScene::create()`/`::rebake()`, `LambertianCubeReadback::finish()`).

glTF-specific material and rendering integration lives with the loader in [`osgx::gltf`](GLTF.md) —
generic `osgx` does not depend on or duplicate its public shader interface.

## `osgx/Shadow.hpp`

Directional shadow mapping, shared by any `LightSet`-lit scene (nothing here is glTF/PBR-specific;
`osgx::gltf::pbribl` consumes it as an optional parameter — see [GLTF.md](GLTF.md)). Lives flat in
`osgx::` — `registerShadowShaderLibs()`'s `"osgx::shadow"` catalog tag is a conventional shader-lib
key only, same split as `PBR.hpp`/`IBL.hpp` above.

Only ONE light — the key/directional light — is ever shadowed; point/spot-light shadows need a
cubemap and meaningfully different frustum math, and remain a separate, unimplemented feature (see
TODO.md's Shadow section).

- `ShadowMapOptions` — `size` (shadow-map resolution), `extent` (half-width of the orthographic
  frustum's box; `0` derives it from `sceneBoundRadius * margin`), `margin` (scales both the
  derived `extent` and near/far — keeps near:far bounded to a healthy ratio regardless of scene
  scale, avoiding depth-precision collapse on a large scene), `bias`, `strength` (`0` = no effect,
  `1` = fully black).
- `ShadowMap` — owns the `PRE_RENDER` depth-only `camera` (add it to the scene graph) plus the
  uniforms `DIRECT_LIGHTING_HOOK_SHADOWED` reads every frame (`shadowMatrix`, `bias`, `strength`,
  `casterIndex` — which `LightSet` index this shadow is cast by/matched against, default `0`).
  - `ShadowMap::create(lightDirection, sceneBoundCenter, sceneBoundRadius, options={})` — builds an
    **orthographic** depth-only camera (the physically-correct frustum shape for a directional,
    parallel-ray light) with its own minimal depth-only `Program` (`ON|OVERRIDE`) — a caster's own,
    potentially expensive, main-render `Program` never runs during the shadow pass. Not glTF-alpha-mask
    aware by design; a caller needing alpha-cutout shadows overrides the Program on that geometry's
    own StateSet.
  - `updateMatrix()` — recomputes `shadowMatrix` from `lightView`/`lightProj` after mutating either
    directly.
  - `reposition(lightDirection, sceneBoundCenter, sceneBoundRadius, options={})` — repositions an
    *existing* `ShadowMap` in place (no new camera/FBO/depth-texture allocation), cheap enough to
    call every frame for an interactively-moving light (e.g. an ImGui-dragged direction). `create()`
    remains the right call for a light fixed at scene-build time.
- `DIRECT_LIGHTING_HOOK_SHADOWED` — a drop-in replacement for `PBR.hpp`'s
  `DIRECT_LIGHTING_HOOK_DEFAULT`: identical per-light dispatch loop, except the light at
  `osgx_shadowCasterIndex` has its contribution multiplied by `osgx_ShadowFactor()` (world-space PCF
  3×3 shadow test). Both define `osgx_DirectLighting()` with the same signature, so swapping hooks
  is the only shader change needed — see `examples/osgx-shadow.cpp` for the full A/B wiring
  (press `s` to toggle).

See `examples/osgx-shadow.cpp` (standalone `LightSet` + `ShadowMap` proof, live-draggable light
direction) and `examples/osgx-gbuffer.cpp` (the same shadow map plugged into the deferred pipeline
below).

## `osgx/GBuffer.hpp`

Generic deferred G-buffer camera setup — not PBR/glTF-specific. `osgx::gltf::pbribl`'s own deferred
split (`PBRIBLGBuffer::create()`, see [GLTF.md](GLTF.md)) is built on top of this, not a separate
mechanism, and a non-PBR deferred shader can use it directly too. Lives flat in `osgx::`, same
reasoning as `Shadow.hpp` above.

- `AttachmentFormat` — texture internal-format presets for one G-buffer color attachment: `RGBA8`
  (ordinary LDR color/albedo), `RGB16F` (signed `[-1,1]` data, e.g. a view-space normal, needing no
  encode/decode remap), `RGBA16F` (HDR color, e.g. emissive, which can exceed `1.0` before
  tonemapping), `RGBA32F` (real eye-space position, written straight from the vertex shader rather
  than reconstructed from depth — see `PBRIBLGBuffer::positionTexture`'s note in GLTF.md for why
  that reconstruction is unreliable across nested `PRE_RENDER` cameras).
- `GBuffer` — `camera` is the `PRE_RENDER` FBO pass that writes `colorTextures` (indexed exactly as
  passed to `create()`) plus `depthTexture`. The caller still owns adding `camera` to the scene
  graph.
  - `GBuffer::create(node, width, height, colorFormats, referenceFrame=RELATIVE_RF)` — `node` is a
    real 3D scene subgraph (a geometry pass, not a fullscreen quad), writing
    `colorFormats.size()` simultaneous color attachments (`COLOR_BUFFER0..N`, requiring
    `layout(location = n) out` declarations in the caller's fragment shader) plus a real
    `GL_DEPTH_COMPONENT24` depth attachment. `referenceFrame` defaults to `RELATIVE_RF` (composes
    with `node`'s own ancestor transforms/camera); pass `ABSOLUTE_RF` for a camera that should own
    its own fixed view/projection instead (`ShadowMap::create()` builds its own camera directly
    rather than going through this helper, since it's depth-only with no color attachments at all).

- `SSAO` — hemisphere-kernel screen-space ambient occlusion, reading any G-buffer's view-space
  normal + position channels directly and nothing else (glTF-material-shaped, hand-authored, or
  otherwise). Ported from `OpenSceneGraph.py/examples/pyosg-lighting/11-sketchfab.py`'s proven-live
  implementation: a 16-sample hemisphere kernel + a small tiled tangent-space noise-rotation
  texture, a raw RTT pass, then a small box-blur RTT pass denoising it. `radius`/`bias` are live
  `osg::Uniform`s — set them at any time, no pass rebuild needed. `aoTexture` (the blurred,
  single-channel `GL_R8` result) plugs directly into `PBRIBLLightingPassOptions::aoTexture`
  (GLTF.md) or any other consumer wanting a generic occlusion mask.
  - `SSAO::create(normalTexture, positionTexture, projectionMatrix, width, height, radius=0.5, bias=0.02)`
    — `projectionMatrix` is a caller-owned uniform this pass reads every draw; keep it refreshed
    from the same per-frame callback that updates `PBRIBLLightingScene`'s own view-matrix uniforms
    (see that type's own doc comment, GLTF.md, for why it must be a `PRE_RENDER` `preDrawCallback`).
    `radius`/`bias` are scale-dependent (a sane radius is a small fraction of the scene's own
    bounding radius, not a fixed constant) — compute them from the scene being rendered.

See `examples/osgx-gbuffer.cpp` for the full deferred pipeline (`PBRIBLGBuffer` +
`PBRIBLLightingScene`, both built on this), live SSAO wired into `PBRIBLLightingPassOptions::aoTexture`
with live ImGui radius/bias sliders, and a channel-by-channel G-buffer visualizer (press `0`-`6`,
`6` being SSAO's own output). Python: `osgx.GBuffer`/`osgx.SSAO`.

## Namespaces

`osgx::pbr`, `osgx::ibl`, `osgx::shadow`, and `osgx::gbuffer` used to exist as separate C++
namespaces; as of 2026-08-20 every symbol they held lives directly under `osgx::`. The rule: a
namespace exists only for a genuinely separate opt-in subsystem — its own `#include` outside the
`osgx.hpp` umbrella AND its own CMake link target (exactly `debug`/`imgui`/`platform`/`gltf` (+ its
own `gltf::pbribl` sub-target)/`ktx2`). Everything that compiles unconditionally into `libosgx`
stays flat, no matter how "topic-shaped" it feels — this is also why `picking`/`grid`/
`manipulators`/`shadow`/`gbuffer` never got their own namespace. The `"osgx::pbr"`/`"osgx::ibl"`/
`"osgx::shadow"` strings passed to `registerPBRShaderLibs()`/`registerIBLShaderLibs()`/
`registerShadowShaderLibs()` are shader-lib registry catalog tags only — conventional, `#pragma`-
addressable names, unrelated to the (now-flat) C++ namespace.

## `osgx/Version.hpp`

`OSGX_VERSION_MAJOR`/`MINOR`/`PATCH` and the `OSGX_VERSION` string, generated from the CMake
project version. Included by `Core.hpp`, so it's available transitively almost everywhere.
