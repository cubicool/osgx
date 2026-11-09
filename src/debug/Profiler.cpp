#include "osgx/debug/Profiler.hpp"

#include "osgx/Callbacks.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Billboard>
#include <osg/ClearNode>
#include <osg/ClipNode>
#include <osg/LOD>
#include <osg/LightSource>
#include <osg/OccluderNode>
#include <osg/OcclusionQueryNode>
#include <osg/Projection>
#include <osg/Switch>
#include <osg/TexGenNode>
#include <osgUtil/CullVisitor>
#include <osgViewer/Renderer>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <thread>

namespace osgx::debug {

namespace {

constexpr Location DRAWABLE("drawable", Phase::DRAW);
constexpr Location CULL_NODE("cull", Phase::CULL);
constexpr Location CULLED("culled", Phase::CULL);
constexpr Location UPDATE_NODE("update", Phase::UPDATE);
constexpr Location EVENT_NODE("event", Phase::EVENT);
constexpr Location CAMERA_DRAW("camera", Phase::DRAW);
constexpr Location READBACK("readback", Phase::DRAW);
constexpr Location PROFILE_FOLD("profile", Phase::DRAW);
constexpr Location SWAP("SwapBuffers", Phase::DRAW);
constexpr Location PROFILER_GROUP("osgx profiler", Phase::DRAW);
constexpr Location GPU_CLOCK("gpu clock", Phase::DRAW);

unsigned int frameNumber(const osg::RenderInfo& ri) {
	const auto* frameStamp = ri.getState()->getFrameStamp();

	return frameStamp ? frameStamp->getFrameNumber() : 0u;
}

bool enabled(const detail::ProfilerState* state) {
	return !state || state->enabled.load(std::memory_order_relaxed);
}

// Pushes a KHR_debug group named objectLabel(object), if initialize() bound the functions.
void pushObjectGroup(const osg::Object* object) {
	if(!detail::_pushGroup) return;

	const auto& name = object->getName();

	if(!name.empty()) detail::pushGroup(Source::APPLICATION, 0, name);

	else detail::pushGroup(Source::APPLICATION, 0, objectLabel({}, object->className(), object));
}

// A KHR_debug group around a DRAW span (no-op before initialize()).
class DebugGroup {
public:
	explicit DebugGroup(std::string_view label) {
		detail::pushGroup(Source::APPLICATION, 0, label);
	}

	~DebugGroup() {
		detail::popGroup();
	}

	DebugGroup(const DebugGroup&) = delete;
	DebugGroup& operator=(const DebugGroup&) = delete;
};

// ENTER (in PRE_DRAW) or LEAVE (in FINAL_DRAW) of the camera being drawn: the camera's DRAW span,
// which the profile uses as the camera's own node, and its KHR_debug group.
class CameraSpanCallback: public osg::Camera::DrawCallback {
public:
	CameraSpanCallback(TraceRecord::Kind kind, detail::ProfilerState* state):
	_kind(kind),
	_state(state) {}

	void operator()(osg::RenderInfo& ri) const override {
		const auto* camera = ri.getCurrentCamera();

		if(!camera) return;

		static thread_local std::unordered_map<const detail::ProfilerState*, std::size_t> open;
		auto& depth = open[_state.get()];

		if(_kind == TraceRecord::Kind::ENTER) {
			if(!enabled(_state)) return;

			threadTraceBuffer().record(_kind, CAMERA_DRAW, camera, frameNumber(ri), camera);
			pushObjectGroup(camera);
			depth++;
		}

		else {
			if(!depth) return;

			detail::popGroup();
			threadTraceBuffer().record(_kind, CAMERA_DRAW, camera, frameNumber(ri), camera);
			depth--;
		}
	}

private:
	TraceRecord::Kind _kind;
	osg::ref_ptr<detail::ProfilerState> _state;
};

// Records each buffer swap of a GraphicsContext as a DRAW span, through the swap callback it had.
class SwapSpanCallback: public osg::GraphicsContext::SwapCallback {
public:
	SwapSpanCallback(osg::GraphicsContext::SwapCallback* previous, detail::ProfilerState* state):
	_previous(previous),
	_state(state) {}

