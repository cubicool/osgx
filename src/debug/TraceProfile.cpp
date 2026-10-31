#include "osgx/debug/TraceProfile.hpp"
#include "osgx/debug/Profiler.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace osgx::debug {

namespace {

// A profile node: running averages plus the current frame's totals.
struct Node {
	Phase phase = Phase::UPDATE;
	const Location* location = nullptr;
	const osg::Camera* camera = nullptr;
	const osg::Object* object = nullptr;
	std::string name;
	const char* className = nullptr;
	std::string label;

	double inclusiveMs = 0.0;
	double selfMs = 0.0;
	double drawMs = 0.0;
	double gpuMs = 0.0;
	double count = 0.0;
	double draws = 0.0;
	double startMs = 0.0;
	double gpuStartMs = -1.0;
	bool culled = false;
	int drawOrder = -1;
	unsigned int frame = 0;
	bool averaged = false;

	// This frame.
	bool touched = false;
	double frameInclusiveMs = 0.0;
	double frameSelfMs = 0.0;
	double frameDrawMs = 0.0;
	double frameGpuMs = 0.0;
	double frameCount = 0.0;
	double frameDraws = 0.0;
	double frameStartMs = -1.0;
	double frameGpuStartMs = -1.0;
	bool frameCulled = false;
	int frameDrawOrder = -1;

	std::list<Node> children;

	// Where the last lookup matched; traversal order repeats from frame to frame, so the next
	// lookup usually matches the child after it.
	std::list<Node>::iterator cursor = children.end();
};

bool transparent(const TraceRecord& record) {
	return
		detail::isNodeLocation(record.location) &&
		record.className &&
		(std::strcmp(record.className, "Group") == 0 || std::strcmp(record.className, "Geode") == 0) &&
		record.name().empty()
	;
}

bool matches(const Node& node, const osg::Camera* camera, const osg::Object* object, std::string_view name) {
	return node.camera == camera && node.object == object && node.name == name;
}

Node& child(
	Node& parent,
	Phase phase,
	const Location* location,
	const osg::Camera* camera,
	const osg::Object* object,
	std::string_view name,
	const char* className
) {
	auto& children = parent.children;

	if(!children.empty()) {
		auto start = parent.cursor == children.end() ? children.begin() : std::next(parent.cursor);

		if(start == children.end()) start = children.begin();

		auto it = start;

		do {
			if(matches(*it, camera, object, name)) {
				parent.cursor = it;

				return *it;
			}

			if(++it == children.end()) it = children.begin();
		} while(it != start);
	}

	auto& node = children.emplace_back();

	node.phase = phase;
	node.location = location;
	node.camera = camera;
	node.object = object;
	node.name = name;
	node.className = className;

	node.label = detail::spanLabel(location, name, className, object);

	parent.cursor = std::prev(children.end());

	return node;
}

struct DrawKey {
	const osg::Camera* camera;
	const osg::Object* object;

	bool operator==(const DrawKey&) const = default;
};

struct DrawKeyHash {
	std::size_t operator()(const DrawKey& key) const {
		return std::hash<const void*>()(key.camera) ^ (std::hash<const void*>()(key.object) << 1);
	}
};

struct DrawCost {
	double ms = 0.0;
	double gpuMs = 0.0;
	double draws = 0.0;
	int order = -1;
};

void blend(double& average, double value, double alpha) {
	average += alpha * (value - average);
}

// Keeps the earliest start of the frame (frame values are -1 until set).
void markStart(double& frameStart, double ms) {
	if(frameStart < 0.0 || ms < frameStart) frameStart = ms;
}

// A "gpu clock" sample: a CPU tick and the GL_TIMESTAMP read at the same moment.
struct ClockSample {
	osg::Timer_t tick;
	std::uint64_t gpuNs;
};

void copyOut(const Node& from, TraceNode& to) {
	to.phase = from.phase;
	to.location = from.location;
	to.camera = from.camera;
	to.object = from.object;
	to.name = from.name;
	to.className = from.className;
	to.label = from.label;
	to.inclusiveMs = from.inclusiveMs;
	to.selfMs = from.selfMs;
	to.drawMs = from.drawMs;
	to.gpuMs = from.gpuMs;
	to.count = from.count;
	to.draws = from.draws;
	to.startMs = from.startMs;
	to.gpuStartMs = from.gpuStartMs;
	to.culled = from.culled;
	to.drawOrder = from.drawOrder;
	to.frame = from.frame;

	for(const auto& c : from.children) copyOut(c, to.children.emplace_back());
}

}

