#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Referenced>
#include <osg/Timer>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace osg {
	class Camera;
	class Object;
}

namespace osgx::debug {

// ================================================================================================
// Profiling trace buffers
//
// Every thread that records profiling data owns one TraceBuffer: a fixed-capacity ring of
// TraceRecords, written only by that thread (no locking on record) and readable from any other
// thread. Each TraceRecord is an ENTER, LEAVE or INSTANT at a static Location, optionally about an
// osg::Object and under the camera current at the time (cull and draw), at an absolute osg::Timer
// tick (comparable across threads), tagged with the FrameStamp frame number it belongs to. Nesting is implicit in ENTER/LEAVE order; trees and
// statistics are rebuilt from the records by readers, never kept by the writer.
//
// threadTraceBuffer() returns the calling thread's buffer, creating and registering it on first
// use; traceBuffers() returns every buffer created so far. Buffers outlive their threads, so a
// reader can still read the last frames of a thread that has exited.
//
// Records are published seqlock-style: the writer stores every field with relaxed atomics after a
// release fence and then publishes the new head with a release store; a reader copies the fields
// with relaxed atomic loads, then (after an acquire fence) re-reads the head to detect a slot that
// was overwritten during the copy and discards it.
// ================================================================================================

// The OSG traversal a Location belongs to. GPU time is not a phase: it is recorded on DRAW records.
enum class Phase: std::uint8_t {
	EVENT,
	UPDATE,
	CULL,
	DRAW
};

// One instrumentation site. Declare as a static constexpr so TraceRecords can point at it:
//
//   static constexpr osgx::debug::Location LOCATION("cull", osgx::debug::Phase::CULL);
//
// Without a phase, each record at the site takes the phase of the span open around it on its
// thread (see TraceState), so one site serves any traversal; OSGX_TRACE_SCOPE() declares these.
struct Location {
	constexpr Location(
		const char* n,
		Phase p,
		std::source_location source=std::source_location::current()
	):
	name(n),
	phase(p),
	file(source.file_name()),
	line(source.line()) {}

	constexpr explicit Location(
		const char* n,
		std::source_location source=std::source_location::current()
	):
	name(n),
	file(source.file_name()),
	line(source.line()) {}

	const char* name;
	std::optional<Phase> phase;
	const char* file;
	std::uint_least32_t line;
};

struct TraceRecord {
	// The bytes of `object`'s name copied into the record, including the terminating NUL; longer
	// names are cut and end in "...".
	static constexpr std::size_t NAME_SIZE = 56;

	enum class Kind: std::uint8_t {
		ENTER,
		LEAVE,
		INSTANT
	};

	Kind kind = Kind::INSTANT;

	// The location's phase, or for a location without one the phase the record took (see
	// TraceState).
	Phase phase = Phase::EVENT;

	unsigned int frame = 0;
	std::uint64_t sequence = 0;
	osg::Timer_t tick = 0;
	const Location* location = nullptr;

	// Identity only: readers compare it, never dereference it (the object may be gone by the time
	// a record is read). What readers need from the object is copied below when the ENTER or
	// INSTANT record is written; in a LEAVE record both are unspecified (not written).
	//
	// TODO: Continue to investigate workarounds for identifying objects across frames by pointer;
	// a deleted object's address can be reused by a new one, which then looks like the same object.
	const osg::Object* object = nullptr;

	// object->className(), a string literal.
	const char* className = nullptr;

	// object->getName(), NUL-terminated; see name().
	std::array<std::uint64_t, NAME_SIZE / sizeof(std::uint64_t)> nameStorage{};

	// The camera being culled or drawn (null in the event and update phases). One drawable drawn
	// by several cameras is told apart by it.
	const osg::Camera* camera = nullptr;

	// GPU timestamps in nanoseconds (GL_TIMESTAMP), written back after the query results arrive;
	// zero until then.
	std::uint64_t gpuBegin = 0;
	std::uint64_t gpuEnd = 0;

