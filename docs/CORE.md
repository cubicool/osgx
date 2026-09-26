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
> like `::load()`/`::prepare()` only when the operation genuinely isn't "build one") rather than a
> taxonomy of `create*`/`make*` free
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

## `osgx/Library.hpp`

`osgx::Library` owns libosgx's process-wide state: the shader-lib catalogs (registered by its
constructor), the `cachedShader()` cache, the `SharedBRDFLUT` textures, and the `PixelText` atlas.
The state is created by the constructor and released by the destructor, so none of it is left for
static destruction at process exit.

```cpp
auto lib = osgx::initialize(arguments); // before the viewer, so the viewer is destroyed first
osgViewer::Viewer viewer(arguments);
```

- Exactly one may be alive; constructing a second throws `std::logic_error`.
- `resolveShaderLibs()`, `registerShaderLibs()`, `cachedShader()`, `SharedBRDFLUT::create()`, and
  `PixelText` throw `std::logic_error` while no Library is alive.
- Libraries built on osgx subclass it (`class Library: public osgx::Library`), which initializes
  osgx first and releases the subclass's state first. `Library::instance<T>()` returns the live
  Library as `T`.
- Binding slots (`osgx::Bindings`, types `UBO`, `SSBO` and `TextureUnit`, each its own index space): each library declares slot
  names, optionally with a preferred index (osgx's texture-unit slots prefer the units they used as
  fixed constants: Material 0-3, Environment 5-7, SDF 10); a slot gets its index on first use, so
  only features a program uses consume indices. GLSL writes `@<name>@` (e.g.
  `layout(std140, binding = @osgx::environment@)`), which `resolveShaderLibs()` substitutes.
  `LibraryOptions::bindings` pins a slot's index by name; `LibraryOptions::reserve` keeps indices
  free for the application's own buffers (`reserveBelow(type, count)` reserves `[0, count)`, so
  osgx assigns that type from `count` up). Two pins sharing an index, or a pin naming an undeclared
  slot, throw. `Bindings::slots()` lists every declared slot with its index once assigned.
  `examples/osgx-library.cpp` shows all three for an application with its own shaders (a declared
  `app::globals` slot, a reserved hard-coded SSBO 0, a pinned `osgx::material`).
- Python: `lib = osgx.initialize(bindings=..., reserve=...)`, kept referenced until the viewer is
  done; `lib.binding(name)`, `lib.declare(type, name, preferred=None)`, `lib.slots()`. The Python
  module's subclass also owns `osgx.gltf`'s async-reader texture cache.

### Buffer blocks and samplers

A GLSL block goes in a uniform buffer (UBO, `std140`, `Bindings::Type::UBO`) when all three hold:

1. Its size is fixed at shader compile time (no unsized `[]` array).
2. It fits in 16 KB, the minimum `GL_MAX_UNIFORM_BLOCK_SIZE`.
3. The shader only reads it.

Otherwise it goes in a shader storage buffer (SSBO, `std430`, `Bindings::Type::SSBO`).

When a block qualifies for both:

- UBO: every invocation reads the same data (a material, an environment, a light set). GPUs read
  uniform buffers through a constant cache that is fastest when all invocations read the same
  address.
- SSBO: each invocation indexes different elements (per-glyph, per-layer, per-joint data).

Binding points are the other reason to prefer UBOs. OpenGL 4 guarantees at least 36 uniform-buffer
binding points and 12 uniform blocks per shader stage, but only 8 shader-storage-buffer binding
points.

A block built from 16-byte rows (`vec4`s, or a `vec3` followed by a scalar) has the same byte
layout under `std140` and `std430`; `std140` differs by rounding scalar and `vec2` array strides,
and struct alignment, up to 16 bytes.

In osgx: `Material`, `Environment`, `LightSet`, `GridSettings` and `SDF` are UBOs; the glTF joint
palette (`osgx::joints`) and `PixelText`'s glyph indices (`osgx::pixelText`) are SSBOs.