	void swapBuffersImplementation(osg::GraphicsContext* gc) override {
		if(!enabled(_state)) {
			if(_previous) _previous->swapBuffersImplementation(gc);
			else gc->swapBuffersImplementation();

			return;
		}

		const auto* frameStamp = gc->getState() ? gc->getState()->getFrameStamp() : nullptr;

		TraceScope scope(SWAP, gc, frameStamp ? frameStamp->getFrameNumber() : 0u);
		DebugGroup group(SWAP.name);

		if(_previous) _previous->swapBuffersImplementation(gc);

		else gc->swapBuffersImplementation();
	}

private:
	osg::ref_ptr<osg::GraphicsContext::SwapCallback> _previous;
	osg::ref_ptr<detail::ProfilerState> _state;
};

bool hasCameraDrawCallback(osg::Camera* camera, CameraDrawCallbackSlot slot, osg::Camera::DrawCallback* cb) {
	auto* existing = detail::getCameraDrawCallback(camera, slot);

	if(existing == cb) return true;

	if(auto* group = dynamic_cast<CameraDrawCallbacksGroup*>(existing)) {
		for(std::size_t i = 0; i < group->size(); i++) {
			if(group->get(i) == cb) return true;
		}
	}

	return false;
}

// Brackets `camera`'s drawing with the camera span callbacks, first in PRE_DRAW and FINAL_DRAW.
void installCameraSpans(
	osg::Camera* camera,
	osg::Camera::DrawCallback* enter,
	osg::Camera::DrawCallback* leave
) {
	if(!camera) return;

	if(!hasCameraDrawCallback(camera, CameraDrawCallbackSlot::PRE_DRAW, enter)) {
		prependCameraDrawCallback(camera, CameraDrawCallbackSlot::PRE_DRAW, enter);
	}

	if(!hasCameraDrawCallback(camera, CameraDrawCallbackSlot::FINAL_DRAW, leave)) {
		prependCameraDrawCallback(camera, CameraDrawCallbackSlot::FINAL_DRAW, leave);
	}
}

// Installs the camera span callbacks on every Camera node in a scene (RTT and other nested cameras).
class CameraSpanVisitor: public osg::NodeVisitor {
public:
	CameraSpanVisitor(osg::Camera::DrawCallback* enter, osg::Camera::DrawCallback* leave):
	osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
	_enter(enter),
	_leave(leave) {}

	void apply(osg::Camera& camera) override {
		installCameraSpans(&camera, _enter, _leave);
		traverse(camera);
	}

private:
	osg::Camera::DrawCallback* _enter;
	osg::Camera::DrawCallback* _leave;
};

unsigned int frameNumber(const osg::NodeVisitor& nv) {
	return nv.getFrameStamp() ? nv.getFrameStamp()->getFrameNumber() : 0u;
}

void copyTraversalSettings(osg::NodeVisitor& to, const osg::NodeVisitor& from) {
	to.setTraversalMode(from.getTraversalMode());
	to.setTraversalMask(from.getTraversalMask());
	to.setNodeMaskOverride(from.getNodeMaskOverride());
}

// After the final draw (and query drain) of a frame: folds the previous frame into the profile,
// as a DRAW span of its own ("profile"), and installs the swap span callback on the context the
// first time it runs there (viewers usually create their contexts in realize()).
class AssembleCallback: public osg::Camera::DrawCallback {
public:
	AssembleCallback(Profiler* profiler, detail::ProfilerState* state):
	_profiler(profiler),
	_state(state) {
		setName("profile");
	}