	std::string_view name() const;
};

namespace detail {
	const char* toString(Phase phase);
	const char* toString(TraceRecord::Kind kind);
}

// `name`, or "ClassName@0x<address>" if it is empty ("?" for a null className).
std::string objectLabel(std::string_view name, const char* className, const void* object);

// What a thread's trace buffer knows about where the thread is (TraceBuffer::state()): the frame of
// its latest record, the phase of its innermost open span (an ENTER without its LEAVE yet), else
// of its latest record, and how many spans are open. Records at a Location without a phase take
// `phase`; frame-less TraceScopes take `frame`.
struct TraceState {
	std::optional<unsigned int> frame;
	std::optional<Phase> phase;
	std::size_t depth = 0;
};

class TraceBuffer: public osg::Referenced {
public:
	// The number of frame starts each buffer stores. As with records, one entry is reserved for the
	// one being written, so frameRange()/frames() see the MAX_FRAMES - 1 most recent.
	static constexpr std::size_t MAX_FRAMES = 64;

	// The records of one frame on this buffer's thread: sequences [first, end). `closed` is true
	// once a later frame has started on this thread (an open range still grows). Records before
	// tail() are gone, so a range with first < tail() is truncated.
	struct FrameRange {
		std::uint64_t first = 0;
		std::uint64_t end = 0;
		bool closed = false;
	};

	// `capacity` is rounded up to a power of two (minimum 2).
	TraceBuffer(std::size_t capacity, std::size_t index);

	TraceBuffer(const TraceBuffer&) = delete;
	TraceBuffer& operator=(const TraceBuffer&) = delete;

	// Writer side: only the owning thread calls these.

	// The open spans whose phases are kept (see TraceState); deeper spans take the phase of the
	// deepest kept one.
	static constexpr std::size_t MAX_DEPTH = 64;

	// Appends a record at the current tick and returns its sequence number. A frame number
	// different from the previous record's starts a new frame range. A record at a Location
	// without a phase takes TraceState::phase; with none (nothing recorded on this thread yet) it
	// is not written and nothing is returned. A LEAVE whose ENTER was not written must not be
	// recorded either (it would close the open span around it).
	std::optional<std::uint64_t> record(
		TraceRecord::Kind kind,
		const Location& location,
		const osg::Object* object,
		unsigned int frame,
		const osg::Camera* camera=nullptr
	);

	// Appends a record without an object, named `name` (cut like object names; LEAVE records keep
	// none), in the frame of this buffer's previous record (0 before any). Used for annotations
	// and debug messages, which carry text but no frame number. Phases as record().
	std::optional<std::uint64_t> recordText(
		TraceRecord::Kind kind,
		const Location& location,
		std::string_view name,
		const osg::Camera* camera=nullptr
	);

	// Writes GPU timestamps into the record `sequence`. Returns false if it has been overwritten.
	bool setGPUTimes(std::uint64_t sequence, std::uint64_t begin, std::uint64_t end);

	// This thread's TraceState.
	TraceState state() const;

	// Reader side: any thread.

	// The sequence number the next record will get (the number of records ever written).
	std::uint64_t head() const;

	// The oldest sequence number still readable. One slot is reserved for the record being written,
	// so at most capacity() - 1 records are readable.
	std::uint64_t tail() const;

	// Copies the record `sequence` into `record`. Returns false if it was not yet written, has been
	// overwritten, or was overwritten while being copied.
	bool read(std::uint64_t sequence, TraceRecord& record) const;

	// The most recent range recorded for `frame`, if this buffer still remembers that frame start.
	std::optional<FrameRange> frameRange(unsigned int frame) const;

	// The frame numbers whose starts this buffer remembers, oldest first. A frame number appears
	// more than once if records of other frames were written in between.
	std::vector<unsigned int> frames() const;

	std::size_t capacity() const;

	// The order in which this buffer was created among all buffers (0 for the first).
	std::size_t index() const;

	std::thread::id threadID() const;

