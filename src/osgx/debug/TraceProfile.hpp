#pragma once

#include "osgx/debug/Trace.hpp"

#include <array>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace osgx::debug {

// ================================================================================================
// The rolling profile
//
// TraceProfile::add() reads one finished frame from every TraceBuffer and folds it into one tree
// per Phase, in the order its records were written:
//
// - EVENT and UPDATE: the visitors' call trees, a node per visited object, nested as traversed.
// - CULL: grouped by camera, then the scene as the cull visitor traversed it; a nested (RTT)
//   camera is a node with its subtree beneath it.
// - DRAW: grouped by camera (the camera's own node is its drawing span, PRE_DRAW to FINAL_DRAW,
//   when recorded), a node per drawable in submission order (drawOrder), plus the other DRAW spans
//   (query readback, profile fold, buffer swap). Only drawables count as draws.
//
// Every CULL node of a drawable also receives that drawable's DRAW CPU and GPU time, matched by
// (camera, object), so the CULL tree doubles as the scene tree with draw costs on its leaves.
//
// Siblings are keyed by (camera, object, name): visiting one object twice under the same parent in
// a frame accumulates into one node (count, draws). Objects are never dereferenced; names and class
// names come from the records, camera names from the CULL records of Camera nodes or from the
// caller (cameras that are not scene nodes, such as a view's master camera).
//
// Unnamed plain osg::Group and osg::Geode spans (class name exactly "Group" or "Geode") are
// transparent: they get no node, their children become children of the enclosing node, and their
// own time counts as the enclosing node's self time. A culled one still gets a node.
//
// Values are moving averages over the frames a node appeared in; nodes not seen within the last
// `window` frames are dropped.
// ================================================================================================

struct TraceNode {
	Phase phase = Phase::UPDATE;
	const Location* location = nullptr;
	const osg::Camera* camera = nullptr;

	// Identity only, never dereferenced.
	const osg::Object* object = nullptr;

	std::string name;
	const char* className = nullptr;

	// What to display: the object's name; else, for the profiler's per-node spans and camera
	// groups, objectLabel() ("ClassName@0x<address>"); else the Location's name.
	std::string label;

	// Milliseconds per frame.
	double inclusiveMs = 0.0;
	double selfMs = 0.0;

	// CPU and GPU time of the drawable's draws: joined into CULL nodes, recorded by DRAW nodes.
	double drawMs = 0.0;
	double gpuMs = 0.0;

	// Per frame: ENTER/LEAVE spans merged into this node, and draws.
	double count = 0.0;
	double draws = 0.0;

	// From the latest frame the node appeared in: whether the cull visitor rejected it, and the
	// submission index of its first draw (-1 if not drawn).
	bool culled = false;
	int drawOrder = -1;

	// The latest frame the node appeared in.
	unsigned int frame = 0;

	std::list<TraceNode> children;
};

class TraceProfile {
public:
	using CameraNames = std::unordered_map<const osg::Camera*, std::string>;

	// Moving averages weigh the newest frame 2 / (window + 1).
	explicit TraceProfile(std::size_t window=60);
	~TraceProfile();

	TraceProfile(const TraceProfile&) = delete;
	TraceProfile& operator=(const TraceProfile&) = delete;

	// Folds `frame` in. Returns false (changing nothing) if no buffer holds it, or if any buffer
	// holding it has not closed it (a later frame has not started on that thread yet) or no longer
	// holds all of it.
	bool add(unsigned int frame, const CameraNames& cameraNames={});

	// A copy of the four phase trees, indexed by Phase.
	std::array<TraceNode, 4> snapshot() const;

	// The newest frame added, if any.
	std::optional<unsigned int> lastFrame() const;

private:
	struct Impl;

	mutable std::mutex _mutex;
	std::unique_ptr<Impl> _impl;
};

}
