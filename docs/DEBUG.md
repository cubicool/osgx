# `osgx::debug` - GL_KHR_debug, profiler, and GPU load simulation

`osgx/Debug.hpp` provides four independent systems that can be used together or separately. It is
not included by `osgx.hpp`; opt in explicitly with `#include "osgx/Debug.hpp"`.

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
- `osgx::debug::installDefaultCallback()` — registers a callback that writes driver messages to `osg::notify`.
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

**Annotations are camera-scoped.** A `PRE_DRAW` / `FINAL_DRAW` callback pair brackets
everything that camera renders — not individual nodes or groups within the scene graph.
Sub-groups of a single camera's scene cannot be individually annotated this way; for
per-group bracketing, each group needs its own RTT camera. For per-drawable bracketing,
use `ProfilerCallback` instead.

- `osgx::debug::Scoped` — **use this by default.** RAII push/pop; construct on the stack inside any `drawImplementation` or draw callback. Begin and end are in the same scope.
- `osgx::debug::AnnotationGroup` — use only when begin and end must live in *separate* callback objects. Its sole purpose is to share push/pop state across `AnnotationBeginCallback` (`PRE_DRAW`) and `AnnotationEndCallback` (`FINAL_DRAW`) so the group remains matched across the camera's frame boundary. Not a general-purpose annotation type.
- `osgx::debug::AnnotationBeginCallback` / `AnnotationEndCallback` — install an `AnnotationGroup` on a camera's `PRE_DRAW` or `FINAL_DRAW` slot so it fires at the correct time.
- `osgx::debug::appendCameraDrawCallback()` / `prependCameraDrawCallback()` — safely add callbacks to an existing slot without clobbering others.

Groups nest naturally. `FrameByFrameViewer` installs a single `Frame` group on its
camera automatically. Any other object with its own RTT camera (e.g. an `osgSlug::Atlas`)
can install its own `AnnotationBeginCallback` / `AnnotationEndCallback` and it will
appear nested inside the viewer's group in the trace:

```
Frame
  Atlas: glyph-cache
    [draw calls]
  Atlas: labels
    [draw calls]
/Frame
```

---

## System 3 — GPU Timestamp Profiler

`glQueryCounter` wraps each drawable in a pair of GPU timestamp queries so you can
measure actual GPU cost per draw call, averaged over a ring buffer of recent frames.
This is a two-phase design to avoid stalling the GPU pipeline mid-submission.

**Phase 1 — submit** (`ProfilerCallback`, per drawable):
```
glQueryCounter(begin) → draw → glQueryCounter(end) → push to FrameAccumulator
```
Never blocks. Install with `ProfilerVisitor` or attach `ProfilerCallback` directly.

**Phase 2 — drain** (`ProfilerFinalCallback`, per camera):
```
after all draws: read GL_QUERY_RESULT → update ring buffers → print/report
```
Blocks only on the last outstanding query. Install on the camera's `FINAL_DRAW` slot.

- `osgx::debug::ProfilerCallback<N>` — drawable wrapper; issues timestamp queries and measures CPU time.
- `osgx::debug::ProfilerVisitor<N>` — walks the scene graph and installs `ProfilerCallback` on every drawable.
- `osgx::debug::ProfilerFinalCallback<N>` — camera callback that drains `FrameAccumulator` results.
- `QueryMode::SYNC` (default) — blocks per-query; suitable for step-by-step viewers.
- `QueryMode::ASYNC` — drains the previous frame's results with zero meaningful stall; use for continuous renderers.

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

Wrap the drawable with `ProfilerCallback` when measuring the resulting GPU cost. Shader-clock
ticks are implementation-defined, so dynamic calibration is the appropriate default outside a
controlled test rig.

---

## Data flow summary

```
GL driver ──glDebugMessageCallback──► your callback (errors, warnings)

you ──AnnotationBegin/EndCallback──► GL driver ──► apitrace / RenderDoc

ProfilerCallback ──► FrameAccumulator ──► ProfilerFinalCallback ──► osg::notify (or osgx::imgui::Widget)

ShaderClockDrawable ──► private 1x1 FBO ──► calibrated GPU delay

FrameByFrameViewer::requestRender() ◄── 'n' key (or an osgx::imgui button)
```

The four systems are independent. Annotations never need to know the profiler exists, and the
driver callback has nothing to do with either. ShaderClockDrawable can be profiled, but does not
depend on the profiler.

---

## `FrameByFrameViewer`

A single, consolidated `osgViewer::Viewer` subclass for apitrace-friendly debugging.
Press **`n`** to advance one frame; the viewer idles between keystrokes so a capture
session does not flood with unintended frames. On first render it automatically installs
`Frame` / `Render` annotation groups on the camera (System 2). Combine with
`ProfilerVisitor` + `ProfilerFinalCallback` (System 3) and `installDefaultCallback()`
(System 1) for a fully instrumented scene.

---

## Extensions

- [GL_KHR_debug](https://registry.khronos.org/OpenGL/extensions/KHR/KHR_debug.txt)
- GL_ARB_debug_output
- GL_EXT_debug_marker
- GL_EXT_debug_label
- GL_AMD_debug_output
- GL_ARB_shader_clock