	// A free-form name for the owning thread ("draw", a GraphicsContext name, ...); empty by
	// default.
	std::string getLabel() const;
	void setLabel(const std::string& label);

protected:
	~TraceBuffer() override = default;

private:
	// Frame starts use the same publication protocol as records, with _numFrames as the head.
	struct FrameStart {
		std::atomic<unsigned int> frame{0};
		std::atomic<std::uint64_t> first{0};
	};

	std::size_t _slot(std::uint64_t sequence) const;

	// A record at the current tick with the next sequence number; starts a frame range if `frame`
	// differs from the previous record's and updates the open spans. Empty if the record has no
	// phase (see record()).
	std::optional<TraceRecord> _begin(
		TraceRecord::Kind kind,
		const Location& location,
		unsigned int frame,
		const osg::Camera* camera
	);

	// Publishes a record made by _begin().
	std::uint64_t _commit(const TraceRecord& record);

	std::vector<TraceRecord> _records;
	std::atomic<std::uint64_t> _head{0};

	std::array<FrameStart, MAX_FRAMES> _frames;
	std::atomic<std::uint64_t> _numFrames{0};

	// Owning thread only.
	unsigned int _lastFrame = 0;
	bool _hasFrame = false;

	std::array<Phase, MAX_DEPTH> _openPhases{};
	std::size_t _depth = 0;
	std::optional<Phase> _lastPhase;

	std::size_t _index;
	std::thread::id _threadID;

	mutable std::mutex _labelMutex;
	std::string _label;
};

// The calling thread's buffer, created with getTraceBufferCapacity() and registered on first use.
TraceBuffer& threadTraceBuffer();

// Every buffer created so far, in creation order.
std::vector<osg::ref_ptr<TraceBuffer>> traceBuffers();

// The capacity (in records) of buffers created after this call. Default 32768 (4 MB per buffer).
void setTraceBufferCapacity(std::size_t capacity);
std::size_t getTraceBufferCapacity();

// Records ENTER on construction and LEAVE on destruction in the calling thread's buffer. Without a
// frame, the scope belongs to the thread's latest frame (TraceState::frame; 0 before any record).
// If the ENTER is not written (see TraceBuffer::record()), neither is the LEAVE.
//
// A scope at a Location without a phase that opens inside an open DRAW span (a camera's drawing, a
// drawable's draw, an annotation) is also a KHR_debug group named by the Location, so GL tools
// (apitrace, RenderDoc, Nsight) show it where the profiler does. The open DRAW span means the
// thread is drawing with its context current; elsewhere (other phases, between frames, other
// threads) no context may be current, so no group is pushed.
class TraceScope {
public:
	TraceScope(
		const Location& location,
		const osg::Object* object,
		unsigned int frame,
		const osg::Camera* camera=nullptr
	);

	explicit TraceScope(const Location& location, const osg::Object* object=nullptr);

	~TraceScope();

	TraceScope(const TraceScope&) = delete;
	TraceScope& operator=(const TraceScope&) = delete;

	// The sequence number of the ENTER record, if it was written.
	std::optional<std::uint64_t> sequence() const { return _sequence; }

private:
	TraceBuffer& _buffer;
	const Location& _location;
	const osg::Object* _object;
	unsigned int _frame;
	const osg::Camera* _camera;
	std::optional<std::uint64_t> _sequence;
	bool _group = false;
};

// A TraceScope named `name` (a string literal) until the end of the enclosing block, at a Location
// without a phase declared on the spot (inside drawing, also a KHR_debug group; see TraceScope):
//
//   void Terrain::sortTiles() {
//       OSGX_TRACE_SCOPE("sort tiles");
//       ...
//   }
#define OSGX_TRACE_SCOPE(name) OSGX_TRACE_SCOPE_AT(name, __LINE__)
#define OSGX_TRACE_SCOPE_AT(name, line) OSGX_TRACE_SCOPE_IMPL(name, line)
#define OSGX_TRACE_SCOPE_IMPL(name, line) \
	static constexpr ::osgx::debug::Location osgxTraceLocation##line(name); \
	::osgx::debug::TraceScope osgxTraceScope##line(osgxTraceLocation##line)

}