struct TraceProfile::Impl {
	std::size_t window;
	double alpha;

	std::array<Node, 4> phases;
	std::optional<unsigned int> lastFrame;

	// Reused across frames.
	std::vector<std::vector<TraceRecord>> records;
	std::unordered_map<DrawKey, DrawCost, DrawKeyHash> drawCosts;
	std::vector<ClockSample> clockSamples;
	CameraNames names;

	struct Open {
		Node* node;
		const TraceRecord* enter;
		double childMs = 0.0;

		// An unnamed Group/Geode span: `node` is the enclosing node, which it adds nothing to.
		bool transparent = false;
	};

	std::array<std::vector<Open>, 4> stacks;

	void fold(
		const std::vector<TraceRecord>& sourceRecords,
		unsigned int frame,
		osg::Timer_t origin
	);
	void finish(Node& node, unsigned int frame, bool joinDraws);
};

void TraceProfile::Impl::fold(
	const std::vector<TraceRecord>& sourceRecords,
	unsigned int frame,
	osg::Timer_t origin
) {
	const auto* timer = osg::Timer::instance();
	const auto& culled = detail::culledLocation();
	const auto& drawable = detail::drawableLocation();
	const auto& gpuClock = detail::gpuClockLocation();
	int drawOrder = 0;

	for(auto& stack : stacks) stack.clear();

	clockSamples.clear();

	for(const auto& record : sourceRecords) {
		if(record.location == &gpuClock && record.gpuBegin) {
			clockSamples.push_back({record.tick, record.gpuBegin});
		}
	}

	// The GPU start of a drawable's draw in ms from `origin`, through this thread's clock sample
	// closest in time; negative if the draw has no GPU times or the thread no sample.
	auto gpuStart = [&](const TraceRecord& enter) {
		if(enter.gpuEnd <= enter.gpuBegin || clockSamples.empty()) return -1.0;

		auto distance = [&enter](const ClockSample& sample) {
			return sample.tick > enter.tick ? sample.tick - enter.tick : enter.tick - sample.tick;
		};

		const auto& sample = *std::min_element(
			clockSamples.begin(),
			clockSamples.end(),
			[&distance](const ClockSample& a, const ClockSample& b) { return distance(a) < distance(b); }
		);

		const auto offsetNs = static_cast<std::int64_t>(enter.gpuBegin - sample.gpuNs);

		return timer->delta_m(origin, sample.tick) + static_cast<double>(offsetNs) / 1.0e6;
	};

	for(const auto& record : sourceRecords) {
		if(record.location == &gpuClock) continue;

		const auto phase = record.location->phase;
		const double recordMs = timer->delta_m(origin, record.tick);
		auto& stack = stacks[static_cast<std::size_t>(phase)];

		if(record.kind == TraceRecord::Kind::LEAVE) {
			if(stack.empty()) continue;

			const auto open = stack.back();
			const double ms = timer->delta_m(open.enter->tick, record.tick);

			stack.pop_back();

			// Only its children's time leaves the enclosing node's self time.
			if(open.transparent) {
				if(!stack.empty()) stack.back().childMs += open.childMs;

				continue;
			}

			if(!stack.empty()) stack.back().childMs += ms;

			auto& node = *open.node;

			node.frameInclusiveMs += ms;
			node.frameSelfMs += ms - open.childMs;
			node.frameCount += 1.0;

			if(record.location == &drawable) {
				const double gpuMs = open.enter->gpuEnd > open.enter->gpuBegin
					? static_cast<double>(open.enter->gpuEnd - open.enter->gpuBegin) / 1.0e6
					: 0.0
				;

				node.frameDrawMs += ms;
				node.frameGpuMs += gpuMs;
				node.frameDraws += 1.0;

				const auto start = gpuStart(*open.enter);

				if(start >= 0.0) markStart(node.frameGpuStartMs, start);

				if(node.frameDrawOrder < 0) node.frameDrawOrder = drawOrder;

				auto& cost = drawCosts[{record.camera, record.object}];

				cost.ms += ms;
				cost.gpuMs += gpuMs;
				cost.draws += 1.0;

				if(cost.order < 0) cost.order = drawOrder;

				drawOrder++;
			}

			continue;
		}

		Node* parent = &phases[static_cast<std::size_t>(phase)];
		Node* self = nullptr;

		if(!stack.empty()) parent = stack.back().node;

		else if(record.camera && (phase == Phase::CULL || phase == Phase::DRAW)) {
			const auto it = names.find(record.camera);
			const std::string_view name = it != names.end() ? std::string_view(it->second) : std::string_view();

			parent = &child(*parent, phase, nullptr, record.camera, record.camera, name, "Camera");
			parent->touched = true;
			parent->frame = frame;

			markStart(parent->frameStartMs, recordMs);

			// A span of the camera itself (its drawing) is the camera group's own.
			if(record.object == static_cast<const osg::Object*>(record.camera)) {
				self = parent;

				if(!self->location) self->location = record.location;
			}
		}

		if(record.kind == TraceRecord::Kind::ENTER && transparent(record)) {
			stack.push_back({parent, &record, 0.0, true});

			continue;
		}

		auto& node = self
			? *self
			: child(*parent, phase, record.location, record.camera, record.object, record.name(), record.className)
		;

		node.touched = true;
		node.frame = frame;

		markStart(node.frameStartMs, recordMs);

		if(record.kind == TraceRecord::Kind::INSTANT) {
			if(record.location == &culled) node.frameCulled = true;

			continue;
		}

		stack.push_back({&node, &record});
	}
}

