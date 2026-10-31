# `osgx::debug` - GL_KHR_debug, profiler, and GPU load simulation

`osgx/Debug.hpp` provides four independent systems that can be used together or separately. It is
not included by `osgx.hpp`; opt in explicitly with `#include "osgx/Debug.hpp"`, which includes
every `osgx::debug` header. Each part also has its own header under `osgx/debug/`:

| Header | Contents |
|---|---|
| `osgx/debug/Annotation.hpp` | `GL_KHR_debug`: `initialize()`, driver message callback, debug groups and messages, `AnnotationGroup`/`AnnotationScope`, camera draw callback slots |
| `osgx/debug/Profiler.hpp` | `Profiler`, its callbacks and profiling visitors |
| `osgx/debug/Trace.hpp` | per-thread trace buffers |
| `osgx/debug/TraceProfile.hpp` | the rolling per-phase profile built from them |
| `osgx/debug/Viewer.hpp` | `ProfilingViewer`, `FrameByFrameViewer` |
| `osgx/debug/ShaderClock.hpp` | `ShaderClockDrawable` |

For a Dear ImGui front-end, see [`osgx::imgui`](IMGUI.md) — it is a sibling of `osgx::debug`, not
nested under it, since most of it (sliders, `Panel`, texture/stats browsers) has nothing to do with
`GL_KHR_debug` specifically; only its `ProfilerSection` reaches into `osgx::debug`.

---

## System 1 — GL Driver Message Callback

`glDebugMessageCallback` is the GL driver speaking *to you*. When the driver detects
an error, a performance warning, or a shader issue, it calls your registered function.
This is a completely separate pipe from the other two systems — no queries, no groups,
just the driver reporting what it observed.

- `osgx::debug::initialize(gc)` — resolves all `GL_KHR_debug` extension pointers for a context.
- `osgx::debug::installDefaultCallback()` — registers a callback that writes driver messages to the log sink (`setSink()`, default `osg::notify`) and records each one as an INSTANT (`"driver message"`) in the calling thread's trace buffer, so it shows up in the profile where it happened (frame-accurate with synchronous output, the default). The GL's echoes of the application's own groups and inserted messages are skipped.
- `osgx::debug::setCallback(fn)` / `clearCallback()` — install or remove a custom callback.
- `osgx::debug::enableDebugOutput()` / `disableDebugOutput()` — toggle `GL_DEBUG_OUTPUT[_SYNCHRONOUS]`.
- `osgx::debug::GraphicsOperation` — a `setRealizeOperation()` helper that calls `initialize()` once the context exists.

---

## System 2 — KHR Debug Group Annotations

`glPushDebugGroup` / `glPopDebugGroup` are *you* speaking to the GL driver — and by
extension to tools like apitrace and RenderDoc that intercept those calls. Annotations
appear as named, nested regions in a trace, making it immediately clear which draw calls
belong to which logical pass.

**Critical rule:** all calls require a current GL context. Never call `pushGroup` or
`messageInsert` from application-side C++ code (e.g. from `main()` or a viewer loop);
always use the camera callback objects, which fire while the context is guaranteed current.

