#include "osgx/debug/Profiler.hpp"

OSGX_DISABLE_WARNINGS

#include <osgUtil/CullVisitor>

OSGX_ENABLE_WARNINGS

#include <cstdint>
#include <thread>

namespace osgx::debug {

namespace {

constexpr Location DRAWABLE("drawable", Phase::DRAW);

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

Profiler::Profiler(QueryMode mode):
_finalCallback(new ProfilerFinalCallback<>(0, mode)) {}

void Profiler::attach(osgViewer::ViewerBase& viewer) {
	osgViewer::ViewerBase::Views views;

	viewer.getViews(views, false);

	for(auto* view : views) {
		_views.emplace_back(view);

		appendCameraDrawCallback(view->getCamera(), CameraDrawCallbackSlot::FINAL_DRAW, _finalCallback.get());
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