	void operator()(osg::RenderInfo& ri) const override {
		if(!enabled(_state)) return;

		auto* gc = ri.getState()->getGraphicsContext();

		if(gc && !dynamic_cast<SwapSpanCallback*>(gc->getSwapCallback())) {
			gc->setSwapCallback(new SwapSpanCallback(gc->getSwapCallback(), _state));
		}

		const auto frame = frameNumber(ri);

		if(!frame) return;

		osg::ref_ptr<Profiler> profiler;

		if(!_profiler.lock(profiler)) return;

		TraceScope scope(PROFILE_FOLD, this, frame, ri.getCurrentCamera());
		DebugGroup group(PROFILE_FOLD.name);

		profiler->assemble(frame - 1);
	}

private:
	osg::observer_ptr<Profiler> _profiler;
	osg::ref_ptr<detail::ProfilerState> _state;
};

// Replaces the cull visitor of both of `camera`'s SceneViews with a ProfilerCullVisitor copy.
void installCullVisitor(osg::Camera* camera, detail::ProfilerState* state) {
	auto* renderer = camera ? dynamic_cast<osgViewer::Renderer*>(camera->getRenderer()) : nullptr;

	if(!renderer) return;

	for(unsigned int i = 0; i < 2; i++) {
		auto* sceneView = renderer->getSceneView(i);
		auto* cullVisitor = sceneView ? sceneView->getCullVisitor() : nullptr;

		if(!cullVisitor || dynamic_cast<ProfilerCullVisitor*>(cullVisitor)) continue;

		sceneView->setCullVisitor(new ProfilerCullVisitor(*cullVisitor, state));
	}
}

}

namespace detail {

const Location& culledLocation() {
	return CULLED;
}

const Location& drawableLocation() {
	return DRAWABLE;
}

const Location& profilerGroupLocation() {
	return PROFILER_GROUP;
}

bool isProfilerLocation(const Location* location) {
	return location == &READBACK || location == &PROFILE_FOLD;
}

bool isWaitLocation(const Location* location) {
	return location == &SWAP || location == &READBACK;
}

const Location& gpuClockLocation() {
	return GPU_CLOCK;
}

std::string spanLabel(
	const Location* location,
	std::string_view name,
	const char* className,
	const void* object
) {
	if(!location || isNodeLocation(location) || !name.empty()) {
		return objectLabel(name, className, object);
	}

	return location->name;
}

bool isNodeLocation(const Location* location) {
	return
		location == &DRAWABLE ||
		location == &CULL_NODE ||
		location == &CULLED ||
		location == &UPDATE_NODE ||
		location == &EVENT_NODE ||
		location == &CAMERA_DRAW
	;
}

FrameAccumulator::Entry FrameAccumulator::alloc(osg::GLExtensions* ext) {
	if(!freeList.empty()) {
		auto e = std::move(freeList.back());

		freeList.pop_back();

		return e;
	}

	Entry e;

	ext->glGenQueries(1, &e.begin);
	ext->glGenQueries(1, &e.end);

	return e;
}

void FrameAccumulator::push(Entry e) {
	pending.push_back(std::move(e));
}

void FrameAccumulator::drain(osg::GLExtensions* ext) {
	_drain(pending, ext);
}

void FrameAccumulator::swap_and_drain(osg::GLExtensions* ext) {
	_drain(ready, ext);

	std::swap(pending, ready);
}

void FrameAccumulator::_drain(std::vector<Entry>& source, osg::GLExtensions* ext) {
	for(auto& e : source) {
		GLuint64 t0 = 0, t1 = 0;

		ext->glGetQueryObjectui64v(e.begin, GL_QUERY_RESULT, &t0);
		ext->glGetQueryObjectui64v(e.end, GL_QUERY_RESULT, &t1);

		// Only the buffer's own thread may write into it; drains run on the drawing thread. A record
		// already overwritten (the buffer wrapped since the draw) drops the result.
		if(e.buffer && e.buffer->threadID() == std::this_thread::get_id()) {
			e.buffer->setGPUTimes(e.enter, t0, t1);
		}

		freeList.push_back(std::move(e));
	}

	source.clear();
}

osg::buffered_object<FrameAccumulator> _accumulators;

}

ProfilerCallback::ProfilerCallback(
	osg::Drawable::DrawCallback* cb,
	detail::ProfilerState* state
):
_cb(cb),
_state(state) {}

ProfilerCallback::~ProfilerCallback() = default;

void ProfilerCallback::drawImplementation(osg::RenderInfo& ri, const osg::Drawable* drawable) const {
	if(!enabled(_state)) {
		if(!_cb) drawable->drawImplementation(ri);
		else _cb->drawImplementation(ri, drawable);

		return;
	}

	auto* ext = ri.getState()->get<osg::GLExtensions>();
	const auto* camera = ri.getCurrentCamera();
	const auto* frameStamp = ri.getState()->getFrameStamp();
	const unsigned int frame = frameStamp ? frameStamp->getFrameNumber() : 0u;

	auto& buffer = threadTraceBuffer();

	pushObjectGroup(drawable);

	// Written always: DRAWABLE has a phase.
	const auto enter = *buffer.record(TraceRecord::Kind::ENTER, DRAWABLE, drawable, frame, camera);

	if(ext && ext->glQueryCounter) {
		auto& acc = detail::_accumulators[ri.getState()->getContextID()];
		auto e = acc.alloc(ext);

		ext->glQueryCounter(e.begin, GL_TIMESTAMP);

		if(!_cb) drawable->drawImplementation(ri);
		else _cb->drawImplementation(ri, drawable);

		ext->glQueryCounter(e.end, GL_TIMESTAMP);

		e.buffer = &buffer;
		e.enter = enter;

		acc.push(std::move(e));
	}

	else {
		if(!_cb) drawable->drawImplementation(ri);
		else _cb->drawImplementation(ri, drawable);
	}

	buffer.record(TraceRecord::Kind::LEAVE, DRAWABLE, drawable, frame, camera);

	detail::popGroup();
}

ProfilerFinalCallback::ProfilerFinalCallback(QueryMode mode, detail::ProfilerState* state):
_mode(mode),
_state(state) {
	setName("readback");
}

ProfilerFinalCallback::~ProfilerFinalCallback() = default;

void ProfilerFinalCallback::setMode(QueryMode mode) {
	_mode = mode;
}

QueryMode ProfilerFinalCallback::getMode() const {
	return _mode;
}

void ProfilerFinalCallback::operator()(osg::RenderInfo& ri) const {
	if(!enabled(_state)) return;

	auto* ext = ri.getState()->get<osg::GLExtensions>();

	if(!ext || !ext->glGetQueryObjectui64v) return;

	auto& acc = detail::_accumulators[ri.getState()->getContextID()];
	const auto frame = frameNumber(ri);

	if(ext->glGetInteger64v) {
		GLint64 gpuNs = 0;

		ext->glGetInteger64v(GL_TIMESTAMP, &gpuNs);

		auto& buffer = threadTraceBuffer();
		const auto sequence = *buffer.record(
			TraceRecord::Kind::INSTANT,
			GPU_CLOCK,
			ri.getState()->getGraphicsContext(),
			frame,
			ri.getCurrentCamera()
		);

		buffer.setGPUTimes(sequence, static_cast<std::uint64_t>(gpuNs), static_cast<std::uint64_t>(gpuNs));
	}

	TraceScope scope(READBACK, this, frame, ri.getCurrentCamera());
	DebugGroup group(READBACK.name);

	if(_mode == QueryMode::ASYNC) acc.swap_and_drain(ext);

	else acc.drain(ext);
}

ProfilerVisitor::ProfilerVisitor(detail::ProfilerState* state):
osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
_state(state) {}

ProfilerVisitor::~ProfilerVisitor() = default;

void ProfilerVisitor::apply(osg::Geode& geode) {
	traverse(geode);
}

void ProfilerVisitor::apply(osg::Drawable& drawable) {
	if(!dynamic_cast<ProfilerCallback*>(drawable.getDrawCallback())) {
		drawable.setDrawCallback(new ProfilerCallback(drawable.getDrawCallback(), _state));
	}

	traverse(drawable);
}

ProfilerCullVisitor::ProfilerCullVisitor() = default;

ProfilerCullVisitor::ProfilerCullVisitor(
	const osgUtil::CullVisitor& cv,
	detail::ProfilerState* state
):
osg::Object(cv),
osgUtil::CullVisitor(cv),
_state(state) {}

ProfilerCullVisitor::~ProfilerCullVisitor() = default;

osgUtil::CullVisitor* ProfilerCullVisitor::clone() const {
	return new ProfilerCullVisitor(*this, _state);
}

unsigned int ProfilerCullVisitor::_frame() const {
	return getFrameStamp() ? getFrameStamp()->getFrameNumber() : 0u;
}

template<typename T>
void ProfilerCullVisitor::_traced(T& node) {
	if(!enabled(_state)) {
		osgUtil::CullVisitor::apply(node);

		return;
	}

	TraceScope scope(CULL_NODE, &node, _frame(), getCurrentCamera());

	osgUtil::CullVisitor::apply(node);
}

template<typename T>
void ProfilerCullVisitor::_boundsTested(T& node) {
	if(!enabled(_state)) {
		osgUtil::CullVisitor::apply(node);

		return;
	}

	if(isCulled(node)) {
		threadTraceBuffer().record(TraceRecord::Kind::INSTANT, CULLED, &node, _frame(), getCurrentCamera());

		return;
	}

	_traced(node);
}

void ProfilerCullVisitor::apply(osg::Node& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::Geode& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::Billboard& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::LightSource& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::ClipNode& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::TexGenNode& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::Group& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::Transform& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::Projection& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::Switch& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::LOD& node) { _boundsTested(node); }
void ProfilerCullVisitor::apply(osg::ClearNode& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::Camera& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::OccluderNode& node) { _traced(node); }
void ProfilerCullVisitor::apply(osg::OcclusionQueryNode& node) { _boundsTested(node); }

// The base apply() runs the drawable's cull callback before its bounding-box test, so the test is
// repeated afterwards instead of first.
void ProfilerCullVisitor::apply(osg::Drawable& drawable) {
	if(!enabled(_state)) {
		osgUtil::CullVisitor::apply(drawable);

		return;
	}

	_traced(drawable);

	if(drawable.isCullingActive() && isCulled(drawable.getBoundingBox())) {
		threadTraceBuffer().record(TraceRecord::Kind::INSTANT, CULLED, &drawable, _frame(), getCurrentCamera());
	}
}

ProfilerUpdateVisitor::ProfilerUpdateVisitor() = default;

ProfilerUpdateVisitor::ProfilerUpdateVisitor(
	const osgUtil::UpdateVisitor& uv,
	detail::ProfilerState* state
):
_state(state) {
	copyTraversalSettings(*this, uv);
}

ProfilerUpdateVisitor::~ProfilerUpdateVisitor() = default;

template<typename T>
void ProfilerUpdateVisitor::_traced(T& node) {
	if(!enabled(_state)) {
		osgUtil::UpdateVisitor::apply(node);

		return;
	}

	TraceScope scope(UPDATE_NODE, &node, frameNumber(*this));

	osgUtil::UpdateVisitor::apply(node);
}

void ProfilerUpdateVisitor::apply(osg::Node& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Drawable& drawable) { _traced(drawable); }
void ProfilerUpdateVisitor::apply(osg::Geode& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Billboard& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::LightSource& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Group& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Transform& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Projection& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::Switch& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::LOD& node) { _traced(node); }
void ProfilerUpdateVisitor::apply(osg::OccluderNode& node) { _traced(node); }

ProfilerEventVisitor::ProfilerEventVisitor() = default;

ProfilerEventVisitor::ProfilerEventVisitor(
	const osgGA::EventVisitor& ev,
	detail::ProfilerState* state
):
_state(state) {
	copyTraversalSettings(*this, ev);
	setActionAdapter(const_cast<osgGA::GUIActionAdapter*>(ev.getActionAdapter()));
}

ProfilerEventVisitor::~ProfilerEventVisitor() = default;

template<typename T>
void ProfilerEventVisitor::_traced(T& node) {
	if(!enabled(_state)) {
		osgGA::EventVisitor::apply(node);

		return;
	}

	TraceScope scope(EVENT_NODE, &node, frameNumber(*this));

	osgGA::EventVisitor::apply(node);
}

void ProfilerEventVisitor::apply(osg::Node& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Drawable& drawable) { _traced(drawable); }
void ProfilerEventVisitor::apply(osg::Geode& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Billboard& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::LightSource& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Group& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Transform& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Projection& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::Switch& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::LOD& node) { _traced(node); }
void ProfilerEventVisitor::apply(osg::OccluderNode& node) { _traced(node); }

Profiler::Profiler(QueryMode mode):
_state(new detail::ProfilerState()),
_finalCallback(new ProfilerFinalCallback(mode, _state)),
_assembleCallback(new AssembleCallback(this, _state)),
_cameraEnter(new CameraSpanCallback(TraceRecord::Kind::ENTER, _state)),
_cameraLeave(new CameraSpanCallback(TraceRecord::Kind::LEAVE, _state)) {}

void Profiler::attach(osgViewer::ViewerBase& viewer) {
	auto* updateVisitor = viewer.getUpdateVisitor();

	if(updateVisitor && !dynamic_cast<ProfilerUpdateVisitor*>(updateVisitor)) {
		viewer.setUpdateVisitor(new ProfilerUpdateVisitor(*updateVisitor, _state));
	}

	auto* eventVisitor = viewer.getEventVisitor();

	if(eventVisitor && !dynamic_cast<ProfilerEventVisitor*>(eventVisitor)) {
		viewer.setEventVisitor(new ProfilerEventVisitor(*eventVisitor, _state));
	}

	osgViewer::ViewerBase::Views views;

	viewer.getViews(views, false);

	for(auto* view : views) {
		_views.emplace_back(view);

		appendCameraDrawCallback(view->getCamera(), CameraDrawCallbackSlot::FINAL_DRAW, _finalCallback.get());
		appendCameraDrawCallback(view->getCamera(), CameraDrawCallbackSlot::FINAL_DRAW, _assembleCallback.get());
		installCameraSpans(view->getCamera(), _cameraEnter.get(), _cameraLeave.get());
		installCullVisitor(view->getCamera(), _state);

		for(unsigned int i = 0; i < view->getNumSlaves(); i++) {
			auto* camera = view->getSlave(i)._camera.get();

			installCameraSpans(camera, _cameraEnter.get(), _cameraLeave.get());
			installCullVisitor(camera, _state);
		}
	}

	refresh();
}

void Profiler::refresh() {
	ProfilerVisitor visitor(_state);
	CameraSpanVisitor cameras(_cameraEnter.get(), _cameraLeave.get());

	// observer_ptr::lock() fails for a view with a zero reference count (a stack-allocated
	// osgViewer::Viewer), so the observed pointer is read directly; it is null once the view is
	// deleted. Call from the thread that owns the viewer.
	for(auto& observed : _views) {
		auto* view = observed.get();

		if(!view || !view->getSceneData()) continue;

		view->getSceneData()->accept(visitor);
		view->getSceneData()->accept(cameras);
	}
}

void Profiler::setMode(QueryMode mode) {
	_finalCallback->setMode(mode);
}

QueryMode Profiler::getMode() const {
	return _finalCallback->getMode();
}

void Profiler::setEnabled(bool enabled) {
	_state->enabled.store(enabled, std::memory_order_relaxed);
}

bool Profiler::getEnabled() const {
	return _state->enabled.load(std::memory_order_relaxed);
}

std::array<TraceNode, 4> Profiler::getProfile() const {
	return _profile.snapshot();
}

std::optional<unsigned int> Profiler::getProfileFrame() const {
	return _profile.lastFrame();
}

// Master and slave cameras are owned by their views, not the scene, so the cull visitor never
// records them as nodes; their names are read here.
TraceProfile::CameraNames Profiler::_cameraNames() const {
	TraceProfile::CameraNames names;

	for(const auto& observed : _views) {
		auto* view = observed.get();

		if(!view) continue;

		names[view->getCamera()] = view->getCamera()->getName();

		for(unsigned int i = 0; i < view->getNumSlaves(); i++) {
			const auto* camera = view->getSlave(i)._camera.get();

			if(camera) names[camera] = camera->getName();
		}
	}

	return names;
}

void Profiler::assemble(unsigned int frame) {
	std::lock_guard<std::mutex> lock(_assembleMutex);

	if(_lastAssembled && frame <= *_lastAssembled) return;

	if(_profile.add(frame, _cameraNames())) _lastAssembled = frame;
}

namespace {

void reportTree(std::ostream& out, const TraceNode& node, unsigned int newest, int depth) {
	char line[160];

	for(const auto& child : node.children) {
		const auto label = std::string(static_cast<std::size_t>(depth) * 2, ' ') + child.label;

		int n = std::snprintf(
			line,
			sizeof(line),
			"  %-44.44s%9.3f%9.3f%6.1f",
			label.c_str(),
			child.inclusiveMs,
			child.selfMs,
			child.count
		);

		if(child.draws > 0.0 && n > 0) {
			n += std::snprintf(
				line + n,
				sizeof(line) - static_cast<std::size_t>(n),
				"%9.3f%9.3f%5d",
				child.drawMs,
				child.gpuMs,
				child.drawOrder
			);
		}

		out << line;

		if(child.culled) out << "  culled";

		if(child.frame < newest) out << "  stale";

		out << std::endl;

		reportTree(out, child, newest, depth + 1);
	}
}

}

namespace {

struct WaterfallRow {
	const TraceNode* node;
	std::size_t depth;
	TraceSpan cpu;