Samplers: a sampler declared in more than one shader object of a Program takes its unit from a
uniform, not `layout(binding)`. NVIDIA's linker rejects identical `layout(binding)` sampler
redeclarations when the objects declare other bound samplers in a different order (valid GLSL;
glslangValidator links it), and ignores a sampler binding declared only in an object that does not
use the sampler. Uniform and storage blocks redeclared across objects link correctly, provided
every declaration carries the same `binding`.

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
  uniform directly — see `osgx::LightSet::apply()`'s own history in [`osgx/Light.hpp`](#osgxlighthpp)
  for why that specific shortcut is unreliable under a sibling `Program`'s `StateAttribute::OVERRIDE`.
- `CursorCapture` — a `GUIEventHandler` implementing the standard hide+warp+accumulate trick for
  turntable/FPS-style relative-motion look controls: while `setCaptured(true)`, hides the cursor
  and re-centers it on every move, accumulating the delta for `consume()` to poll once per update
  traversal. **Not** true OS-level pointer confinement (nothing stops the cursor visibly darting to
  the screen edge for one frame between warps on some window managers) — that needs real
  `XGrabPointer` work. Deliberately not wired into
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
- `DepthProjectionCallback(name="osgx_depthProjection")` — a `Camera::DrawCallback` that records
  the projection a camera actually drew with into two `FLOAT_MAT4` uniforms, `name` and
  `name + "Inverse"`, for a later pass that samples that camera's depth. Install it as the
  depth-producing camera's post-draw callback and add `getProjection()`/`getProjectionInverse()`
  to the consuming pass's StateSet. OSG clamps each camera's near/far privately during cull, so
  neither the producing camera's `getProjectionMatrix()` nor the consuming pass's own
  `osg_ProjectionMatrix` is the matrix the depth was written with; the one `osg::State` holds
  right after the producing camera draws is.
