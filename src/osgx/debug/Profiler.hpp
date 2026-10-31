#pragma once

#include "osgx/Core.hpp"
#include "osgx/debug/Annotation.hpp"
#include "osgx/debug/Trace.hpp"

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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace osgx::debug {

inline constexpr size_t DEFAULT_BUFFER_SIZE = 60;

enum class CullState {
	UNKNOWN,
	VISIBLE,
	CULLED
};

// Under threading models that pipeline cull and draw across threads (anything but
// SingleThreaded), the cull traversal for frame N+1 runs concurrently with the draw
// traversal for frame N - so by the time draw(N) reads a path's cullFrame, cull has
// typically already moved on and stamped it with N+1 (or later). cullFrame >= frameNum
// is therefore normal, not staleness; only a cullFrame that has fallen genuinely BEHIND
// frameNum (the path hasn't actually been visited by cull in a while) indicates the
// cullState can no longer be trusted.
inline constexpr unsigned int CULL_STALE_TOLERANCE = 2;

inline bool isCullStale(
	unsigned int cullFrame,
	unsigned int frameNum,
	unsigned int tolerance=CULL_STALE_TOLERANCE
) {
	return frameNum > cullFrame && (frameNum - cullFrame) > tolerance;
}

namespace detail {
	// Per-context accumulator for two-phase GPU timing. ProfilerCallback (phase 1) pushes
	// pending timestamp query pairs here without blocking. ProfilerFinalCallback (phase 2)
	// syncs once and drains all results after the full camera has rendered.
	//
	// Keyed by contextID via osg::buffered_object so each GL context has its own
	// isolated accumulator; GL query objects are context-local.
	struct FrameAccumulator {
		struct Entry {
			GLuint begin = 0;
			GLuint end = 0;

			// The drawable's ENTER and LEAVE records in the drawing thread's TraceBuffer.
			TraceBuffer* buffer = nullptr;
			std::uint64_t enter = 0;
			std::uint64_t leave = 0;
		};

		class GPUStatsBuffer {
		public:
			void add(GLuint64 value, size_t capacity=DEFAULT_BUFFER_SIZE) {
				if(!capacity) capacity = 1;

				_capacity = capacity;
				_samples.push_back(value);

				while(_samples.size() > _capacity) _samples.erase(_samples.begin());
			}

			GLuint64 average() const {
				return average(_samples.size());
			}

			GLuint64 average(size_t count) const {
				count = std::min(count, _samples.size());

				if(!count) return 0;

				GLuint64 sum = 0;
				const auto first = _samples.size() - count;

				for(size_t i = first; i < _samples.size(); i++) sum += _samples[i];

				return sum / static_cast<GLuint64>(count);
			}

			size_t size() const { return _samples.size(); }
			size_t capacity() const { return _capacity; }

		private:
			std::vector<GLuint64> _samples;
			size_t _capacity = DEFAULT_BUFFER_SIZE;
		};

		struct PathStats {
			GPUStatsBuffer gpuBuffer;

			// CPU nanoseconds between the drawable's ENTER and LEAVE trace records.
			GPUStatsBuffer cpuBuffer;

			CullState cullState = CullState::UNKNOWN;
			unsigned int cullFrame = 0;

			size_t samplesSincePrint = 0;
		};

		using Stats = std::unordered_map<std::string, PathStats>;

		std::vector<Entry> pending;
		std::vector<Entry> ready;
		std::vector<Entry> freeList;
		Stats stats;

		// stats is written from the cull traversal (markCull()) and read/written from
		// the draw traversal (_drain(), and readers via snapshot()/profilerStats()).
		// Under any threading model that pipelines cull and draw across threads
		// (i.e. anything but SingleThreaded), those overlap - a plain unordered_map
		// isn't safe for that. unique_ptr (not a bare mutex) keeps FrameAccumulator
		// move-constructible, which osg::buffered_object's vector needs when it grows.
		std::unique_ptr<std::mutex> statsMutex = std::make_unique<std::mutex>();

		// Thread-safe copy for readers (e.g. the ImGui profiler table) that shouldn't
		// hold the lock - and therefore block the cull thread - while rendering.
		Stats snapshot() const;

		Entry alloc(osg::GLExtensions* ext);
		void push(Entry e);
		void markCull(const std::string& path, CullState state, unsigned int frameNum);

		// SYNC: drain current frame's pending queries, blocking per-query.
		void drain(
			osg::GLExtensions* ext,
			size_t sampleWindow,
			size_t printEvery,
			unsigned int frameNum
		);

		// ASYNC: drain the previous frame's results (should already be ready),
		// then advance the double-buffer so this frame's pending becomes next
		// frame's ready. One-frame lag; ring-buffer averaging makes it irrelevant.
		void swap_and_drain(
			osg::GLExtensions* ext,
			size_t sampleWindow,
			size_t printEvery,
			unsigned int frameNum
		);

	private:
		void _drain(
			std::vector<Entry>& source,
			osg::GLExtensions* ext,
			size_t sampleWindow,
			size_t printEvery,
			unsigned int frameNum
		);
	};

	// Defined once in Debug.cpp - see the comment above _pushGroup et al. for why this
	// must be `extern`, not `inline`.
	extern osg::buffered_object<FrameAccumulator> _accumulators;
}

// Thread-safe snapshot of per-path GPU timing stats for the given GL context.
// Populated by ProfilerFinalCallback at the end of each frame, and by
// ProfilerCullCallback during the cull traversal. Returns a copy (not a
// reference) because those two writers can run concurrently with this call
// under any threading model that pipelines cull and draw across threads --
// safe to call from onDraw regardless (POST_DRAW fires before FINAL_DRAW in
// OSG, so you see the previous frame's data - ring-buffer averaging makes
// that irrelevant).
detail::FrameAccumulator::Stats profilerStats(unsigned int contextID);

// GPU/glGenQueries Profiling
//
// Two-phase design: ProfilerCallback (phase 1) wraps each drawable and issues glQueryCounter
// begin/end without blocking. ProfilerFinalCallback (phase 2) drains the accumulated results after
// the camera finishes rendering.
//
// Usage:
//
// root->accept(osgx::debug::ProfilerVisitor());
// appendCameraDrawCallback(cam, FINAL_DRAW, new osgx::debug::ProfilerFinalCallback());
enum class QueryMode { SYNC, ASYNC };

namespace detail {
	// A drawable reachable through more than one camera/parent path (e.g. the same model
	// attached under both a ShadowCam and a G-Buffer camera) shares ONE osg::Drawable
	// instance and therefore one ProfilerCallback - there is physically only one _name
	// slot to overwrite no matter how many parents reference it. The fix is to fold in
	// *which camera is actually rendering right now*, read at the point of use (draw time
	// via RenderInfo::getCurrentCamera(), cull time via CullVisitor::getCurrentCamera()),
	// rather than trying to store multiple paths per drawable. `camera` may be null (e.g.
	// no current camera on the RenderInfo/CullVisitor stack), in which case `name` is
	// returned unchanged.
	std::string cameraQualifiedPath(const osg::Camera* camera, const std::string& name);

	// ProfilerCallback's draw: records the drawable's ENTER/LEAVE in the calling thread's
	// TraceBuffer, brackets the draw (through `cb` if not null) with GL_TIMESTAMP queries pushed to
	// the context's FrameAccumulator, and wraps it in a KHR_debug group. `name` is the profiler
	// name (the drawable's own name if empty).
	void profiledDraw(
		osg::RenderInfo& ri,
		const osg::Drawable* drawable,
		const osg::Drawable::DrawCallback* cb,
		const std::string& name
	);
}

class ProfilerCallbackBase: public osg::Drawable::DrawCallback {
public:
	virtual void setProfilerName(const std::string& name) = 0;
	virtual const std::string& getProfilerName() const = 0;
};

class ProfilerCullCallbackBase: public osg::DrawableCullCallback {
public:
	virtual void setProfilerName(const std::string& name) = 0;
	virtual const std::string& getProfilerName() const = 0;
};

// Phase 1: wrap a drawable, bracket its draw with GL timestamp queries.
// Never blocks; safe in any threading model.
template<size_t N=DEFAULT_BUFFER_SIZE>
class ProfilerCallback: public ProfilerCallbackBase {
public:
	using CPUBuffer = osgx::aring_buffer<decltype(osg::Timer::instance()->tick()), N>;

	ProfilerCallback(const std::string& name="", osg::Drawable::DrawCallback* cb=nullptr):
	_name(name),
	_cb(cb) {}

	ProfilerCallback(osg::Drawable::DrawCallback* cb):
	ProfilerCallback("", cb) {}

	const CPUBuffer& cpuBuffer() const { return _cpuBuffer; }

	void setProfilerName(const std::string& name) override { _name = name; }
	const std::string& getProfilerName() const override { return _name; }

	virtual void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const override {
		const auto start = osg::Timer::instance()->tick();

		detail::profiledDraw(ri, drawable, _cb.get(), _name);

		_cpuBuffer.add(osg::Timer::instance()->tick() - start);
	}

protected:
	std::string _name;
	osg::ref_ptr<osg::Drawable::DrawCallback> _cb;
	mutable CPUBuffer _cpuBuffer;
};

class ProfilerCullCallback: public ProfilerCullCallbackBase {
public:
	ProfilerCullCallback(const std::string& name="", osg::Callback* cb=nullptr):
	_name(name),
	_cb(cb) {}

	void setProfilerName(const std::string& name) override { _name = name; }
	const std::string& getProfilerName() const override { return _name; }

	bool cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* ri) const override;

private:
	std::string _name;
	osg::ref_ptr<osg::Callback> _cb;
};