Every `pushGroup()`/`popGroup()` pair is also recorded as a DRAW-phase span (`"annotation"`) and every
`messageInsert()` as an INSTANT (`"message"`) in the calling thread's trace buffer, named by the
message (cut to 55 bytes), so they appear in the profile's DRAW tree. Conversely, once
`initialize()` has run, the `Profiler`'s own DRAW spans (cameras, drawables, readback, profile fold,
swap) are KHR debug groups too, so apitrace/RenderDoc show the same structure as the profile.
Push and pop on one thread, nested inside the spans around them: within one draw callback, or
`PRE_DRAW` to `POST_DRAW` of one camera (the profiler's camera span runs `PRE_DRAW` to `FINAL_DRAW`).

**Annotations are camera-scoped.** A `PRE_DRAW` / `POST_DRAW` callback pair brackets
everything that camera renders — not individual nodes or groups within the scene graph.
Sub-groups of a single camera's scene cannot be individually annotated this way; for
per-group bracketing, each group needs its own RTT camera. For per-drawable bracketing,
use `ProfilerCallback` instead.

- `osgx::debug::AnnotationScope` — **use this by default.** RAII push/pop; construct on the stack inside any `drawImplementation` or draw callback. Begin and end are in the same scope.
- `osgx::debug::AnnotationGroup` — use only when begin and end must live in *separate* callback objects. Its sole purpose is to share push/pop state across `AnnotationBeginCallback` (`PRE_DRAW`) and `AnnotationEndCallback` (`POST_DRAW`) so the group remains matched across the camera's frame boundary. Not a general-purpose annotation type.
- `osgx::debug::AnnotationBeginCallback` / `AnnotationEndCallback` — install an `AnnotationGroup` on a camera's `PRE_DRAW` or `POST_DRAW` slot so it fires at the correct time.
- `osgx::debug::appendCameraDrawCallback()` / `prependCameraDrawCallback()` — safely add callbacks to an existing slot without clobbering others.

Groups nest naturally. `FrameByFrameViewer` installs a single `Frame` group on its
camera automatically. Any other object with its own RTT camera (e.g. an `osgSlug::Atlas`)
can install its own `AnnotationBeginCallback` / `AnnotationEndCallback` and it will
appear nested inside the viewer's group in the trace:

```
Camera@0x...
  Frame
    Atlas: glyph-cache
      [draw calls]
    Atlas: labels
      [draw calls]
readback
profile
```

---

## System 3 — Profiler

`osgx::debug::Profiler` measures every OSG phase of a frame: the event and update traversals
(per node visited), the cull traversal (per node, plus which nodes were culled), and drawing (per
drawable CPU time and GPU time from `GL_TIMESTAMP` queries, per camera, the query readback, buffer
swaps). Everything is recorded into per-thread trace buffers (`osgx/debug/Trace.hpp`) and folded,
one frame behind, into a rolling profile: one tree per phase in traversal order, with moving
averages of inclusive and self time per node (`osgx/debug/TraceProfile.hpp`).

```cpp
auto profiler = osgx::make_ref<osgx::debug::Profiler>();   // QueryMode::SYNC by default

viewer.setSceneData(root);
profiler->attach(viewer);   // before realize()

// ... frames ...

profiler->report(std::cout);          // the four trees as text
auto trees = profiler->getProfile();  // the same as data (TraceNode)
```

`attach()` installs a `ProfilerCallback` on every drawable (`ProfilerVisitor`), a
`ProfilerFinalCallback` on each view's master camera, profiling cull/update/event visitors, and
camera and swap spans. `refresh()` re-scans the scene for new drawables and cameras.
`osgx::debug::ProfilingViewer` is an `osgViewer::Viewer` that attaches its own profiler and adds
spans for its event and update traversals (handlers, manipulator, pager merges).

GPU timing is two-phase so the GPU pipeline is never stalled mid-submission:

- `ProfilerCallback` (per drawable) issues `glQueryCounter(begin)`, draws, `glQueryCounter(end)`;
  never blocks.
- `ProfilerFinalCallback` (per camera, `FINAL_DRAW`) reads the results back into the trace.
  `QueryMode::SYNC` reads this frame's results (blocks until the GPU finishes the frame; suitable
  for step-by-step viewers); `QueryMode::ASYNC` reads the previous frame's (no stall unless the
  GPU is the bottleneck; use for continuous rendering). The readback is a span of its own, so its
  cost is visible in the profile.

`osgx::imgui`'s profiler section (`addProfilerSection(view)`) shows the profile as a table.

---

## System 4 - Shader-Clock GPU Load Simulation

`ShaderClockDrawable` creates deliberate, calibrated GPU work at the point where it occurs in
scene traversal. It uses `GL_ARB_shader_clock` in a private 1x1 render target, so it does not
change CPU pacing or visible framebuffer contents. Add it directly to a scene graph, set a
duration in milliseconds, and it will stall the GPU for approximately that duration each draw.

- `ClockCalibration::DYNAMIC` (default) measures the context's shader-clock rate with GPU
  timestamp queries on first draw. This calibration blocks the CPU once; ordinary stall draws do
  not read data back. `recalibrate()`, `setRecalibrationInterval()`, or
  `setRecalibrationFrameInterval()` refreshes the rate. The frame interval is useful when the
  workload is rendered intermittently or must be sampled on a deterministic render cadence.
- `ClockCalibration::STATIC` uses a rate supplied with `setTicksPerMillisecond()`, avoiding
  timestamp queries and CPU readback entirely.
- `isSupported(contextID)` reports whether the first draw under that context found
  `GL_ARB_shader_clock`. Unsupported contexts log once and subsequently perform no work.

Profile the scene (System 3) to measure the resulting GPU cost. Shader-clock
ticks are implementation-defined, so dynamic calibration is the appropriate default outside a
controlled test rig.

---

## Data flow summary

```
GL driver ──glDebugMessageCallback──► your callback (errors, warnings)

you ──AnnotationBegin/EndCallback──► GL driver ──► apitrace / RenderDoc

visitors + ProfilerCallback ──► TraceBuffer ──► Profiler (rolling profile) ──► report() / osgx::imgui

ShaderClockDrawable ──► private 1x1 FBO ──► calibrated GPU delay

FrameByFrameViewer::requestRender() ◄── 'n' key (or an osgx::imgui button)
```

The four systems are independent. Annotations never need to know the profiler exists, and the
driver callback has nothing to do with either. ShaderClockDrawable can be profiled, but does not
depend on the profiler.

---

## `FrameByFrameViewer`

A `ProfilingViewer` for apitrace-friendly debugging. Press **`n`** to render one frame; between
keystrokes it only processes events and idles, so a capture session does not flood with
unintended frames. On first render it installs a `Frame` annotation group on the camera
(System 2); its profiler (System 3) is attached automatically, and `setReportStream()` writes the
profile after each rendered frame (showing the frame before, since frames are folded one behind).
Combine with `installDefaultCallback()` (System 1) for a fully instrumented scene.

---

## Extensions

- [GL_KHR_debug](https://registry.khronos.org/OpenGL/extensions/KHR/KHR_debug.txt)
- GL_ARB_debug_output
- GL_EXT_debug_marker
- GL_EXT_debug_label
- GL_AMD_debug_output
- GL_ARB_shader_clock