- The `osgx::projection` shader-library catalog (a catalog tag, not a C++ namespace — see
  [Namespaces](#namespaces); registered by `osgx::Library`) has three entries:
  - `#pragma osgx::projection UNPROJECT` → `vec3 osgx_Unproject(vec2 ndc, float depth)`, matching
    `unprojectRay()`'s own math exactly, through the drawing camera's own `osg_ProjectionMatrix`/
    `osg_ViewMatrixInverse`.
  - `#pragma osgx::projection DEPTH` → `float osgx_LinearizeDepth(float depth, mat4
    projectionMatrix)` (depth-buffer sample → distance from the camera along the view axis),
    deriving near/far from `projectionMatrix`'s own entries.
  - `#pragma osgx::projection VIEW_POSITION` → `vec3 osgx_ViewPositionFromDepth(vec2 uv, float
    depth, mat4 inverseProjection)` ([0, 1] texture coordinate + raw depth sample → view-space
    position).

  `DEPTH` and `VIEW_POSITION` take the matrix as a parameter because the depth usually comes from
  a different camera than the one drawing the consuming pass; pass them
  `DepthProjectionCallback`'s uniforms.

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

- The `osgx::sdf` catalog's `#pragma osgx::sdf SHAPES` entry expands to all
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

### Baked distance fields: `SAMPLING`, `TEXTURE`, and the `SDF` attribute

The same header also renders BAKED SDF/MSDF textures from any producer (msdfgen, slughorn's
exporter, a hand-authored PNG) - osgx only ever consumes them, never generates them.

- `#pragma osgx::sdf SAMPLING` - pure helpers: `osgx_SDF_Median(vec3)`,
  `osgx_SDF_ScreenPixelRange(vec2 uv, vec2 texSize, float pixelRange)` (Chlumsky's screenPixelRange,
  derivative-driven, so correct under magnification/minification/rotation), and
  `osgx_SDF_CoverageFromDistance(float d, float screenPixelRange)`.
- `#pragma osgx::sdf TEXTURE` - declares the `SDF` attribute's inputs and
  `float osgx_SDF_Coverage(vec2 uv)` (tile-local `uv` in `[0, 1]`). Must be listed after
  `SAMPLING`: `#pragma osgx::sdf SAMPLING,TEXTURE`. Requires `#version 430`+.
- `osgx::SDF::makeTexture(image)` - builds the `Texture2D` a distance field must be sampled from (bilinear, no
  mipmaps, clamped; a single-channel grayscale PNG, which OSG loads as `GL_LUMINANCE`, is relabeled
  `GL_RED`/`GL_R8`). Loading a whole atlas from a glTF manifest: `docs/GLTF.md`, "`osgx_sdf`".
- `osgx::SDF` - a `StateAttribute` (CAPABILITY member 3; `osgx::sdf` and `osgx::sdf.texture` slots) holding
  the texture, `sdfType` (`SDFType::SDF`/`SDFType::MSDF`), `pixelRange` (texels, msdfgen's `-pxrange`), and
  `uvRect` `(u0, v0, u1, v1)` (whole-texture space; `v0 > v1` flips the tile). Same design as
  `Material`/`GridSettings`.

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
counterpart to the text-splicing above. A Program-building call site (e.g.
`osgx::PBRScene::create()`) declares which `Hook` slots it supports as a
`defaults` `HookList`; a caller overrides only the slots it cares about via its own `HookList`,
via one shared enum/mechanism instead of each call site growing its own `osg::Shader*
someHook=nullptr` parameter. `applyHooks()` guarantees exactly one shader ends up attached per
supported slot, always — never zero, never two (GLSL permits one body per function, so an override
substitutes the built-in rather than competing with it).

## `osgx/Skinning.hpp`

Generic mesh vertex attributes and linear blend skinning, independent of any file format.

- `TANGENT_ATTRIBUTE` (7), `JOINT_INDICES_ATTRIBUTE` (8), `JOINT_WEIGHTS_ATTRIBUTE` (9) and their
  GLSL names `osg_Tangent`, `osgx_JointIndices`, `osgx_JointWeights`. `bindMeshAttributes(program)`
  binds the names to the locations.
- The `"osgx::skinning"` catalog: `SKINNING_DECL` (`struct osgx_SkinnedVertex` and the
  `osgx_ApplySkin(position, normal, tangent)` prototype a vertex shader calls) and `JOINT_INPUTS`
  (the joint attributes plus `mat4 osgx_jointMatrices[]`, a storage buffer at the `osgx::joints`
  slot).
- `SKINNING_HOOK_IDENTITY` and `SKINNING_HOOK_LINEAR_BLEND`: complete VERTEX sources defining
  `osgx_ApplySkin()`, for the `Hook::Skinning` slot. Pass them through `resolveShaderLibs()`.

- `Skin(joints, inverseBindMatrices)`: a skeleton's joint palette source. Palette entry `i` is
  `inverseBind[i] * jointWorld[i] * inverse(meshWorld)`, read from the joint `MatrixTransform`s'
  current matrices; whatever moves the joints is independent of the `Skin`. `attach(mesh)` gives
  `mesh` its own palette (a `MatrixfArray` storage buffer bound at the `osgx::joints` slot) and a
  `SkinPaletteCallback` that recomputes it every update traversal. One `Skin` can be attached to
  several meshes.

The glTF loader fills these attributes and builds one `Skin` per glTF skin, attached to each node
that references it.

## `osgx/PBR.hpp`

Reusable BRDF GLSL snippets and the `Material` `StateAttribute`, living flat in `osgx::` (not its
own namespace — the `"osgx::pbr"` catalog tag is a conventional shader-lib
key only, unrelated to the C++ namespace; see [Namespaces](#namespaces) below).

- GLSL snippets — GGX distribution, Schlick Fresnel, Smith geometry — plain function-body
  snippets, concatenated into a consuming fragment shader via `resolveShaderLibs()`, not full
  shaders of their own.
- Per-light BRDF response — `DIRECT_SPECULAR` (Cook-Torrance), `DIRECT_DIFFUSE` (Lambert),
  `DIRECT_LIGHT` (both, against an `osgx_Material`) and `DIRECT_LIGHT_SPHERE` (sphere-light
  specular widening; also needs `Light.hpp`'s `SPHERE_LIGHT_SPECULAR`). They take a light
  direction and radiance, not a `LightSet`, so any light source can drive them.
- `Material` — a `StateAttribute` carrying PBR material factors (base color/roughness/metallic/
  occlusion/emissive factor/alpha mode + cutoff) and up to four texture maps, applied via ONE
  `std140` uniform block - no per-material uniforms, and no sampler uniforms to set.
- Material read side — `MATERIAL_INPUTS` (the `osgx_materialInputs` buffer plus four samplers
  that declare their own units via `layout(binding)`), `GET_MATERIAL` (`osgx_GetMaterial(bcUV,
  ormUV)` → `osgx_Material`), `GET_SHADING_NORMAL` (normal map + derivative-TBN fallback,
  fragment-only), `GET_EMISSIVE`, and `GET_ALPHA`. Any shader reading any `osgx::Material` -
  hand-built or glTF-loaded - uses these; nothing here is glTF-specific.

Direct lights (`LightSet`/`LightType`/`OrbitLightRig`) split out to `osgx/Light.hpp` 2026-09-23 -
see that file's own section below. This file has no C++ dependency on it; a consumer wanting both
includes both, same as always via the `osgx.hpp` umbrella.

## `osgx/Light.hpp`

Typed direct lights (Point/Directional/Spot, plus a Sphere variant via nonzero `sourceRadius` -
not a fourth type) and their Material-free GLSL: radiance per type, `LIGHT_SAMPLE`,
`SPHERE_LIGHT_SPECULAR`, and the `osgx_DirectLighting()` contract (`DIRECT_LIGHTING_*`). Its
snippets are the `"osgx::light"` catalog; the BRDF response to a light is `PBR.hpp`'s. The full
default direct-lighting hook pulls three lines, in this order:

```glsl
#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
#pragma osgx::light POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR
#pragma osgx::pbr DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE
```

A consumer that only calls `osgx_DirectLighting()` needs just `#pragma osgx::pbr MATERIAL_STRUCT`
+ `#pragma osgx::light DIRECT_LIGHTING_DECL`.

Vocabulary note: this codebase calls the group "direct" lights, not glTF's "punctual" - glTF's
`KHR_lights_punctual` is explicitly size-zero, and the Sphere variant (nonzero `sourceRadius`) is
not, so "punctual" would misdescribe it. "Direct" instead answers the question that actually
matters here: computed explicitly per-light, as opposed to baked/prefiltered ambient/IBL.

- `LightSet` — a `StateAttribute` owning a `std140`-uniform-block-backed array of typed direct lights (`LightType::Point`/`Directional`/`Spot`; a sphere light is a `Point`/`Spot` with non-zero `sourceRadius`, not a fourth type) Construct it, then attach it through `StateSet::setAttributeAndModes()` (size `MAX_LIGHTS`, zero-initialized, everything off until `setPoint()`/`setDirectional()`/`setSpot()`, each of which enables its slot). Its `apply()` binds the block's buffer only. Shaders loop the compile-time `OSGX_MAX_LIGHTS` bound and skip slots whose `enabled` flag is 0; `setEnabled()` toggles a configured light without changing its packed data. `LightGizmos` draws every enabled slot. Typed setters/getters (`getType()`, `getPosIntensity()`, `getColor()`, `getSourceRadius()`, `getDirection()`, `getSpotAngles()`, …) replace the old parallel-`osg::Uniform`-array contract.
- `LIGHT_SAMPLE` (`osgx_SampleLight(osgx_Light, worldPos)` → `osgx_LightSample{L, radiance, toLight, sourceRadius}`) — the Material-free per-light evaluation: picks the right `*_LIGHT_RADIANCE` function for the light's type and returns the incoming light, nothing about surface response. The seam between lights and materials: `DIRECT_LIGHTING_HOOK_DEFAULT` (and `Shadow.hpp`'s shadowed variant) consume it for PBR, and a Lambert/toon/NPR shader can consume it directly with no `osgx_Material` in scope - list `LIGHT_UNIFORMS, POINT_LIGHT_RADIANCE, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE` on one `#pragma osgx::light` line.
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

Reusable IBL GLSL snippets (the `"osgx::ibl"` catalog tag, same
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

## `osgx/Environment.hpp`

`Environment` — distant image-based lighting as one `StateAttribute`, the third lighting attribute
beside `Material` (surface) and `LightSet` (direct lights). It owns a prefiltered specular cubemap,
a diffuse irradiance cubemap, and the shared split-sum BRDF LUT, plus orientation, the
roughness-to-mip mapping, and diffuse/specular intensities in one `std140` uniform block (`osgx::environment`
slot). `setAttributeAndModes()` binds all of it (textures at the `osgx::environment.*` slots) and enables
`GL_TEXTURE_CUBE_MAP_SEAMLESS`.

```cpp
auto env = osgx::make_ref<osgx::Environment>(hdrImage);          // GPU bakes (GGX + Lambertian)
auto env = osgx::make_ref<osgx::Environment>(specCube, diffCube); // wrap existing cubemaps

if(env->getBakeRoot()) root->addChild(env->getBakeRoot());       // non-null while bakes are pending
stateSet->setAttributeAndModes(env);
```

- File loading is not part of the class: load first (`osgDB::readImageFile()`,
  `loadPrefilterCubemap()`), then construct.
- `setRotation(osg::Quat)` — world-space rotation; identity is the equirect's own orientation,
  `osg::Quat(-PI/2, Z)` matches the Khronos glTF-Sample-Viewer.
- `setMaxSpecularMip()` — the specular mip holding roughness 1.0. The bake constructor sets the
  bake's last level; the wrap constructor defaults to the texture's last level (Khronos-style KTX2
  prefilters need `levels - 2`).
- GLSL (`#pragma osgx::environment`): `ENVIRONMENT_INPUTS`; `ENVIRONMENT_SAMPLE` — Material-free
  `osgx_EnvironmentSpecular(R, roughness)`, `osgx_EnvironmentIrradiance(N)`,
  `osgx_EnvironmentBRDF(NdotV, roughness)`, `osgx_EnvironmentDirection(dir)`;
  `ENVIRONMENT_LIGHTING` — `osgx_EvaluateEnvironment(osgx_Material, N, V)` (needs
  `MATERIAL_STRUCT`/`F_MULTISCATTER` from `osgx::pbr`). All directions are world-space, Z-up.

## `osgx/Shadow.hpp`

Directional shadow mapping, shared by any `LightSet`-lit scene (nothing here is glTF/PBR-specific;
`PBRSceneOptions`/`PBRLightingPassOptions` take it as an optional input). Lives flat in
`osgx::` — the `"osgx::shadow"` catalog tag is a conventional shader-lib
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
  - `ShadowMap::createSpot(position, direction, outerConeAngle, sceneBoundCenter, sceneBoundRadius,
    options={})` — a spot light's map: a **perspective** depth-only camera at the light's position,
    looking along `direction` (ray travel direction and half-angle in radians, as
    `LightSet::setSpot()`), covering the outer cone. Near/far bracket the scene bound as seen from
    the light. Same camera/texture/uniforms/hook as `create()`; perspective depth is non-linear, so
    a spot map usually wants a smaller `bias` (`osgx-shadow --type spot` uses `0.0005`).
    `repositionSpot(...)` (same arguments) is its `reposition()`.
- `DIRECT_LIGHTING_HOOK_SHADOWED` — a drop-in replacement for `Light.hpp`'s
  `DIRECT_LIGHTING_HOOK_DEFAULT`: identical per-light dispatch loop, except the light at
  `osgx_shadowCasterIndex` has its contribution multiplied by `osgx_ShadowFactor()` (world-space PCF
  3×3 shadow test). Both define `osgx_DirectLighting()` with the same signature, so swapping hooks
  is the only shader change needed — see `examples/osgx-shadow.cpp` for the full A/B wiring
  (press `s` to toggle).

See `examples/osgx-shadow.cpp` (standalone `LightSet` + `ShadowMap` proof, live-draggable light
direction) and `examples/osgx-gbuffer.cpp` (the same shadow map plugged into the deferred pipeline
below).

## `osgx/GBuffer.hpp`

Generic deferred G-buffer camera setup — not PBR-specific. `PBRGBuffer::create()` (see
[`osgx/PBRDeferred.hpp`](#osgxpbrdeferredhpp)) is built on top of this, and a non-PBR deferred
shader can use it directly too. Lives flat in `osgx::`, same
reasoning as `Shadow.hpp` above.

- `AttachmentFormat` — texture internal-format presets for one G-buffer color attachment: `RGBA8`
  (ordinary LDR color/albedo), `RGB16F` (signed `[-1,1]` data, e.g. a view-space normal, needing no
  encode/decode remap), `RGBA16F` (HDR color, e.g. emissive, which can exceed `1.0` before
  tonemapping), `RGBA32F` (real eye-space position, written straight from the vertex shader rather
  than reconstructed from depth — see `PBRGBuffer::positionTexture` below for why
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
  single-channel `GL_R8` result) plugs directly into `PBRLightingPassOptions::aoTexture` or any
  other consumer wanting a generic occlusion mask.
  - `SSAO::create(normalTexture, positionTexture, projectionMatrix, width, height, radius=0.5, bias=0.02)`
    — `projectionMatrix` is a caller-owned uniform this pass reads every draw; keep it refreshed
    from the same per-frame callback that updates `PBRLightingPass`'s own view-matrix uniforms
    (see `PBRLightingPass::update()` below for why it must be a `PRE_RENDER` `preDrawCallback`).
    `radius`/`bias` are scale-dependent (a sane radius is a small fraction of the scene's own
    bounding radius, not a fixed constant) — compute them from the scene being rendered.

See `examples/osgx-gbuffer.cpp` for the full deferred pipeline (`PBRGBuffer` +
`PBRLightingPass`, both built on this), live SSAO wired into `PBRLightingPassOptions::aoTexture`
with live ImGui radius/bias sliders, and a channel-by-channel G-buffer visualizer (press `0`-`6`,
`6` being SSAO's own output). Python: `osgx.GBuffer`/`osgx.SSAO`.

## `osgx/PBRScene.hpp`

Forward PBR for `osgx::Material` geometry: one Program (`PBR_VERTEX_SHADER` plus a fragment shader)
shading each material by whichever light sources are present.

- `PBRScene::create(node, options={})` attaches the Program (`ON|OVERRIDE`) to `node`'s StateSet.
  `PBRSceneOptions`, every field optional:
  - `environment` — an `osgx::Environment` (image-based light), attached to the node. Without one
    the environment term is zero and surfaces are lit by the `osgx::LightSet` direct lights
    inherited from the scene graph and their own emissive.
  - `shadowMap` — shadows the key light (`DIRECT_LIGHTING_HOOK_SHADOWED`; depth texture at the
    `osgx::shadowMap` slot).
  - `hooks` — substitutes `Hook::Skinning` (`osgx_ApplySkin()`, e.g. `SKINNING_HOOK_LINEAR_BLEND`)
    and `Hook::Tonemap` (`osgx_Tonemap()`).
  - `diagnostics` — adds the `debugMode`/`disableNormalMap`/`disableRoughnessMap`/
    `disableSpecularAA` uniforms returned in `PBRScene`.
- Imported StateSet defines: `OSGX_PBR_ENVIRONMENT` (set when an environment is given),
  `OSGX_PBR_DIAGNOSTICS`.

The Khronos glTF-Sample-Viewer setup is `osgx::gltf::loadEnvironment()` plus this (see
[GLTF.md](GLTF.md#rendering-gltf-content)). Python: `osgx.PBRScene.create(node,
osgx.PBRSceneOptions(environment=env, ...))`.

## `osgx/PBRDeferred.hpp`

Deferred PBR for `osgx::Material` geometry: a geometry pass writing material-only G-buffer
textures, then a fullscreen lighting pass running the same `osgx_EvaluateEnvironment()`/
`osgx_DirectLighting()` logic as a forward PBR shader, against those textures.

- `PBRGBuffer::create(node, width, height)` — `node` becomes the child of a `PRE_RENDER` geometry
  pass (built on `GBuffer`) using `PBR_VERTEX_SHADER` (`PBR.hpp`). Textures: `albedoTexture`
  (rgb albedo, a ao), `normalTexture` (view-space normal, RGB16F), `materialTexture` (r roughness,
  g metallic), `emissiveTexture` (rgb HDR emissive, a alpha coverage), `positionTexture`
  (view-space position, RGBA32F), `depthTexture`. Position is written straight from the vertex
  shader, not reconstructed from depth: each nested `PRE_RENDER` camera clamps its own private
  copy of the projection during cull and never writes it back, so a projection read off the main
  camera does not reliably match the one the geometry pass used.
- `PBRLightingPass::create(gbuffer, mainCamera, options={})` — a `POST_RENDER` fullscreen-quad
  pass. `PBRLightingPassOptions`, every field optional:
  - `environment` — same contract as `PBRSceneOptions::environment`; without one the built-in
    shader's environment term is zero.
  - `tonemap` — `false` leaves the output linear HDR (no curve, no gamma) for further passes.
  - `hooks` — substitutes `Hook::Tonemap` (the `osgx_Tonemap()` definition),
    `Hook::DirectLighting` (the `osgx_DirectLighting()` definition) or `Hook::DeferredLighting`
    (the whole fragment `main()`).
  - `shadowMap` — shadows the key light (`DIRECT_LIGHTING_HOOK_SHADOWED`).
  - `aoTexture` — multiplied into the ambient term (e.g. `SSAO`'s output).
  - `diagnostics`.
- **Call `PBRLightingPass::update(mainCamera)` from a `preDrawCallback` on the first `PRE_RENDER`
  camera in the scene graph** (by render order) — not from `mainCamera`'s own `preDrawCallback`,
  and not after `viewer.frame()` returns. Every `PRE_RENDER` camera draws before `mainCamera`'s
  `preDrawCallback` fires, so either alternative hands the pass a stale view matrix — visible as
  artifacts that worsen while the camera moves.
- The `osgx::gbuffer` catalog, for a `Hook::DeferredLighting` shader: `DEFERRED_LIGHTING_INPUTS`
  (`vUV`, the samplers `gAlbedo`/`gNormal`/`gMaterial`/`gEmissive`/`gPosition`,
  `osgx_mainViewMatrix`/`osgx_mainViewMatrixInverse`, `out vec4 fragColor`) and `GET_GBUFFER`
  (`struct osgx_GBuffer` and `osgx_GetGBuffer(uv)`). Pass the shader source through
  `resolveShaderLibs()`. The G-buffer textures bind at the `osgx::gbuffer.*` texture-unit slots.
- StateSet defines a custom lighting shader can import (`#pragma import_defines`): `OSGX_PBR_AO`
  (`aoTexture` set, sampled as `aoTex`), `OSGX_PBR_NO_TONEMAP`, `OSGX_PBR_DIAGNOSTICS`,
  `OSGX_PBR_ENVIRONMENT`.

See `examples/osgx-gbuffer.cpp` for the full wiring (shadow camera, SSAO, and a per-channel
G-buffer visualizer) and `examples/osgx-gbuffer-comic.cpp`/`-blueprint.cpp`/`-edgewear.cpp` for
`Hook::DeferredLighting` shaders. Python: `osgx.PBRGBuffer`, `osgx.PBRLightingPass`,
`osgx.PBRLightingPassOptions`.

## Namespaces

`osgx::pbr`, `osgx::ibl`, `osgx::shadow`, and `osgx::gbuffer` used to exist as separate C++
namespaces; as of 2026-08-20 every symbol they held lives directly under `osgx::`. The rule: a
namespace exists only for a genuinely separate opt-in subsystem — its own `#include` outside the
`osgx.hpp` umbrella AND its own CMake link target (exactly `debug`/`imgui`/`platform`/`gltf`/
`ktx2`). Everything that compiles unconditionally into `libosgx`
stays flat, no matter how "topic-shaped" it feels — this is also why `picking`/`grid`/
`manipulators`/`shadow`/`gbuffer` never got their own namespace. The `"osgx::pbr"`/`"osgx::ibl"`/
`"osgx::shadow"` catalog names are shader-lib registry tags only — conventional, `#pragma`-
addressable names, unrelated to the C++ namespace.

## `osgx/Version.hpp`

`OSGX_VERSION_MAJOR`/`MINOR`/`PATCH` and the `OSGX_VERSION` string, generated from the CMake
project version. Included by `Core.hpp`, so it's available transitively almost everywhere.