// Phase 2: install on a camera via FINAL_DRAW slot to drain the FrameAccumulator.
//
// QueryMode::SYNC (default): blocks per-query on GL_QUERY_RESULT for the current
// frame. Fine for step-by-step viewers; may stall continuous rendering.
//
// QueryMode::ASYNC: drains the previous frame's results (already retired by the
// GPU) then swaps buffers. Zero meaningful stall; one-frame lag on results that the
// ring-buffer averaging makes irrelevant.
template<size_t N=DEFAULT_BUFFER_SIZE>
class ProfilerFinalCallback: public osg::Camera::DrawCallback {
public:
	explicit ProfilerFinalCallback(size_t printEvery=N, QueryMode mode=QueryMode::SYNC):
	_printEvery(printEvery),
	_mode(mode) {}

	// TODO: Switching ASYNC->SYNC leaves one set of retired query objects in
	// FrameAccumulator::ready that SYNC will never drain. Harmless for a debug
	// tool (one-time minor GL object leak), but a future cleanup pass should
	// flush ready on mode change. SYNC->ASYNC is clean.
	void setMode(QueryMode mode) { _mode = mode; }
	QueryMode getMode() const { return _mode; }

	void operator()(osg::RenderInfo& ri) const override {
		auto* ext = ri.getState()->get<osg::GLExtensions>();

		if(!ext || !ext->glGetQueryObjectui64v) return;

		const unsigned int frameNum = ri.getState()->getFrameStamp()
			? ri.getState()->getFrameStamp()->getFrameNumber()
			: 0u
		;

		auto& acc = detail::_accumulators[ri.getState()->getContextID()];

		if(_mode == QueryMode::ASYNC) acc.swap_and_drain(ext, N, _printEvery, frameNum);

		else acc.drain(ext, N, _printEvery, frameNum);
	}

private:
	size_t _printEvery;
	QueryMode _mode;
};

