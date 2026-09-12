#pragma once

#include "Callbacks.hpp"
#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GLExtensions>
#include <osg/Drawable>
#include <osg/Camera>
#include <osgUtil/CullVisitor>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>

// TODO: Make some macro wrappers for pushGroup/insertMessage that use __FUNCTION__, __FILE__, etc.
// TODO: pushGroup/insertMessage accept an "id", which we probably should manage automatically.

namespace osgx::debug {

using namespace osgx::literals;

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

enum class Severity: GLenum {
	DONT_CARE = 0x1100,
	HIGH = 0x9146,
	LOW = 0x9148,
	MEDIUM = 0x9147,
	NOTIFICATION = 0x826B
};

// TODO: The default "source" (for pushGroup/messageInsert) is APPLICATION; does this make sense?
// What are the others even FOR!?
enum class Source: GLenum {
	DONT_CARE = 0x1100,
	API = 0x8246,
	APPLICATION = 0x824A,
	OTHER = 0x824B,
	SHADER_COMPILER = 0x8248,
	THIRD_PARTY = 0x8249,
	WINDOW_SYSTEM = 0x8247
};

enum class Type: GLenum {
	DONT_CARE = 0x1100,
	DEPRECATED_BEHAVIOR = 0x824D,
	ERROR = 0x824C,
	MARKER = 0x8268,
	OTHER = 0x8251,
	PERFORMANCE = 0x8250,
	POP_GROUP = 0x826A,
	PORTABILITY = 0x824F,
	PUSH_GROUP = 0x8269,
	UNDEFINED_BEHAVIOR = 0x824E
};

namespace detail {
	const char* toString(Source s);
	const char* toString(Type t);
	const char* toString(Severity s);
}

namespace detail {
	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPushDebugGroup.xhtml
	using glPushDebugGroupFunc = void(APIENTRYP)(GLenum, GLuint, GLsizei, const char*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPopDebugGroup.xhtml
	using glPopDebugGroupFunc = void(APIENTRYP)();

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageInsert.xhtml
	using glDebugMessageInsertFunc = void(APIENTRYP)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageCallback.xhtml
	using glDebugMessageCallbackFunc = void(APIENTRYP)(GLDEBUGPROC, const void*);

	// https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDebugMessageControl.xhtml
	using glDebugMessageControlFunc = void(APIENTRYP)(
		GLenum source,
		GLenum type,
		GLenum severity,
		GLsizei count,
		const GLuint* ids,
		GLboolean enabled
	);

	// Defined once in Debug.cpp. Deliberately NOT `inline` variables: an `inline` namespace-
	// scope variable only merges into one definition within a single link unit. Any separately
	// dlopen()'d module (an osgDB plugin, most of which load via RTLD_LOCAL) that included this
	// header directly used to get its own private copy of this state, disconnected from every
	// other consumer's - e.g. a ProfilerVisitor running in one module writing into a
	// FrameAccumulator that profilerStats() in another module could never see. `extern` +
	// exactly one definition in the compiled osgx library fixes that: every consumer that links
	// osgx::osgx resolves to the same already-loaded symbol.
	extern glPushDebugGroupFunc _pushGroup;
	extern glPopDebugGroupFunc _popGroup;
	extern glDebugMessageInsertFunc _messageInsert;
	extern glDebugMessageCallbackFunc _messageCallback;
	extern glDebugMessageControlFunc _messageControl;

	// All osgx::debug log output - internal diagnostics, GL annotation mirrors, driver
	// messages - flows through this single sink. Replace it to redirect to spdlog,
	// Qt logging, an ImGui scrollback, etc. Default: osg::notify(NOTICE).
	extern std::function<void(std::string_view)> _sink;

	inline void notify(const auto&... args) {
		std::ostringstream oss;
		((oss << args), ...);
		_sink(oss.str());
	}

	void APIENTRY defaultCallback(
		GLenum source,
		GLenum type,
		GLuint id,
		GLenum severity,
		GLsizei length,
		const GLchar* message,
		const void* userParam
	);

	void pushGroup(Source source, GLuint id, const std::string& message);
	void popGroup();

	void messageInsert(
		GLenum source,
		GLenum type,
		GLuint id,
		GLenum severity,
		const std::string& message
	);

	void setCallback(GLDEBUGPROC callback, const void* userParam=nullptr);
	void clearCallback();
	void enableDebugOutput(bool synchronous=true);
	void disableDebugOutput();
	void installDefaultCallback(bool synchronous=true);

	void messageControl(
		GLenum source,
		GLenum type,
		GLenum severity,
		bool enabled,
		GLsizei count=0,
		const GLuint* ids=nullptr
	);

	// Per-context accumulator for two-phase GPU timing. ProfilerCallback (phase 1) pushes
	// pending timestamp query pairs here without blocking. ProfilerFinalCallback (phase 2)
	// syncs once and drains all results after the full camera has rendered.
	//
	// Keyed by contextID via osg::buffered_object so each GL context has its own
	// isolated accumulator; GL query objects are context-local.
	struct FrameAccumulator {
		struct Entry {
			std::string path;

			GLuint begin = 0;
			GLuint end = 0;
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

void pushGroup(Source source, GLuint id, const std::string& message);
void pushGroup(GLuint id, const std::string& message);
void popGroup();

void messageInsert(
	Source source,
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
);

void messageInsert(
	Type type,
	GLuint id,
	Severity severity,
	const std::string& message
);

// TODO: More messageInsert() wrappers for Type/Severity.

void setCallback(GLDEBUGPROC callback, const void* userParam=nullptr);
void clearCallback();
void installDefaultCallback(bool synchronous=true);

// Replace the log sink for all osgx::debug output (internal diagnostics, GL annotation
// mirrors, driver messages). The default writes to osg::notify(NOTICE).
void setSink(std::function<void(std::string_view)> sink);

void enableDebugOutput(bool synchronous=true);
void disableDebugOutput();

void messageControl(
	Source source,
	Type type,
	Severity severity,
	bool enabled,
	GLsizei count,
	const GLuint* ids=nullptr
);

void messageControl(Source source, Type type, Severity severity, bool enabled);

// TODO: Add a check for whether we're already initialized.
void initialize(osg::GraphicsContext* gc);
void deinitialize(bool disableOutput=true);

// Annotations are camera-scoped: a PRE_DRAW/FINAL_DRAW pair brackets everything that camera
// renders. Sub-groups within a single camera's scene graph cannot be individually annotated this
// way; for per-group bracketing, each group needs its own camera (RTT). For per-drawable bracketing
// use Scoped inside a DrawCallback instead.
//
// Choosing the right tool:
//
// Scoped - begin and end are in the SAME scope (e.g. inside a single drawImplementation call).
// RAII; use on the stack.
//
// AnnotationGroup - begin and end must live in SEPARATE callbacks. Its sole purpose is to share
// state across AnnotationBeginCallback (PRE_DRAW) and AnnotationEndCallback (FINAL_DRAW) so the
// push/pop remain matched across the camera's frame boundary. Not a general-purpose annotation
// type.
//
// All calls require a current GL context; never call begin()/end() from application-side C++ code
// outside a draw callback.
class AnnotationGroup: public osg::Referenced {
public:
	AnnotationGroup(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_id(id),
	_source(source),
	_message(message),
	_measureTime(measureTime) {}

	void begin();
	void end();

private:
	GLuint _id;
	Source _source;
	std::string _message;

	bool _active = false;
	bool _measureTime = false;

	osg::Timer_t _start{};
};

// RAII wrapper: opens the group on construction, closes on destruction.
// Safe on the stack inside a draw callback; do not use from application C++ code.
class Scoped {
public:
	Scoped(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_state(id, message, source, measureTime) {
		_state.begin();
	}

	~Scoped() {
		_state.end();
	}

	Scoped(const Scoped&) = delete;
	Scoped& operator=(const Scoped&) = delete;

private:
	AnnotationGroup _state;
};

enum class CameraDrawCallbackSlot {
	PRE_DRAW,
	POST_DRAW,
	FINAL_DRAW
};

namespace detail {
	osg::Camera::DrawCallback* getCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot);

	void setCameraDrawCallback(
		osg::Camera* camera,
		CameraDrawCallbackSlot slot,
		osg::Camera::DrawCallback* cb
	);
}

void appendCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot, osg::Camera::DrawCallback* cb);
void prependCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot, osg::Camera::DrawCallback* cb);

// Fires AnnotationGroup::begin() from a camera draw slot (GL context current).
class AnnotationBeginCallback: public osg::Camera::DrawCallback {
public:
	using Annotation = osg::ref_ptr<AnnotationGroup>;

	AnnotationBeginCallback(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_annotation(new AnnotationGroup(id, message, source, measureTime)) {}

	explicit AnnotationBeginCallback(Annotation annotation):
	_annotation(std::move(annotation)) {}

	void operator()(osg::RenderInfo&) const override {
		_annotation->begin();
	}

private:
	Annotation _annotation;
};

// Fires AnnotationGroup::end() from a camera draw slot (GL context current).
class AnnotationEndCallback: public osg::Camera::DrawCallback {
public:
	using Annotation = osg::ref_ptr<AnnotationGroup>;

	AnnotationEndCallback(
		GLuint id,
		std::string_view message,
		Source source=Source::APPLICATION,
		bool measureTime=false
	):
	_annotation(new AnnotationGroup(id, message, source, measureTime)) {}

	explicit AnnotationEndCallback(Annotation annotation):
	_annotation(std::move(annotation)) {}

	void operator()(osg::RenderInfo&) const override {
		_annotation->end();
	}

private:
	Annotation _annotation;
};

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

	ProfilerCallback(const std::string& name):
	ProfilerCallback(name, nullptr) {}

	ProfilerCallback(osg::Drawable::DrawCallback* cb):
	ProfilerCallback("", cb) {}

	const CPUBuffer& cpuBuffer() const { return _cpuBuffer; }

	void setProfilerName(const std::string& name) override { _name = name; }
	const std::string& getProfilerName() const override { return _name; }

	virtual void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const override {
		auto* ext = ri.getState()->get<osg::GLExtensions>();
		const auto& leafName = _name.empty() ? drawable->getName() : _name;
		const auto path = detail::cameraQualifiedPath(ri.getCurrentCamera(), leafName);
		const auto contextID = ri.getState()->getContextID();

		const auto start = osg::Timer::instance()->tick();

		Scoped scoped(0, path);

		if(ext && ext->glQueryCounter) {
			auto& acc = detail::_accumulators[contextID];
			auto e = acc.alloc(ext);

			e.path = path;

			ext->glQueryCounter(e.begin, GL_TIMESTAMP);

			if(!_cb) drawable->drawImplementation(ri);
			else _cb->drawImplementation(ri, drawable);

			ext->glQueryCounter(e.end, GL_TIMESTAMP);

			acc.push(std::move(e));
		}

		else {
			if(!_cb) drawable->drawImplementation(ri);
			else _cb->drawImplementation(ri, drawable);
		}

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

/* static const auto DEBUG_EXTENSIONS = std::vector<std::string>{
	"GL_KHR_debug",
	"GL_ARB_debug_output",
	"GL_EXT_debug_marker",
	"GL_EXT_debug_label",
	"GL_AMD_debug_output",
}; */

class GraphicsOperation: public osg::GraphicsOperation {
public:
	GraphicsOperation():
	osg::Referenced(true),
	osg::GraphicsOperation("osgx::debug::initialize Operation", false) {}

	virtual void operator()(osg::GraphicsContext* gc) {
		initialize(gc);
	}
};

// An osgViewer::Viewer subclass for apitrace-friendly debugging. Press 'n' to
// advance one frame; the viewer idles between keystrokes so apitrace captures
// never flood with unintended frames.
//
// On first render, installs AnnotationBeginCallback / AnnotationEndCallback on
// the camera so every rendered frame appears in apitrace as:
//
// Frame { [all draw calls for this frame] }
//
// Any other object with its own RTT camera (e.g. osgSlug::Atlas) can install
// its own annotation callbacks and they will nest naturally inside this group.
class FrameByFrameViewer: public osgViewer::Viewer {
public:
	class EventHandler: public osgGA::GUIEventHandler {
	public:
		bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;
	};

	FrameByFrameViewer():
	_renderKeyHandler(new EventHandler()) {
		addEventHandler(_renderKeyHandler);
	}

	void requestRender() {
		_render.store(true, std::memory_order_release);
	}

	int run() override;

private:
	void _ensureInitialized();
	void _installAnnotationCallbacks();

	static constexpr unsigned int _POLL_INTERVAL_US = 100000;

	osg::ref_ptr<EventHandler> _renderKeyHandler;

	std::atomic<bool> _render{true};
	bool _firstFrame = true;
	bool _annotationCallbacksInstalled = false;
	osg::Timer_t _startTick = osg::Timer::instance()->tick();
	unsigned int _count = 0;
};

}
