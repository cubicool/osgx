#include "osgx/debug/Profiler.hpp"

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

#include <cstdint>
#include <thread>

namespace osgx::debug {

namespace {

constexpr Location DRAWABLE("drawable", Phase::DRAW);
constexpr Location CULL_NODE("cull", Phase::CULL);
constexpr Location CULLED("culled", Phase::CULL);
constexpr Location UPDATE_NODE("update", Phase::UPDATE);
constexpr Location EVENT_NODE("event", Phase::EVENT);

unsigned int frameNumber(const osg::NodeVisitor& nv) {
	return nv.getFrameStamp() ? nv.getFrameStamp()->getFrameNumber() : 0u;
}

void copyTraversalSettings(osg::NodeVisitor& to, const osg::NodeVisitor& from) {
	to.setTraversalMode(from.getTraversalMode());
	to.setTraversalMask(from.getTraversalMask());
	to.setNodeMaskOverride(from.getNodeMaskOverride());
}

// Replaces the cull visitor of both of `camera`'s SceneViews with a ProfilerCullVisitor copy.
void installCullVisitor(osg::Camera* camera) {
	auto* renderer = camera ? dynamic_cast<osgViewer::Renderer*>(camera->getRenderer()) : nullptr;

	if(!renderer) return;

	for(unsigned int i = 0; i < 2; i++) {
		auto* sceneView = renderer->getSceneView(i);
		auto* cullVisitor = sceneView ? sceneView->getCullVisitor() : nullptr;

		if(!cullVisitor || dynamic_cast<ProfilerCullVisitor*>(cullVisitor)) continue;

		sceneView->setCullVisitor(new ProfilerCullVisitor(*cullVisitor));
	}
}

// The profiler name of the drawable a DRAWABLE record points at. The drawable was drawn this frame
// (SYNC) or the previous one (ASYNC); one deleted in between would dangle here.
std::string profilerName(const osg::Object* object) {
	const auto* drawable = dynamic_cast<const osg::Drawable*>(object);

	if(!drawable) return {};

	const auto* profiler = dynamic_cast<const ProfilerCallbackBase*>(drawable->getDrawCallback());

	if(profiler && !profiler->getProfilerName().empty()) return profiler->getProfilerName();

	return drawable->getName();
}

}

namespace detail {

FrameAccumulator::Stats FrameAccumulator::snapshot() const {
	std::lock_guard<std::mutex> lock(*statsMutex);

	return stats;
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

void FrameAccumulator::markCull(const std::string& path, CullState state, unsigned int frameNum) {
	std::lock_guard<std::mutex> lock(*statsMutex);
	auto& s = stats[path];

	s.cullState = state;
	s.cullFrame = frameNum;
}

void FrameAccumulator::drain(
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	_drain(pending, ext, sampleWindow, printEvery, frameNum);
}

void FrameAccumulator::swap_and_drain(
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	_drain(ready, ext, sampleWindow, printEvery, frameNum);

	std::swap(pending, ready);
}

void FrameAccumulator::_drain(
	std::vector<Entry>& source,
	osg::GLExtensions* ext,
	size_t sampleWindow,
	size_t printEvery,
	unsigned int frameNum
) {
	for(auto& e : source) {
		GLuint64 t0 = 0, t1 = 0;

		ext->glGetQueryObjectui64v(e.begin, GL_QUERY_RESULT, &t0);
		ext->glGetQueryObjectui64v(e.end, GL_QUERY_RESULT, &t1);

		// Only the buffer's own thread may write into it; drains run on the drawing thread.
		if(e.buffer && e.buffer->threadID() == std::this_thread::get_id()) {
			e.buffer->setGPUTimes(e.enter, t0, t1);
		}

		TraceRecord enter;
		TraceRecord leave;

		// A record already overwritten (the buffer wrapped since the draw) drops the sample.
		if(!e.buffer || !e.buffer->read(e.enter, enter) || !e.buffer->read(e.leave, leave)) {
			freeList.push_back(std::move(e));

			continue;
		}

		const auto path = cameraQualifiedPath(enter.camera, profilerName(enter.object));
		const GLuint64 gpuNs = t1 - t0;
		const auto cpuNs = static_cast<GLuint64>(osg::Timer::instance()->delta_n(enter.tick, leave.tick));
		bool shouldPrint = false;
		GLuint64 avgNs = 0;

		{
			std::lock_guard<std::mutex> lock(*statsMutex);
			auto& s = stats[path];

			s.gpuBuffer.add(gpuNs, sampleWindow);
			s.cpuBuffer.add(cpuNs, sampleWindow);
			s.samplesSincePrint++;

			if(printEvery > 0 && s.samplesSincePrint >= printEvery) {
				s.samplesSincePrint = 0;
				shouldPrint = true;
				avgNs = s.gpuBuffer.average(printEvery);
			}
		}

		// Kept outside the lock - notify()/stdout shouldn't hold up the cull thread.
		if(shouldPrint) {
			// TODO: This is annoyingly AWFUL and should be fixed; SOON!
			notify(
				"osgx::debug::Profiler | [", path, "] GPU: ", gpuNs / 1000u, "us",
				" | avg: ", avgNs / 1000u, "us",
				" Frame: ", frameNum
			);
		}

		freeList.push_back(std::move(e));
	}

	source.clear();
}

osg::buffered_object<FrameAccumulator> _accumulators;

std::string cameraQualifiedPath(const osg::Camera* camera, const std::string& name) {
	if(!camera) return name;

	const auto& cameraName = camera->getName();

	std::string label = cameraName.empty()
		? camera->className() + std::string("@") + std::to_string(reinterpret_cast<std::uintptr_t>(camera))
		: cameraName;

	return label + "/" + name;
}

void profiledDraw(
	osg::RenderInfo& ri,
	const osg::Drawable* drawable,
	const osg::Drawable::DrawCallback* cb,
	const std::string& name
) {
	auto* ext = ri.getState()->get<osg::GLExtensions>();
	const auto* camera = ri.getCurrentCamera();
	const auto* frameStamp = ri.getState()->getFrameStamp();
	const unsigned int frame = frameStamp ? frameStamp->getFrameNumber() : 0u;
	const auto path = cameraQualifiedPath(camera, name.empty() ? drawable->getName() : name);

	auto& buffer = threadTraceBuffer();

	AnnotationScope annotation(0, path);

	const auto enter = buffer.record(TraceRecord::Kind::ENTER, DRAWABLE, drawable, frame, camera);

	if(ext && ext->glQueryCounter) {
		auto& acc = _accumulators[ri.getState()->getContextID()];
		auto e = acc.alloc(ext);

		ext->glQueryCounter(e.begin, GL_TIMESTAMP);

		if(!cb) drawable->drawImplementation(ri);
		else cb->drawImplementation(ri, drawable);

		ext->glQueryCounter(e.end, GL_TIMESTAMP);

		e.buffer = &buffer;
		e.enter = enter;
		e.leave = buffer.record(TraceRecord::Kind::LEAVE, DRAWABLE, drawable, frame, camera);

		acc.push(std::move(e));
	}

	else {
		if(!cb) drawable->drawImplementation(ri);
		else cb->drawImplementation(ri, drawable);

		buffer.record(TraceRecord::Kind::LEAVE, DRAWABLE, drawable, frame, camera);
	}
}

}

detail::FrameAccumulator::Stats profilerStats(unsigned int contextID) {
	return detail::_accumulators[contextID].snapshot();
}

bool ProfilerCullCallback::cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo* ri) const {
	const auto frameNum = nv && nv->getFrameStamp()
		? nv->getFrameStamp()->getFrameNumber()
		: 0u
	;

	const auto contextID = ri && ri->getState()
		? ri->getState()->getContextID()
		: 0u
	;

	auto* cv = nv ? nv->asCullVisitor() : nullptr;
	const auto& leafName = _name.empty() && drawable ? drawable->getName() : _name;
	const auto path = detail::cameraQualifiedPath(cv ? cv->getCurrentCamera() : nullptr, leafName);
	bool callbackCulled = false;

	if(_cb.valid()) {
		if(auto* dcb = _cb->asDrawableCullCallback()) {
			callbackCulled = dcb->cull(nv, drawable, ri);
		}

		else _cb->run(drawable, nv);
	}

	const bool boundsCulled = drawable && cv && drawable->isCullingActive()
		&& cv->isCulled(drawable->getBoundingBox())
	;

	detail::_accumulators[contextID].markCull(
		path,
		(callbackCulled || boundsCulled) ? CullState::CULLED : CullState::VISIBLE,
		frameNum
	);

	return callbackCulled;
}

ProfilerCullVisitor::ProfilerCullVisitor() = default;

ProfilerCullVisitor::ProfilerCullVisitor(const osgUtil::CullVisitor& cv):
osg::Object(cv),
osgUtil::CullVisitor(cv) {}

ProfilerCullVisitor::~ProfilerCullVisitor() = default;

osgUtil::CullVisitor* ProfilerCullVisitor::clone() const {
	return new ProfilerCullVisitor(*this);
}

unsigned int ProfilerCullVisitor::_frame() const {
	return getFrameStamp() ? getFrameStamp()->getFrameNumber() : 0u;
}

template<typename T>
void ProfilerCullVisitor::_traced(T& node) {
	TraceScope scope(CULL_NODE, &node, _frame(), getCurrentCamera());

	osgUtil::CullVisitor::apply(node);
}

template<typename T>
void ProfilerCullVisitor::_boundsTested(T& node) {
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
	_traced(drawable);

	if(drawable.isCullingActive() && isCulled(drawable.getBoundingBox())) {
		threadTraceBuffer().record(TraceRecord::Kind::INSTANT, CULLED, &drawable, _frame(), getCurrentCamera());
	}
}

ProfilerUpdateVisitor::ProfilerUpdateVisitor() = default;

ProfilerUpdateVisitor::ProfilerUpdateVisitor(const osgUtil::UpdateVisitor& uv) {
	copyTraversalSettings(*this, uv);
}

ProfilerUpdateVisitor::~ProfilerUpdateVisitor() = default;

template<typename T>
void ProfilerUpdateVisitor::_traced(T& node) {
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

ProfilerEventVisitor::ProfilerEventVisitor(const osgGA::EventVisitor& ev) {
	copyTraversalSettings(*this, ev);
	setActionAdapter(const_cast<osgGA::GUIActionAdapter*>(ev.getActionAdapter()));
}

ProfilerEventVisitor::~ProfilerEventVisitor() = default;

template<typename T>
void ProfilerEventVisitor::_traced(T& node) {
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
_finalCallback(new ProfilerFinalCallback<>(0, mode)) {}

void Profiler::attach(osgViewer::ViewerBase& viewer) {
	auto* updateVisitor = viewer.getUpdateVisitor();

	if(updateVisitor && !dynamic_cast<ProfilerUpdateVisitor*>(updateVisitor)) {
		viewer.setUpdateVisitor(new ProfilerUpdateVisitor(*updateVisitor));
	}

	auto* eventVisitor = viewer.getEventVisitor();

	if(eventVisitor && !dynamic_cast<ProfilerEventVisitor*>(eventVisitor)) {
		viewer.setEventVisitor(new ProfilerEventVisitor(*eventVisitor));
	}

	osgViewer::ViewerBase::Views views;

	viewer.getViews(views, false);

	for(auto* view : views) {
		_views.emplace_back(view);

		appendCameraDrawCallback(view->getCamera(), CameraDrawCallbackSlot::FINAL_DRAW, _finalCallback.get());
		installCullVisitor(view->getCamera());

		for(unsigned int i = 0; i < view->getNumSlaves(); i++) installCullVisitor(view->getSlave(i)._camera.get());
	}

	refresh();
}

void Profiler::refresh() {
	ProfilerVisitor<> visitor;

	// observer_ptr::lock() fails for a view with a zero reference count (a stack-allocated
	// osgViewer::Viewer), so the observed pointer is read directly; it is null once the view is
	// deleted. Call from the thread that owns the viewer.
	for(auto& observed : _views) {
		auto* view = observed.get();

		if(view && view->getSceneData()) view->getSceneData()->accept(visitor);
	}
}

void Profiler::setMode(QueryMode mode) {
	_finalCallback->setMode(mode);
}

QueryMode Profiler::getMode() const {
	return _finalCallback->getMode();
}

}