// Walks the scene graph and installs a ProfilerCallback on every Drawable.
// Install ProfilerFinalCallback on the camera separately to complete setup.
template<size_t N=DEFAULT_BUFFER_SIZE>
class ProfilerVisitor: public osg::NodeVisitor {
public:
	ProfilerVisitor():
	osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}

	virtual void apply(osg::Geode& g) {
		traverse(g);
	}

	virtual void apply(osg::Drawable& d) {
		std::string path;

		for(auto* n : getNodePath()) {
			path += "/";
			path += n->getName().empty() ? n->className() : n->getName();
		}

		// In pre-Node-Drawable OSG, the drawable isn't pushed onto the NodePath.
		if(getNodePath().empty() || getNodePath().back() != &d) {
			path += "/";
			path += d.getName().empty() ? d.className() : d.getName();
		}

		if(auto* profiler = dynamic_cast<ProfilerCallbackBase*>(d.getDrawCallback())) {
			profiler->setProfilerName(path);

			detail::notify("osgx::debug::ProfilerVisitor | Updating ProfilerCallback on ", path);
		}

		else {
			auto dcb = new ProfilerCallback<N>(path, d.getDrawCallback());

			detail::notify("osgx::debug::ProfilerVisitor | Setting ProfilerCallback on ", path);

			d.setDrawCallback(dcb);
		}

		if(auto* profiler = dynamic_cast<ProfilerCullCallbackBase*>(d.getCullCallback())) {
			profiler->setProfilerName(path);

			detail::notify("osgx::debug::ProfilerVisitor | Updating ProfilerCullCallback on ", path);
		}

		else {
			auto ccb = new ProfilerCullCallback(path, d.getCullCallback());

			detail::notify("osgx::debug::ProfilerVisitor | Setting ProfilerCullCallback on ", path);

			d.setCullCallback(ccb);
		}

		traverse(d);
	}
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
	explicit ProfilerCullVisitor(const osgUtil::CullVisitor& cv);

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
	explicit ProfilerUpdateVisitor(const osgUtil::UpdateVisitor& uv);

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
	explicit ProfilerEventVisitor(const osgGA::EventVisitor& ev);

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
	template<typename T>
	void _traced(T& node);
};

// Installs the profiler on a viewer: a ProfilerCallback (and ProfilerCullCallback) on every
// drawable of each view's scene data, via ProfilerVisitor; one shared ProfilerFinalCallback on
// each view's master camera; a ProfilerCullVisitor in both SceneViews of each view camera's
// (master and slaves) Renderer; and the viewer's update and event visitors replaced by a
// ProfilerUpdateVisitor and ProfilerEventVisitor. Attach once, after the views' scene data is set and before
// realize() (or with the viewer's threads stopped); refresh() re-runs the visitor after the scenes
// change. Stereo left/right cull visitors are not replaced.
class Profiler: public osg::Referenced {
public:
	explicit Profiler(QueryMode mode=QueryMode::SYNC);

	Profiler(const Profiler&) = delete;
	Profiler& operator=(const Profiler&) = delete;

	void attach(osgViewer::ViewerBase& viewer);
	void refresh();

	void setMode(QueryMode mode);
	QueryMode getMode() const;

protected:
	~Profiler() override = default;

private:
	std::vector<osg::observer_ptr<osgViewer::View>> _views;
	osg::ref_ptr<ProfilerFinalCallback<>> _finalCallback;
};

}
