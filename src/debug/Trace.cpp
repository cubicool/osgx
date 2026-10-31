#include "osgx/debug/Trace.hpp"

#include <algorithm>
#include <bit>

namespace osgx::debug {

namespace {

constexpr std::size_t DEFAULT_CAPACITY = 65536;

template<typename T>
void storeRelaxed(T& target, T value) {
	std::atomic_ref<T>(target).store(value, std::memory_order_relaxed);
}

// std::atomic_ref<const T> does not exist before C++26; the records themselves are never const.
template<typename T>
T loadRelaxed(const T& source) {
	return std::atomic_ref<T>(const_cast<T&>(source)).load(std::memory_order_relaxed);
}

void storeRecord(TraceRecord& slot, const TraceRecord& record) {
	storeRelaxed(slot.kind, record.kind);
	storeRelaxed(slot.frame, record.frame);
	storeRelaxed(slot.sequence, record.sequence);
	storeRelaxed(slot.tick, record.tick);
	storeRelaxed(slot.location, record.location);
	storeRelaxed(slot.object, record.object);
	storeRelaxed(slot.camera, record.camera);
	storeRelaxed(slot.gpuBegin, record.gpuBegin);
	storeRelaxed(slot.gpuEnd, record.gpuEnd);
}

void loadRecord(const TraceRecord& slot, TraceRecord& record) {
	record.kind = loadRelaxed(slot.kind);
	record.frame = loadRelaxed(slot.frame);
	record.sequence = loadRelaxed(slot.sequence);
	record.tick = loadRelaxed(slot.tick);
	record.location = loadRelaxed(slot.location);
	record.object = loadRelaxed(slot.object);
	record.camera = loadRelaxed(slot.camera);
	record.gpuBegin = loadRelaxed(slot.gpuBegin);
	record.gpuEnd = loadRelaxed(slot.gpuEnd);
}

struct Registry {
	std::mutex mutex;
	std::vector<osg::ref_ptr<TraceBuffer>> buffers;
	std::size_t capacity = DEFAULT_CAPACITY;
};

Registry& registry() {
	static Registry r;

	return r;
}

thread_local TraceBuffer* threadBuffer = nullptr;

}

namespace detail {

const char* toString(Phase phase) {
	switch(phase) {
		case Phase::EVENT: return "EVENT";
		case Phase::UPDATE: return "UPDATE";
		case Phase::CULL: return "CULL";
		case Phase::DRAW: return "DRAW";
	}

	return "UNKNOWN";
}

const char* toString(TraceRecord::Kind kind) {
	switch(kind) {
		case TraceRecord::Kind::ENTER: return "ENTER";
		case TraceRecord::Kind::LEAVE: return "LEAVE";
		case TraceRecord::Kind::INSTANT: return "INSTANT";
	}

	return "UNKNOWN";
}

}

TraceBuffer::TraceBuffer(std::size_t capacity, std::size_t index):
_records(std::bit_ceil(std::max<std::size_t>(capacity, 2))),
_index(index),
_threadID(std::this_thread::get_id()) {}

std::size_t TraceBuffer::_slot(std::uint64_t sequence) const {
	return static_cast<std::size_t>(sequence & (_records.size() - 1));
}

std::uint64_t TraceBuffer::record(
	TraceRecord::Kind kind,
	const Location& location,
	const osg::Object* object,
	unsigned int frame,
	const osg::Camera* camera
) {
	const auto sequence = _head.load(std::memory_order_relaxed);

	if(!_hasFrame || frame != _lastFrame) {
		const auto n = _numFrames.load(std::memory_order_relaxed);
		auto& start = _frames[static_cast<std::size_t>(n % MAX_FRAMES)];

		std::atomic_thread_fence(std::memory_order_release);

		start.frame.store(frame, std::memory_order_relaxed);
		start.first.store(sequence, std::memory_order_relaxed);

		_numFrames.store(n + 1, std::memory_order_release);
		_lastFrame = frame;
		_hasFrame = true;
	}

	TraceRecord record;

	record.kind = kind;
	record.frame = frame;
	record.sequence = sequence;
	record.tick = osg::Timer::instance()->tick();
	record.location = &location;
	record.object = object;
	record.camera = camera;

	// Orders the previous head store before this slot's field stores, so a reader that sees any of
	// them also sees a head that marks the slot as being overwritten.
	std::atomic_thread_fence(std::memory_order_release);

	storeRecord(_records[_slot(sequence)], record);

	_head.store(sequence + 1, std::memory_order_release);

	return sequence;
}

bool TraceBuffer::setGPUTimes(std::uint64_t sequence, std::uint64_t begin, std::uint64_t end) {
	if(sequence >= _head.load(std::memory_order_relaxed) || sequence < tail()) return false;

	auto& slot = _records[_slot(sequence)];

	if(loadRelaxed(slot.sequence) != sequence) return false;

	storeRelaxed(slot.gpuBegin, begin);
	storeRelaxed(slot.gpuEnd, end);

	return true;
}

std::uint64_t TraceBuffer::head() const {
	return _head.load(std::memory_order_acquire);
}

std::uint64_t TraceBuffer::tail() const {
	const auto h = _head.load(std::memory_order_acquire);

	return h >= _records.size() ? h - _records.size() + 1 : 0;
}

bool TraceBuffer::read(std::uint64_t sequence, TraceRecord& record) const {
	const auto h = _head.load(std::memory_order_acquire);

	if(sequence >= h || h - sequence >= _records.size()) return false;

	loadRecord(_records[_slot(sequence)], record);

	std::atomic_thread_fence(std::memory_order_acquire);

	// The writer starts overwriting this slot once the head reaches sequence + capacity.
	return
		_head.load(std::memory_order_relaxed) - sequence < _records.size() &&
		record.sequence == sequence
	;
}

std::optional<TraceBuffer::FrameRange> TraceBuffer::frameRange(unsigned int frame) const {
	const auto n = _numFrames.load(std::memory_order_acquire);
	const auto h = _head.load(std::memory_order_acquire);
	const auto oldest = n >= MAX_FRAMES ? n - MAX_FRAMES + 1 : 0;

	for(auto i = n; i > oldest; i--) {
		const auto entry = i - 1;
		const auto& start = _frames[static_cast<std::size_t>(entry % MAX_FRAMES)];
		const auto startFrame = start.frame.load(std::memory_order_relaxed);
		const auto first = start.first.load(std::memory_order_relaxed);
		const bool closed = entry + 1 < n;
		const auto end = closed
			? _frames[static_cast<std::size_t>((entry + 1) % MAX_FRAMES)].first.load(std::memory_order_relaxed)
			: h
		;

		std::atomic_thread_fence(std::memory_order_acquire);

		// Entry `entry` is rewritten once _numFrames reaches entry + MAX_FRAMES; older entries
		// are gone too.
		if(_numFrames.load(std::memory_order_relaxed) >= entry + MAX_FRAMES) return std::nullopt;

		if(startFrame == frame) return FrameRange{first, end, closed};
	}

	return std::nullopt;
}

std::vector<unsigned int> TraceBuffer::frames() const {
	const auto n = _numFrames.load(std::memory_order_acquire);
	const auto oldest = n >= MAX_FRAMES ? n - MAX_FRAMES + 1 : 0;

	std::vector<unsigned int> result;

	for(auto entry = oldest; entry < n; entry++) {
		result.push_back(_frames[static_cast<std::size_t>(entry % MAX_FRAMES)].frame.load(std::memory_order_relaxed));
	}

	std::atomic_thread_fence(std::memory_order_acquire);

	// Drop entries that may have been rewritten during the copy (entry + MAX_FRAMES <= current).
	const auto current = _numFrames.load(std::memory_order_relaxed);

	if(current >= oldest + MAX_FRAMES) {
		const auto stale = std::min<std::uint64_t>(current - MAX_FRAMES - oldest + 1, result.size());

		result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(stale));
	}

