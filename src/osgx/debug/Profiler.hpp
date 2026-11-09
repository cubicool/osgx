#pragma once

#include "osgx/Core.hpp"
#include "osgx/debug/Annotation.hpp"
#include "osgx/debug/Trace.hpp"
#include "osgx/debug/TraceProfile.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Drawable>
#include <osg/Geode>
#include <osg/NodeVisitor>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/buffered_value>
#include <osg/observer_ptr>
#include <osgViewer/View>
#include <osgGA/EventVisitor>
#include <osgUtil/CullVisitor>
#include <osgUtil/UpdateVisitor>
#include <osgViewer/ViewerBase>

OSGX_ENABLE_WARNINGS

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace osgx::debug {

namespace detail {
	class ProfilerState: public osg::Referenced {
	public:
		std::atomic<bool> enabled{true};
	};

	// Per-context GPU timestamp queries for two-phase timing. ProfilerCallback (phase 1) pushes
	// pending query pairs here without blocking; ProfilerFinalCallback (phase 2) reads their
	// results after the camera has rendered and writes them into the drawables' trace records.
	//
	// Keyed by contextID via osg::buffered_object so each GL context has its own
	// isolated accumulator; GL query objects are context-local. Used only from the context's
	// draw thread.
	struct FrameAccumulator {
		struct Entry {
			GLuint begin = 0;
			GLuint end = 0;

			// The drawable's ENTER record in the drawing thread's TraceBuffer.
			TraceBuffer* buffer = nullptr;
			std::uint64_t enter = 0;
		};

		std::vector<Entry> pending;
		std::vector<Entry> ready;
		std::vector<Entry> freeList;

		Entry alloc(osg::GLExtensions* ext);
		void push(Entry e);

		// SYNC: read the current frame's pending queries, blocking per-query.
		void drain(osg::GLExtensions* ext);

		// ASYNC: read the previous frame's results (should already be ready), then advance the
		// double buffer so this frame's pending becomes next frame's ready. One-frame lag.
		void swap_and_drain(osg::GLExtensions* ext);

	private:
		void _drain(std::vector<Entry>& source, osg::GLExtensions* ext);
	};

	// Defined once in Profiler.cpp - see the comment above _pushGroup et al. (Annotation.hpp) for
	// why this must be `extern`, not `inline`.
	extern osg::buffered_object<FrameAccumulator> _accumulators;
}

// GPU/glGenQueries Profiling
//
// Two-phase design: ProfilerCallback (phase 1) wraps each drawable, records it in the trace and
// issues glQueryCounter begin/end without blocking. ProfilerFinalCallback (phase 2) reads the
// query results after the camera finishes rendering and writes them into the trace records.
// Profiler::attach() installs both (and the trace visitors) on a viewer; by hand:
//
// root->accept(osgx::debug::ProfilerVisitor());
// appendCameraDrawCallback(cam, FINAL_DRAW, new osgx::debug::ProfilerFinalCallback());
enum class QueryMode { SYNC, ASYNC };

namespace detail {
	// The Location of ProfilerCullVisitor's INSTANT for a node rejected by its bounds.
	const Location& culledLocation();

	// The Location of ProfilerCallback's spans: the DRAW spans that are draws.
	const Location& drawableLocation();

	// Whether spans at `location` are the profiler's per-node spans (a node visited by a
	// profiling visitor, a culled node, a drawable's draw, a camera's drawing), which are about
	// their object rather than a named operation.
	bool isNodeLocation(const Location* location);

	// Whether spans at `location` are the profiler's own per-frame work rather than the
	// application's: the query readback and the profile fold. The profile groups them under one
	// node per camera at profilerGroupLocation() ("osgx profiler").
	bool isProfilerLocation(const Location* location);
	const Location& profilerGroupLocation();

	// Whether spans at `location` are waits rather than work: the buffer swap (with vsync mostly
	// blocked until the display takes a frame) and the query readback (blocked until the GPU
	// finishes, under SYNC, and sometimes where the driver blocks for vsync instead of in the
	// swap).
	bool isWaitLocation(const Location* location);

	// The Location of ProfilerFinalCallback's once-per-frame GPU clock sample: an INSTANT whose
	// object is the GraphicsContext, tick the CPU time and gpuBegin (= gpuEnd) the GL_TIMESTAMP
	// read at the same moment, which maps that draw thread's GPU timestamps onto CPU ticks.
	const Location& gpuClockLocation();

	// What to call a span: its object's name; else, for per-node spans (or no Location),
	// objectLabel(); else the Location's name.
	std::string spanLabel(
		const Location* location,
		std::string_view name,
		const char* className,
		const void* object
	);
}

// Phase 1: records the drawable's draw (ENTER/LEAVE) in the drawing thread's TraceBuffer and
// brackets it with GL_TIMESTAMP queries pushed to the context's FrameAccumulator, inside a KHR_debug
// group named by objectLabel() (after initialize()). Draws through `cb` if not null. Never blocks;
// safe in any threading model.
class ProfilerCallback: public osg::Drawable::DrawCallback {
public:
	explicit ProfilerCallback(
		osg::Drawable::DrawCallback* cb=nullptr,
		detail::ProfilerState* state=nullptr
	);

	void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const override;

protected:
	~ProfilerCallback() override;

private:
	osg::ref_ptr<osg::Drawable::DrawCallback> _cb;
	osg::ref_ptr<detail::ProfilerState> _state;
};

// Phase 2: install on a camera via FINAL_DRAW slot to drain the FrameAccumulator, writing GPU
// times into the drawables' trace records.
//
// QueryMode::SYNC (default): blocks per-query on GL_QUERY_RESULT for the current
// frame. Fine for step-by-step viewers; may stall continuous rendering.
//
// QueryMode::ASYNC: drains the previous frame's results (already retired by the
// GPU) then swaps buffers. Zero meaningful stall; one-frame lag.
class ProfilerFinalCallback: public osg::Camera::DrawCallback {
public:
	explicit ProfilerFinalCallback(QueryMode mode=QueryMode::SYNC, detail::ProfilerState* state=nullptr);

	// TODO: Switching ASYNC->SYNC leaves one set of retired query objects in
	// FrameAccumulator::ready that SYNC will never drain. Harmless for a debug
	// tool (one-time minor GL object leak), but a future cleanup pass should
	// flush ready on mode change. SYNC->ASYNC is clean.
	void setMode(QueryMode mode);
	QueryMode getMode() const;

	void operator()(osg::RenderInfo& ri) const override;

protected:
	~ProfilerFinalCallback() override;

private:
	QueryMode _mode;
	osg::ref_ptr<detail::ProfilerState> _state;
};

// Walks the scene graph and installs a ProfilerCallback on every Drawable that has none, wrapping
// its draw callback. Install ProfilerFinalCallback on the camera separately to complete setup.
class ProfilerVisitor: public osg::NodeVisitor {
public:
	explicit ProfilerVisitor(detail::ProfilerState* state=nullptr);

	~ProfilerVisitor() override;

	void apply(osg::Geode& geode) override;
	void apply(osg::Drawable& drawable) override;

private:
	osg::ref_ptr<detail::ProfilerState> _state;
};

// An osgUtil::CullVisitor that records every node and drawable it visits as a CULL TraceScope
// (object = the node, camera = the camera being culled). A node rejected by its bounding sphere is
// recorded as one INSTANT at the "culled" Location instead, for the node types whose cull starts
// with that test (Node, Geode, Billboard, Group, Transform, LOD, OcclusionQueryNode); a drawable
// rejected by its bounding box gets the same INSTANT after its scope. Rejections made inside
// other apply() overloads or by cull callbacks are not recorded as such.
class ProfilerCullVisitor: public osgUtil::CullVisitor {
public:
	ProfilerCullVisitor();

	// Copies `cv`'s cull settings, as CullVisitor::clone() does.
	explicit ProfilerCullVisitor(const osgUtil::CullVisitor& cv, detail::ProfilerState* state=nullptr);

	META_NodeVisitor(osgx::debug, ProfilerCullVisitor)

	osgUtil::CullVisitor* clone() const override;

	using osgUtil::CullVisitor::apply;

	void apply(osg::Node& node) override;
	void apply(osg::Geode& node) override;
	void apply(osg::Drawable& drawable) override;
	void apply(osg::Billboard& node) override;
	void apply(osg::LightSource& node) override;
	void apply(osg::ClipNode& node) override;
	void apply(osg::TexGenNode& node) override;
	void apply(osg::Group& node) override;
	void apply(osg::Transform& node) override;
	void apply(osg::Projection& node) override;
	void apply(osg::Switch& node) override;
	void apply(osg::LOD& node) override;
	void apply(osg::ClearNode& node) override;
	void apply(osg::Camera& node) override;
	void apply(osg::OccluderNode& node) override;
	void apply(osg::OcclusionQueryNode& node) override;

protected:
	~ProfilerCullVisitor() override;

private:
	osg::ref_ptr<detail::ProfilerState> _state;

	unsigned int _frame() const;

	template<typename T>
	void _traced(T& node);

	// _traced(), unless isCulled(node) - the first test the base apply() makes for T.
	template<typename T>
	void _boundsTested(T& node);
};

// An osgUtil::UpdateVisitor that records every node and drawable it visits as an UPDATE
// TraceScope (object = the node). The update traversal only enters subtrees that need it (nodes
// with update callbacks, or above one), so this records the nodes whose update callbacks run.
class ProfilerUpdateVisitor: public osgUtil::UpdateVisitor {
public:
	ProfilerUpdateVisitor();

	// Copies `uv`'s traversal mode and masks.
	explicit ProfilerUpdateVisitor(const osgUtil::UpdateVisitor& uv, detail::ProfilerState* state=nullptr);

	META_NodeVisitor(osgx::debug, ProfilerUpdateVisitor)

	using osgUtil::UpdateVisitor::apply;

	void apply(osg::Node& node) override;
	void apply(osg::Drawable& drawable) override;
	void apply(osg::Geode& node) override;
	void apply(osg::Billboard& node) override;
	void apply(osg::LightSource& node) override;
	void apply(osg::Group& node) override;
	void apply(osg::Transform& node) override;
	void apply(osg::Projection& node) override;
	void apply(osg::Switch& node) override;
	void apply(osg::LOD& node) override;
	void apply(osg::OccluderNode& node) override;

protected:
	~ProfilerUpdateVisitor() override;

private:
	osg::ref_ptr<detail::ProfilerState> _state;

	template<typename T>
	void _traced(T& node);
};

// The osgGA::EventVisitor counterpart of ProfilerUpdateVisitor: EVENT TraceScopes for the nodes
// the event traversal visits (one traversal per event). Event handlers attached to the view are
// called by the viewer directly, not through this visitor, and are not recorded.
class ProfilerEventVisitor: public osgGA::EventVisitor {
public:
	ProfilerEventVisitor();

	// Copies `ev`'s action adapter, traversal mode and masks.
	explicit ProfilerEventVisitor(const osgGA::EventVisitor& ev, detail::ProfilerState* state=nullptr);

	META_NodeVisitor(osgx::debug, ProfilerEventVisitor)

	using osgGA::EventVisitor::apply;

	void apply(osg::Node& node) override;
	void apply(osg::Drawable& drawable) override;
	void apply(osg::Geode& node) override;
	void apply(osg::Billboard& node) override;
	void apply(osg::LightSource& node) override;
	void apply(osg::Group& node) override;
	void apply(osg::Transform& node) override;
	void apply(osg::Projection& node) override;
	void apply(osg::Switch& node) override;
	void apply(osg::LOD& node) override;
	void apply(osg::OccluderNode& node) override;

protected:
	~ProfilerEventVisitor() override;

private:
	osg::ref_ptr<detail::ProfilerState> _state;

	template<typename T>
	void _traced(T& node);
};

// How a waterfall shows a node's GPU time (the GPU time of its draws, its subtree's):
//   OFF       not at all (the GPU ms are still listed).
//   COST      drawn from the node's CPU start, its length the GPU time: how much GPU work the
//             node adds, next to its CPU time.
//   TIMELINE  from the first draw's GPU start to the last draw's GPU end: when the GPU ran it.
//             The GPU runs a draw some time after the CPU issues it (one frame later,
//             overlapping the next frame's CPU work, when queries are read ASYNC), so GPU work
//             trails behind its CPU work and can run past the budget without being over it.
//             GPU times are mapped onto CPU time by the drawing thread's "gpu clock" sample, so
//             the positions are approximate.
enum class WaterfallGpu { OFF, COST, TIMELINE };

// The load of a frame in ms: the CPU busy time of its busiest thread (the self time of every node
// that thread recorded, waits excluded: detail::isWaitLocation()), the GPU busy time (its draws'
// GPU time), where its waterfall bars end with each WaterfallGpu (the CPU spans, waits included;
// with COST also each node's GPU time drawn from its CPU start; with TIMELINE also each node's GPU
// span), and where its GPU work starts (ms from the frame's start; -1 without GPU times). Busy
// times are what a budget is compared with: where spans end includes waiting (with vsync, a frame
// always ends near the refresh period).
struct FrameLoad {
	double cpuMs = 0.0;
	double gpuMs = 0.0;
	double cpuEndMs = 0.0;
	double costEndMs = 0.0;
	double timelineEndMs = 0.0;
	double gpuStartMs = -1.0;

	// cpuEndMs, costEndMs or timelineEndMs.
	double endMs(WaterfallGpu gpu) const;
};

FrameLoad frameLoad(const std::array<TraceNode, 4>& profile, unsigned int frame);

// Installs the profiler on a viewer: a ProfilerCallback on every drawable of each view's scene
// data, via ProfilerVisitor; one shared ProfilerFinalCallback on
// each view's master camera; a ProfilerCullVisitor in both SceneViews of each view camera's
// (master and slaves) Renderer; the viewer's update and event visitors replaced by a
// ProfilerUpdateVisitor and ProfilerEventVisitor; DRAW spans of every camera's drawing (PRE_DRAW
// to FINAL_DRAW; master, slave and Camera nodes in the scene); and, once each context has drawn,
// a DRAW span of every buffer swap. The query readback and the profile fold are DRAW spans too
// ("readback", "profile"). After initialize(), every DRAW span the profiler records is also a
// KHR_debug group (camera and drawable spans named by objectLabel(), the others by their Location),
// so apitrace/RenderDoc/Nsight show the same structure. Attach once, after the views' scene data
// is set and before realize() (or with the viewer's threads stopped); refresh() re-runs the visitor
// after the scenes change. Stereo left/right cull visitors are not replaced.
//
// After each master camera's final draw (and the query drain), the previous frame is folded into
// the profile (TraceProfile::add(), at most once per frame); getProfile() reads it from any
// thread.
class Profiler: public osg::Referenced {
public:
	explicit Profiler(QueryMode mode=QueryMode::SYNC);

	Profiler(const Profiler&) = delete;
	Profiler& operator=(const Profiler&) = delete;

	void attach(osgViewer::ViewerBase& viewer);
	void refresh();

	void setMode(QueryMode mode);
	QueryMode getMode() const;

	// Installed hooks remain in place while disabled, but short-circuit before recording traces,
	// issuing GPU queries, pushing profiler debug groups or folding frames. Application tracing is
	// independent of this control.
	void setEnabled(bool enabled);
	bool getEnabled() const;

	// The rolling per-phase trees (TraceProfile::snapshot()).
	std::array<TraceNode, 4> getProfile() const;

	// The newest frame in the profile, if any.
	std::optional<unsigned int> getProfileFrame() const;

	// Writes the four phase trees as indented text in traversal/submission order: inclusive and
	// self ms, spans per frame, draw CPU and GPU ms, draw order, and whether a node was culled or
	// is stale (absent from the newest frame).
	void report(std::ostream& out) const;

	// Writes the newest frame as a waterfall: a row per node down to `maxDepth` levels below each
	// phase (traversal/submission order) with its CPU and GPU ms and a bar from its start to its
	// end within the frame (averages, like the profile); the GPU time of the node's draws (its
	// subtree's) overlays the bar as "#" as `gpu` says (WaterfallGpu), only over the columns it
	// covers. The axis spans `budgetMs` or the bars, if longer (FrameLoad::endMs()), in `width`
	// columns; "|" marks the budget. Bars are "=" for top-level nodes, "-" below and "." for
	// waits (detail::isWaitLocation()). The header compares the frame's CPU and GPU busy times
	// (frameLoad()) with the budget. A node without spans of its own (a CULL camera group) spans
	// its children. Nodes shorter than `minMs` (CPU and GPU) are left out with everything below
	// them, culled nodes too.
	void reportWaterfall(
		std::ostream& out,
		double budgetMs=1000.0 / 60.0,
		std::size_t width=64,
		std::size_t maxDepth=3,
		double minMs=0.05,
		WaterfallGpu gpu=WaterfallGpu::COST
	) const;

	// Folds `frame` into the profile unless it was already, or is not complete yet. Called by the
	// callback attach() installs; safe from any thread.
	void assemble(unsigned int frame);

protected:
	~Profiler() override = default;

private:
	TraceProfile::CameraNames _cameraNames() const;

	std::vector<osg::observer_ptr<osgViewer::View>> _views;
	osg::ref_ptr<detail::ProfilerState> _state;
	osg::ref_ptr<ProfilerFinalCallback> _finalCallback;
	osg::ref_ptr<osg::Camera::DrawCallback> _assembleCallback;
	osg::ref_ptr<osg::Camera::DrawCallback> _cameraEnter;
	osg::ref_ptr<osg::Camera::DrawCallback> _cameraLeave;

	TraceProfile _profile;

	std::mutex _assembleMutex;
	std::optional<unsigned int> _lastAssembled;
};

}