// Blends this frame's totals into the averages of every node touched this frame, resets the
// totals, and drops children not seen within the window. CULL nodes first take their drawable's
// DRAW totals.
void TraceProfile::Impl::finish(Node& node, unsigned int frame, bool joinDraws) {
	if(node.touched) {
		if(joinDraws && node.object) {
			const auto it = drawCosts.find({node.camera, node.object});

			if(it != drawCosts.end()) {
				node.frameDrawMs = it->second.ms;
				node.frameGpuMs = it->second.gpuMs;
				node.frameDraws = it->second.draws;
				node.frameDrawOrder = it->second.order;
			}
		}

		if(!node.averaged) {
			node.inclusiveMs = node.frameInclusiveMs;
			node.selfMs = node.frameSelfMs;
			node.drawMs = node.frameDrawMs;
			node.gpuMs = node.frameGpuMs;
			node.count = node.frameCount;
			node.draws = node.frameDraws;
			node.startMs = node.frameStartMs;
			node.averaged = true;
		}

		else {
			blend(node.inclusiveMs, node.frameInclusiveMs, alpha);
			blend(node.selfMs, node.frameSelfMs, alpha);
			blend(node.drawMs, node.frameDrawMs, alpha);
			blend(node.gpuMs, node.frameGpuMs, alpha);
			blend(node.count, node.frameCount, alpha);
			blend(node.draws, node.frameDraws, alpha);
			blend(node.startMs, node.frameStartMs, alpha);
		}

		if(node.frameGpuStartMs >= 0.0) {
			if(node.gpuStartMs < 0.0) node.gpuStartMs = node.frameGpuStartMs;

			else blend(node.gpuStartMs, node.frameGpuStartMs, alpha);
		}

		node.culled = node.frameCulled;
		node.drawOrder = node.frameDrawOrder;

		node.touched = false;
		node.frameInclusiveMs = 0.0;
		node.frameSelfMs = 0.0;
		node.frameDrawMs = 0.0;
		node.frameGpuMs = 0.0;
		node.frameCount = 0.0;
		node.frameDraws = 0.0;
		node.frameStartMs = -1.0;
		node.frameGpuStartMs = -1.0;
		node.frameCulled = false;
		node.frameDrawOrder = -1;
	}

	for(auto& c : node.children) finish(c, frame, joinDraws);

	node.children.remove_if([this, frame](const Node& c) {
		return frame > c.frame + window;
	});

	node.cursor = node.children.end();
}

std::optional<TraceSpan> cpuSpan(const TraceNode& node, unsigned int frame) {
	if(node.frame == frame && node.count > 0.0) {
		return TraceSpan{node.startMs, node.startMs + node.inclusiveMs, node.inclusiveMs};
	}

	std::optional<TraceSpan> span;

	for(const auto& child : node.children) {
		if(child.frame != frame) continue;

		if(const auto c = cpuSpan(child, frame)) {
			if(!span) span = c;

			else {
				span->startMs = std::min(span->startMs, c->startMs);
				span->endMs = std::max(span->endMs, c->endMs);
			}
		}
	}

	if(span) span->ms = span->endMs - span->startMs;

	return span;
}

