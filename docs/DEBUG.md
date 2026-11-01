# `osgx::debug` - GL_KHR_debug, profiler, and GPU load simulation

`osgx/Debug.hpp` provides four independent systems that can be used together or separately. It is
not included by `osgx.hpp`; opt in explicitly with `#include "osgx/Debug.hpp"`, which includes
every `osgx::debug` header. Each part also has its own header under `osgx/debug/`:

| Header | Contents |
|---|---|
| `osgx/debug/Annotation.hpp` | `GL_KHR_debug`: `initialize()`, driver message callback, debug groups and messages, `AnnotationGroup`/`AnnotationScope`, camera draw callback slots |
| `osgx/debug/Profiler.hpp` | `Profiler`, its callbacks and profiling visitors |
| `osgx/debug/Trace.hpp` | per-thread trace buffers, `TraceScope`/`OSGX_TRACE_SCOPE()`, `TraceState` |
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

- `osgx::debug::AnnotationScope` — **use this by default.** RAII push/pop; construct on the stack inside any `drawImplementation` or draw callback. Begin and end are in the same scope. Python: `with osgx.debug.AnnotationScope(id, message):` pushes on entering the block and pops on leaving it (exception or not).
- `osgx::debug::AnnotationGroup` — use only when begin and end must live in *separate* callback objects. Its sole purpose is to share push/pop state across `AnnotationBeginCallback` (`PRE_DRAW`) and `AnnotationEndCallback` (`POST_DRAW`) so the group remains matched across the camera's frame boundary. Not a general-purpose annotation type. Python: `osgx.debug.AnnotationGroup(id, message)` with `begin()`/`end()` (both safe to call twice).
- A trace scope (`OSGX_TRACE_SCOPE()`, System 3) that opens inside drawing is a debug group too, named by the scope.
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

### Instrumenting your own code

The profile shows OSG's own structure (traversals, nodes, drawables, cameras). To time code of
your own (a callback, a loader step, a function the profile points at), put a trace scope around
it:

```cpp
void Terrain::sortTiles() {
	OSGX_TRACE_SCOPE("sort tiles");

	...
}
```

The scope becomes a node named "sort tiles" under whatever span is open on its thread (an update
callback's node, a camera's drawing, an annotation), in `report()`, the waterfall and the ImGui
view. Recording is lock-free and only writes to the calling thread's buffer, so scopes can be
scattered freely while investigating.

- Frame and phase are taken from the thread (`TraceBuffer::state()`, a `TraceState`): the frame of
  its latest record, and the phase of its innermost open span, else of its latest record. A
  scope on a thread that has recorded nothing yet (no phase) is not recorded.
- Inside an open DRAW span the thread is drawing with its context current, so such a scope is
  also a KHR debug group (System 2) and shows up in apitrace/RenderDoc/Nsight nested as in the
  profile. Elsewhere it is CPU-only: no GL call is made where no context may be current.
- For a fixed phase, an object or an explicit frame, declare the `Location` and the
  `TraceScope` yourself:

  ```cpp
  static constexpr osgx::debug::Location SORT("sort tiles", osgx::debug::Phase::UPDATE);

  osgx::debug::TraceScope scope(SORT, tile, frameNumber);
  ```

  Scopes with an explicit phase are CPU-only.
- Scopes measure CPU time only; threads outside the viewer's traversals (worker pools) have no
  frame of their own.

### Reading the profile: the frame waterfall

`reportWaterfall()` (text) and the `osgx::imgui` profiler section (`addProfilerSection(view)`)
show the newest frame as a waterfall: one row per node, in traversal/submission order, down to a
depth, each with its CPU and GPU ms and a bar where it ran within the frame, on an axis of the
frame budget (60 Hz unless given) or the frame, if longer. Waits (the buffer swap, the SYNC
query readback) are grey/`.`; rows under a threshold are hidden.

```
osgx::debug::Profiler: frame 199: CPU 6.32 ms, GPU 7.18 ms (budget 16.67 ms); ...
  #010  DRAW    main                    1.262    3.104  |                    ############
  #011  DRAW      draw_box              1.017    0.001  |                    ----
  #012  DRAW        draw_box: wait ...  1.012           |                    ----
  #013  DRAW          wait              1.002           |                    ----
  #014  DRAW      gpu_small             0.054    1.035  |                        ####
```

**Budget:** the header compares the frame's *busy* times with the budget (`frameLoad()`): the CPU
time of the busiest thread (self times, waits excluded) and the GPU time of the draws. Where the
spans end is not a measure of load: with vsync a frame always ends near the refresh period, the
waiting hidden in the swap, a SYNC readback, or the GPU waiting for a free back buffer.

**GPU time** overlays each row (`WaterfallGpu`; the ImGui "GPU:" choice):

- `COST` (default): drawn from the row's CPU start, its length the GPU time - how much GPU work
  the row adds, next to its CPU time. Over-budget parts are marked.
- `TIMELINE`: from the row's first draw's GPU start to its last draw's GPU end - when the GPU ran
  it. The GPU trails the CPU (by a frame with ASYNC queries), so this shows latency, and is never
  marked over budget. Positions are mapped from the GPU clock by a sample, so approximate.
- `OFF`: CPU bars only (GPU ms still listed).

The ImGui view adds collapsible rows, tooltips (class, self time, draws, when the GPU ran the row)
and controls for the query mode, budget (30-144 Hz) and threshold. The data behind both is
`getProfile()` (`TraceNode` trees), `frameLoad()` and `cpuSpan()`/`gpuSpan()`.

### Python

`osgx.debug` binds the same API: `Profiler(mode)` with `attach(viewer)`, `refresh()`, `mode`,
`profile` (four `TraceNode` trees), `profileFrame`, `report()` and `reportWaterfall(budgetMs,
width, maxDepth, minMs, gpu)` returning strings; `frameLoad()`, `cpuSpan()`, `gpuSpan()`;
`TraceScope(name, phase=None)` as a context manager and `threadTraceState()`. Keep a reference to
the `Profiler`: the viewer's callbacks only observe it.

```python
profiler = osgx.debug.Profiler()
profiler.attach(viewer)

class Update(osg.NodeCallback):
	def __call__(self, node, nv):
		with osgx.debug.TraceScope("python update"):
			...

		super().__call__(node, nv)

# ... frames ...

print(profiler.reportWaterfall())
```

Python scopes share one trace `Location` per name and phase (kept for the process's lifetime).

**Headless contexts:** an `osgx::headless` context is realized when created, so `viewer.frame()`
never realizes the viewer and a realize operation (`osgx::debug::GraphicsOperation`, which binds
the KHR functions) never runs. Call `viewer.realize()` explicitly.

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

visitors + ProfilerCallback + TraceScope ──► TraceBuffer ──► Profiler (rolling profile) ──► report() / reportWaterfall() / osgx::imgui

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
