#include "osgx/debug/Trace.hpp"
#include "osgx/debug/Annotation.hpp"

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Object>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <type_traits>

namespace osgx::debug {

static_assert(sizeof(TraceRecord) == 128, "TraceRecord is two 64-byte cache lines");

namespace {

constexpr std::size_t DEFAULT_CAPACITY = 32768;

// Relaxed atomic stores and loads of record fields. On GCC/Clang the __atomic builtins (which
// std::atomic_ref is built on) compile to plain inline moves even without optimization, where
// std::atomic_ref costs several out-of-line calls per access; elsewhere std::atomic_ref is used.
// Enumerations go through their underlying type, which the builtins require.
template<typename T>
void storeRelaxed(T& target, T value) {
#if defined(__GNUC__)
	if constexpr(std::is_enum_v<T>) {
		using U = std::underlying_type_t<T>;

		__atomic_store_n(reinterpret_cast<U*>(&target), static_cast<U>(value), __ATOMIC_RELAXED);
	}

	else __atomic_store_n(&target, value, __ATOMIC_RELAXED);
#else
	std::atomic_ref<T>(target).store(value, std::memory_order_relaxed);
#endif
}

// std::atomic_ref<const T> does not exist before C++26; the records themselves are never const.
template<typename T>
T loadRelaxed(const T& source) {
#if defined(__GNUC__)
	if constexpr(std::is_enum_v<T>) {
		using U = std::underlying_type_t<T>;

		return static_cast<T>(__atomic_load_n(reinterpret_cast<const U*>(&source), __ATOMIC_RELAXED));
	}

	else return __atomic_load_n(&source, __ATOMIC_RELAXED);
#else
	return std::atomic_ref<T>(const_cast<T&>(source)).load(std::memory_order_relaxed);
#endif
}

// Copies `name` into the record, cut to NAME_SIZE - 1 bytes ending in "..." if longer.
void storeName(TraceRecord& record, std::string_view name) {
	auto* bytes = reinterpret_cast<char*>(record.nameStorage.data());

	if(name.size() < TraceRecord::NAME_SIZE) std::memcpy(bytes, name.data(), name.size());

	else {
		std::memcpy(bytes, name.data(), TraceRecord::NAME_SIZE - 4);
		std::memcpy(bytes + TraceRecord::NAME_SIZE - 4, "...", 3);
	}
}

void storeRecord(TraceRecord& slot, const TraceRecord& record) {
	storeRelaxed(slot.kind, record.kind);
	storeRelaxed(slot.phase, record.phase);
	storeRelaxed(slot.frame, record.frame);
	storeRelaxed(slot.sequence, record.sequence);
	storeRelaxed(slot.tick, record.tick);
	storeRelaxed(slot.location, record.location);
	storeRelaxed(slot.object, record.object);

	if(record.kind != TraceRecord::Kind::LEAVE) {
		storeRelaxed(slot.className, record.className);

		for(std::size_t i = 0; i < record.nameStorage.size(); i++) {
			storeRelaxed(slot.nameStorage[i], record.nameStorage[i]);
		}
	}

	storeRelaxed(slot.camera, record.camera);
	storeRelaxed(slot.gpuBegin, record.gpuBegin);
	storeRelaxed(slot.gpuEnd, record.gpuEnd);
}

void loadRecord(const TraceRecord& slot, TraceRecord& record) {
	record.kind = loadRelaxed(slot.kind);
	record.phase = loadRelaxed(slot.phase);
	record.frame = loadRelaxed(slot.frame);
	record.sequence = loadRelaxed(slot.sequence);
	record.tick = loadRelaxed(slot.tick);
	record.location = loadRelaxed(slot.location);
	record.object = loadRelaxed(slot.object);

	// Unspecified in a LEAVE record (see storeRecord()).
	if(record.kind != TraceRecord::Kind::LEAVE) {
		record.className = loadRelaxed(slot.className);

		for(std::size_t i = 0; i < record.nameStorage.size(); i++) {
			record.nameStorage[i] = loadRelaxed(slot.nameStorage[i]);
		}
	}

	else {
		record.className = nullptr;
		record.nameStorage = {};
	}

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

std::string_view TraceRecord::name() const {
	const auto* bytes = reinterpret_cast<const char*>(nameStorage.data());

	return std::string_view(bytes, strnlen(bytes, NAME_SIZE));
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

std::string objectLabel(std::string_view name, const char* className, const void* object) {
	if(!name.empty()) return std::string(name);

	return std::format(
		"{}@{:#x}",
		className ? className : "?",
		reinterpret_cast<std::uintptr_t>(object)
	);
}

TraceBuffer::TraceBuffer(std::size_t capacity, std::size_t index):
_records(std::bit_ceil(std::max<std::size_t>(capacity, 2))),
_index(index),
_threadID(std::this_thread::get_id()) {}

std::size_t TraceBuffer::_slot(std::uint64_t sequence) const {
	return static_cast<std::size_t>(sequence & (_records.size() - 1));
}

std::optional<std::uint64_t> TraceBuffer::record(
	TraceRecord::Kind kind,
	const Location& location,
	const osg::Object* object,
	unsigned int frame,
	const osg::Camera* camera
) {
	auto record = _begin(kind, location, frame, camera);

	if(!record) return std::nullopt;

	record->object = object;

	if(object && kind != TraceRecord::Kind::LEAVE) {
		record->className = object->className();

		storeName(*record, object->getName());
	}

	return _commit(*record);
}

std::optional<std::uint64_t> TraceBuffer::recordText(
	TraceRecord::Kind kind,
	const Location& location,
	std::string_view name,
	const osg::Camera* camera
) {
	auto record = _begin(kind, location, _hasFrame ? _lastFrame : 0u, camera);

	if(!record) return std::nullopt;

	if(kind != TraceRecord::Kind::LEAVE) storeName(*record, name);

	return _commit(*record);
}

TraceState TraceBuffer::state() const {
	TraceState result;

	if(_hasFrame) result.frame = _lastFrame;

	result.phase = _depth > 0 ? _openPhases[std::min(_depth, MAX_DEPTH) - 1] : _lastPhase;
	result.depth = _depth;

	return result;
}

std::optional<TraceRecord> TraceBuffer::_begin(
	TraceRecord::Kind kind,
	const Location& location,
	unsigned int frame,
	const osg::Camera* camera
) {
	const auto phase = location.phase ? location.phase : state().phase;

	if(!phase) return std::nullopt;

	if(kind == TraceRecord::Kind::ENTER) {
		if(_depth < MAX_DEPTH) _openPhases[_depth] = *phase;

		_depth++;
	}

	else if(kind == TraceRecord::Kind::LEAVE && _depth > 0) _depth--;

	_lastPhase = phase;

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
	record.phase = *phase;
	record.frame = frame;
	record.sequence = sequence;
	record.tick = osg::Timer::instance()->tick();
	record.location = &location;
	record.camera = camera;

	return record;
}

std::uint64_t TraceBuffer::_commit(const TraceRecord& record) {
	const auto sequence = record.sequence;

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
_camera(camera) {
	const auto state = _buffer.state();

	_sequence = _buffer.record(TraceRecord::Kind::ENTER, location, object, frame, camera);
	_group = _sequence && !location.phase && state.depth > 0 && state.phase == Phase::DRAW;

	if(_group) detail::pushGroup(Source::APPLICATION, 0, location.name);
}

TraceScope::TraceScope(const Location& location, const osg::Object* object):
TraceScope(location, object, threadTraceBuffer().state().frame.value_or(0)) {}

TraceScope::~TraceScope() {
	if(_group) detail::popGroup();

	if(_sequence) _buffer.record(TraceRecord::Kind::LEAVE, _location, _object, _frame, _camera);
}

}