std::optional<TraceSpan> gpuSpan(const TraceNode& node, unsigned int frame) {
	std::optional<TraceSpan> span;

	if(node.frame == frame && node.phase == Phase::DRAW && node.draws > 0.0 && node.gpuStartMs >= 0.0) {
		span = TraceSpan{node.gpuStartMs, node.gpuStartMs + node.gpuMs, node.gpuMs};
	}

	for(const auto& child : node.children) {
		if(child.frame != frame) continue;

		if(const auto c = gpuSpan(child, frame)) {
			if(!span) span = c;

			else {
				span->startMs = std::min(span->startMs, c->startMs);
				span->endMs = std::max(span->endMs, c->endMs);
				span->ms += c->ms;
			}
		}
	}

	return span;
}

TraceProfile::TraceProfile(std::size_t window):
_impl(std::make_unique<Impl>()) {
	_impl->window = window ? window : 1;
	_impl->alpha = 2.0 / (static_cast<double>(_impl->window) + 1.0);

	for(std::size_t i = 0; i < _impl->phases.size(); i++) {
		_impl->phases[i].phase = static_cast<Phase>(i);
		_impl->phases[i].name = detail::toString(static_cast<Phase>(i));
		_impl->phases[i].label = _impl->phases[i].name;
	}
}

TraceProfile::~TraceProfile() = default;

bool TraceProfile::add(unsigned int frame, const CameraNames& cameraNames) {
	std::lock_guard<std::mutex> lock(_mutex);

	auto& impl = *_impl;
	const auto buffers = traceBuffers();
	std::size_t used = 0;

	// Copy the frame's records out of every buffer before touching the tree, so a buffer that
	// overwrites part of it meanwhile leaves the profile unchanged.
	for(const auto& buffer : buffers) {
		const auto range = buffer->frameRange(frame);

		if(!range) continue;

		if(!range->closed || range->first < buffer->tail()) return false;

		if(impl.records.size() <= used) impl.records.emplace_back();

		auto& records = impl.records[used++];

		records.resize(static_cast<std::size_t>(range->end - range->first));

		for(std::size_t i = 0; i < records.size(); i++) {
			if(!buffer->read(range->first + i, records[i])) return false;
		}
	}

	if(!used) return false;

	// Camera names: the caller's, then the names of Camera nodes the cull visitor recorded.
	impl.names = cameraNames;

	for(std::size_t s = 0; s < used; s++) {
		for(const auto& record : impl.records[s]) {
			if(
				record.kind == TraceRecord::Kind::ENTER &&
				record.location->phase == Phase::CULL &&
				record.className &&
				std::strcmp(record.className, "Camera") == 0 &&
				!record.name().empty()
			) impl.names.emplace(static_cast<const osg::Camera*>(record.object), record.name());
		}
	}

	impl.drawCosts.clear();

	// The frame's start: its earliest record on any thread (each buffer's records are in tick
	// order).
	std::optional<osg::Timer_t> origin;

	for(std::size_t s = 0; s < used; s++) {
		if(impl.records[s].empty()) continue;

		const auto first = impl.records[s].front().tick;

		origin = origin ? std::min(*origin, first) : first;
	}

	if(!origin) return false;

	for(std::size_t s = 0; s < used; s++) impl.fold(impl.records[s], frame, *origin);

	for(auto& phase : impl.phases) {
		phase.frame = frame;

		impl.finish(phase, frame, phase.phase == Phase::CULL);
	}

	impl.lastFrame = frame;

	return true;
}

std::array<TraceNode, 4> TraceProfile::snapshot() const {
	std::lock_guard<std::mutex> lock(_mutex);

	std::array<TraceNode, 4> result;

	for(std::size_t i = 0; i < result.size(); i++) copyOut(_impl->phases[i], result[i]);

	return result;
}

std::optional<unsigned int> TraceProfile::lastFrame() const {
	std::lock_guard<std::mutex> lock(_mutex);

	return _impl->lastFrame;
}

}