	return result;
}

std::size_t TraceBuffer::capacity() const {
	return _records.size();
}

std::size_t TraceBuffer::index() const {
	return _index;
}

std::thread::id TraceBuffer::threadID() const {
	return _threadID;
}

std::string TraceBuffer::getLabel() const {
	std::lock_guard<std::mutex> lock(_labelMutex);

	return _label;
}

void TraceBuffer::setLabel(const std::string& label) {
	std::lock_guard<std::mutex> lock(_labelMutex);

	_label = label;
}

TraceBuffer& threadTraceBuffer() {
	if(!threadBuffer) {
		auto& r = registry();

		std::lock_guard<std::mutex> lock(r.mutex);

		r.buffers.push_back(new TraceBuffer(r.capacity, r.buffers.size()));

		threadBuffer = r.buffers.back().get();
	}

	return *threadBuffer;
}

std::vector<osg::ref_ptr<TraceBuffer>> traceBuffers() {
	auto& r = registry();

	std::lock_guard<std::mutex> lock(r.mutex);

	return r.buffers;
}

void setTraceBufferCapacity(std::size_t capacity) {
	auto& r = registry();

	std::lock_guard<std::mutex> lock(r.mutex);

	r.capacity = capacity;
}

std::size_t getTraceBufferCapacity() {
	auto& r = registry();

	std::lock_guard<std::mutex> lock(r.mutex);

	return r.capacity;
}

TraceScope::TraceScope(
	const Location& location,
	const osg::Object* object,
	unsigned int frame,
	const osg::Camera* camera
):
_buffer(threadTraceBuffer()),
_location(location),
_object(object),
_frame(frame),
_camera(camera),
_sequence(_buffer.record(TraceRecord::Kind::ENTER, location, object, frame, camera)) {}

TraceScope::~TraceScope() {
	_buffer.record(TraceRecord::Kind::LEAVE, _location, _object, _frame, _camera);
}

}