	// The GPU span of the node's draws (its subtree's), if any.
	std::optional<TraceSpan> gpu;
};

void waterfallRows(
	const TraceNode& node,
	unsigned int newest,
	std::size_t depth,
	std::size_t maxDepth,
	double minMs,
	std::vector<WaterfallRow>& rows
) {
	for(const auto& child : node.children) {
		if(child.frame < newest || child.culled) continue;

		const auto cpu = cpuSpan(child, newest);

		if(!cpu) continue;

		const auto gpu = gpuSpan(child, newest);

		if(cpu->ms < minMs && !(gpu && gpu->ms >= minMs)) continue;

		rows.push_back({&child, depth, *cpu, gpu});

		if(depth < maxDepth) waterfallRows(child, newest, depth + 1, maxDepth, minMs, rows);
	}
}

}

namespace {

// Extends the bar ends of `load` (FrameLoad::endMs()) to where the waterfall bars of `node` and its
// descendants in `frame` end.
void addBarEnds(const TraceNode& node, unsigned int frame, FrameLoad& load) {
	if(node.frame != frame) return;

	if(const auto cpu = cpuSpan(node, frame)) {
		load.cpuEndMs = std::max(load.cpuEndMs, cpu->endMs);
		load.costEndMs = std::max(load.costEndMs, cpu->endMs);
		load.timelineEndMs = std::max(load.timelineEndMs, cpu->endMs);

		if(const auto gpu = gpuSpan(node, frame)) {
			load.costEndMs = std::max(load.costEndMs, cpu->startMs + gpu->ms);
			load.timelineEndMs = std::max(load.timelineEndMs, gpu->endMs);
		}
	}

	for(const auto& child : node.children) addBarEnds(child, frame, load);
}

// Adds the self time of `node` and its descendants in `frame` to their threads, waits excluded.
// Self times partition each thread's spans in time, so the sums are its busy time.
void addBusy(const TraceNode& node, unsigned int frame, std::map<std::size_t, double>& busy) {
	if(node.frame != frame) return;

	if(node.count > 0.0 && !detail::isWaitLocation(node.location)) busy[node.thread] += node.selfMs;

	for(const auto& child : node.children) addBusy(child, frame, busy);
}

}

double FrameLoad::endMs(WaterfallGpu gpu) const {
	switch(gpu) {
		case WaterfallGpu::OFF: return cpuEndMs;
		case WaterfallGpu::COST: return costEndMs;
		case WaterfallGpu::TIMELINE: return timelineEndMs;
	}

	return cpuEndMs;
}

FrameLoad frameLoad(const std::array<TraceNode, 4>& profile, unsigned int frame) {
	FrameLoad load;
	std::map<std::size_t, double> busy;

	for(const auto& phase : profile) {
		addBusy(phase, frame, busy);
		addBarEnds(phase, frame, load);

		if(const auto gpu = gpuSpan(phase, frame)) {
			load.gpuMs += gpu->ms;
			load.gpuStartMs = gpu->startMs;
		}
	}

	for(const auto& [thread, ms] : busy) load.cpuMs = std::max(load.cpuMs, ms);

	return load;
}

void Profiler::reportWaterfall(
	std::ostream& out,
	double budgetMs,
	std::size_t width,
	std::size_t maxDepth,
	double minMs,
	WaterfallGpu gpu
) const {
	const auto profile = getProfile();
	const auto newest = getProfileFrame();

	if(!newest) {
		out << "osgx::debug::Profiler: no frames yet" << std::endl;

		return;
	}

	std::vector<WaterfallRow> rows;

	for(const auto& phase : profile) waterfallRows(phase, *newest, 0, maxDepth, minMs, rows);

	const auto load = frameLoad(profile, *newest);
	const double axisMs = std::max(budgetMs, load.endMs(gpu));

	const auto column = [&](double ms) {
		return static_cast<std::size_t>(std::clamp(ms / axisMs, 0.0, 1.0) * static_cast<double>(width));
	};

	const auto budgetColumn = column(budgetMs);

	const char* legend = "";

	if(gpu == WaterfallGpu::COST) legend = "; \"#\" = GPU time from the row's start";

	else if(gpu == WaterfallGpu::TIMELINE) legend = "; \"#\" = when the GPU ran it (approximate)";

	char line[256];

	std::snprintf(
		line,
		sizeof(line),
		"osgx::debug::Profiler: frame %u: CPU %.2f ms%s, GPU %.2f ms%s (budget %.2f ms); "
		"bars end at %.2f ms%s; averages",
		*newest,
		load.cpuMs,
		load.cpuMs > budgetMs ? " OVER" : "",
		load.gpuMs,
		load.gpuMs > budgetMs ? " OVER" : "",
		budgetMs,
		load.endMs(gpu),
		legend
	);

	out << line << std::endl;

	for(std::size_t i = 0; i < rows.size(); i++) {
		const auto& row = rows[i];
		const auto* node = row.node;

		std::string bar(width, ' ');

		if(budgetColumn < width) bar[budgetColumn] = '|';

		const auto first = std::min(column(row.cpu.startMs), width - 1);
		const auto last = std::max(first + 1, column(row.cpu.endMs));

		char fill = row.depth == 0 ? '=' : '-';

		if(detail::isWaitLocation(node->location)) fill = '.';

		for(std::size_t c = first; c < last && c < width; c++) bar[c] = fill;

		// Only the columns the GPU time covers, so a short GPU time leaves the CPU bar visible.
		if(row.gpu && gpu != WaterfallGpu::OFF) {
			auto gpuFirst = first;
			auto gpuLast = column(row.cpu.startMs + row.gpu->ms);

			if(gpu == WaterfallGpu::TIMELINE) {
				gpuFirst = std::min(column(row.gpu->startMs), width - 1);
				gpuLast = std::max(gpuFirst + 1, column(row.gpu->endMs));
			}

			for(std::size_t c = gpuFirst; c < gpuLast && c < width; c++) bar[c] = '#';
		}

		const auto label = std::string(row.depth * 2, ' ') + node->label;

		char gpuMs[16] = "";

		if(row.gpu) std::snprintf(gpuMs, sizeof(gpuMs), "%.3f", row.gpu->ms);

		std::snprintf(
			line,
			sizeof(line),
			"  #%03zu  %-6s  %-36.36s%9.3f%9s  |",
			i,
			detail::toString(node->phase),
			label.c_str(),
			row.cpu.ms,
			gpuMs
		);

		out << line << bar << "|" << std::endl;
	}
}

void Profiler::report(std::ostream& out) const {
	const auto profile = getProfile();
	const auto newest = getProfileFrame();

	if(!newest) {
		out << "osgx::debug::Profiler: no frames yet" << std::endl;

		return;
	}

	char header[160];

	std::snprintf(
		header,
		sizeof(header),
		"  %-44s%9s%9s%6s%9s%9s%5s",
		"(ms per frame, moving averages)",
		"incl",
		"self",
		"count",
		"draw",
		"GPU",
		"#"
	);

	out << "osgx::debug::Profiler: frame " << *newest << std::endl << header << std::endl;

	for(const auto& phase : profile) {
		out << "  " << phase.name << std::endl;

		reportTree(out, phase, *newest, 1);
	}
}

}
