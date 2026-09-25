#include "osgx-python.hpp"
#include "osgx/Picking.hpp"

namespace osgx_python {

namespace {

// PickRule is std::function<uint32_t(const uint8_t*, int)> on the C++ side - there's no
// automatic pybind11 caster for a raw `const uint8_t*` argument, so Python can't author a new
// PickRule as an arbitrary callable and hand it to PickReadbackSync's constructor the way C++
// can. Instead the constructor takes this enum and resolves it to the matching built-in free
// function internally. The four rules are ALSO exposed standalone below (operating on a
// py::buffer) so they're directly callable/testable from Python on their own.
enum class PickRuleKind { CENTER, MOST_COVERAGE, NEAREST_TO_CENTER, SPIRAL };

osgx::PickRule toPickRule(PickRuleKind kind) {
	switch(kind) {
		case PickRuleKind::CENTER: return osgx::pickCenter;
		case PickRuleKind::MOST_COVERAGE: return osgx::pickMostCoverage;
		case PickRuleKind::NEAREST_TO_CENTER: return osgx::pickNearestToCenter;
		case PickRuleKind::SPIRAL: default: return osgx::spiralPick;
	}
}

// Validates and unwraps a py::buffer for the N*N RGBA pick-rule functions below.
const uint8_t* pickBufferPtr(const py::buffer& region, int n) {
	py::buffer_info info = region.request();
	auto totalBytes = static_cast<py::ssize_t>(info.size) * info.itemsize;
	auto needed = static_cast<py::ssize_t>(n) * n * 4;

	if(totalBytes < needed) {
		throw std::runtime_error(
			"pick region buffer too small: need " + std::to_string(needed) +
			" bytes for n=" + std::to_string(n) + ", got " + std::to_string(totalBytes)
		);
	}

	return reinterpret_cast<const uint8_t*>(info.ptr);
}

}

// osgx::picking - texture-based object-ID picking (RTT FBO + pick shader). See
// osgx/Picking.hpp and examples/osgx-{picking,hover}.cpp for the full worked patterns this
// mirrors; PickReadback itself (shared onPick/onEnter/onLeave state) is registered here purely
// so PickCameraSync/PickHoverCallback/PickHandler below can accept either PickReadbackSync or
// PickReadbackAsync through one shared `rb` parameter - it has no public constructor exposed,
// Python never instantiates it directly.
void bind_picking(py::module_& m) {
	py::enum_<PickRuleKind>(
		m,
		"PickRule",
		"Selects which built-in rule PickReadbackSync/PickReadbackAsync resolve an N*N pick "
		"region down to a single ID with; see the standalone pickCenter/pickMostCoverage/"
		"pickNearestToCenter/spiralPick functions for the same rules callable directly."
	)
		.value("CENTER", PickRuleKind::CENTER)
		.value("MOST_COVERAGE", PickRuleKind::MOST_COVERAGE)
		.value("NEAREST_TO_CENTER", PickRuleKind::NEAREST_TO_CENTER)
		.value("SPIRAL", PickRuleKind::SPIRAL)
		.export_values()
	;

	m.def(
		"decodePickID",
		[](py::buffer px) { return osgx::decodePickID(pickBufferPtr(px, 1)); },
		"pixel"_a,
		"Decode a 32-bit pick ID from a 4-byte RGBA buffer: "
		"R=bits[7:0], G=bits[15:8], B=bits[23:16], A=bits[31:24]."
	);

	py::class_<osgx::PickIDAllocator>(
		m,
		"PickIDAllocator",
		"Hands out contiguous pick-ID ranges for a drawable whose fragment shader distinguishes "
		"more than one pickable 'part' per draw call. ID 0 stays reserved for 'nothing hit', so "
		"the first real ID is 1. Not thread-safe - allocate at scene-build time on a single "
		"thread, same as the rest of scene construction."
	)
		.def(py::init<>())
		.def(
			"alloc", &osgx::PickIDAllocator::alloc,
			"count"_a = 1,
			"Reserves `count` (>= 1) contiguous IDs and returns the first one. A caller with "
			"multiple parts per drawable adds its own per-part offset (e.g. a 0-based layer "
			"index) to this base to get that part's final ID."
		)
	;

	m.def(
		"pickCenter",
		[](py::buffer region, int n) { return osgx::pickCenter(pickBufferPtr(region, n), n); },
		"region"_a, "n"_a,
		"Center pixel wins - semantics identical to n=1; larger n widens the rasterized region. "
		"`region` is a row-major RGBA buffer, n*n pixels, Y=0 at bottom-left (OpenGL convention)."
	);

	m.def(
		"pickMostCoverage",
		[](py::buffer region, int n) { return osgx::pickMostCoverage(pickBufferPtr(region, n), n); },
		"region"_a, "n"_a,
		"Most-covered non-zero ID wins - useful in dense or overlapping scenes."
	);

	m.def(
		"pickNearestToCenter",
		[](py::buffer region, int n) { return osgx::pickNearestToCenter(pickBufferPtr(region, n), n); },
		"region"_a, "n"_a,
		"Non-zero ID nearest to center wins - good for snap-to-object / hover picking."
	);

	m.def(
		"spiralPick",
		[](py::buffer region, int n) { return osgx::spiralPick(pickBufferPtr(region, n), n); },
		"region"_a, "n"_a,
		"Spirals outward from center ring by ring (Chebyshev distance), returning the first "
		"non-zero ID found. Equivalent result to pickNearestToCenter but exits on the first hit - "
		"preferred default for all pick modes."
	);

	py::enum_<osgx::ActionType>(
		m,
		"ActionType",
		"Which kind of pick event fired: HOVER on a change to the hovered ID (including to "
		"0=background), CLICK when a click resolves. Passed to PickReadback.onPick."
	)
		.value("HOVER", osgx::ActionType::HOVER)
		.value("CLICK", osgx::ActionType::CLICK)
		.export_values()
	;

	py::class_<
		osgx::PickReadback,
		osg::Object,
		osg::ref_ptr<osgx::PickReadback>
	> pickReadback(
		m,
		"PickReadback",
		"Shared onPick/onEnter/onLeave state and thread-safe request/update methods common to "
		"PickReadbackSync and PickReadbackAsync. Has no public constructor - Python never "
		"instantiates this directly, only the two concrete readback subclasses."
	);

	pickReadback
		.def_readwrite(
			"onPick", &osgx::PickReadback::onPick,
			"fn(id: int, action: ActionType) - HOVER fires when the hovered ID changes (including "
			"to 0=background); CLICK fires when a click resolves. May fire on any thread depending "
			"on readback mode - safe for logging/audio/non-scene-graph reactions only."
		)
		.def_readwrite(
			"onEnter", &osgx::PickReadback::onEnter,
			"fn(id: int) - non-zero id entered. Always fired from the update thread via "
			"PickHoverCallback - safe for scene graph modifications."
		)
		.def_readwrite(
			"onLeave", &osgx::PickReadback::onLeave,
			"fn(id: int) - companion to onEnter, same thread guarantee."
		)
		.def(
			"requestPick", &osgx::PickReadback::requestPick, "x"_a, "y"_a,
			"Queue a CLICK-mode pick at window coordinates (x, y)."
		)
		.def(
			"updateMouse", &osgx::PickReadback::updateMouse, "x"_a, "y"_a,
			"Update the tracked cursor position for CONTINUOUS-mode hover picking."
		)
		.def(
			"reportClick", &osgx::PickReadback::reportClick,
			"Fire CLICK with the currently hovered ID - call from a click handler in CONTINUOUS mode."
		)
		.def(
			"invalidate", &osgx::PickReadback::invalidate,
			"Force the tracked pick state back to 'nothing hovered' (id 0), so PickHoverCallback's "
			"next poll fires onLeave naturally. Call once per frame the cursor is confirmed outside "
			"the window (osgx.platform.isCursorInWindow()) - there is no window-leave event to "
			"drive this automatically."
		)
		.def_property_readonly(
			"mouseX", &osgx::PickReadback::mouseX,
			"The most recently tracked window-coordinate cursor X, from requestPick()/updateMouse()."
		)
		.def_property_readonly(
			"mouseY", &osgx::PickReadback::mouseY,
			"The most recently tracked window-coordinate cursor Y, from requestPick()/updateMouse()."
		)
		.def_property_readonly(
			"lastID", &osgx::PickReadback::lastID,
			"The most recently resolved pick ID (0 = background/nothing hovered)."
		)
	;

	// SYNC and ASYNC each pull in a second base (osg::NodeCallback / osg::Camera::DrawCallback)
	// alongside PickReadback - true multiple inheritance from pybind11's point of view (hence
	// py::multiple_inheritance() on both), even though PickReadback's `virtual osg::Object` base
	// means there's only one shared Object/Referenced subobject underneath (see PickReadback's
	// own comment in osgx/Picking.hpp). This is what lets PickCameraSync/PickHoverCallback/
	// PickHandler below accept a PickReadbackSync or PickReadbackAsync Python object directly
	// wherever the C++ signature just says `PickReadback*`.
	py::class_<
		osgx::PickReadbackSync,
		osgx::PickReadback,
		osg::NodeCallback,
		osg::ref_ptr<osgx::PickReadbackSync>
	> pickReadbackSync(
		m,
		"PickReadbackSync",
		"SYNC pick-ID readback: samples an osg.Image attached to the pick camera one frame after "
		"OSG's own internal readback fills it. Install as the pick camera's update callback (or "
		"chained behind PickCameraSync).",
		py::multiple_inheritance()
	);

	py::enum_<osgx::PickReadbackSync::Mode>(
		pickReadbackSync,
		"Mode",
		"CLICK: readback resolves only in response to requestPick()/reportClick(). CONTINUOUS: "
		"1x1 sub-frustum hover - readback resolves every frame, driving onEnter/onLeave."
	)
		.value("CLICK", osgx::PickReadbackSync::Mode::CLICK)
		.value("CONTINUOUS", osgx::PickReadbackSync::Mode::CONTINUOUS)
		.export_values()
	;

	pickReadbackSync.def(
		py::init([](
			int pickSize,
			osg::Image* image,
			int winW,
			int winH,
			PickRuleKind rule,
			osgx::PickReadbackSync::Mode mode
		) {
			return new osgx::PickReadbackSync(pickSize, toPickRule(rule), image, winW, winH, mode);
		}),
		"pickSize"_a,
		"image"_a,
		"winW"_a,
		"winH"_a,
		"rule"_a = PickRuleKind::SPIRAL,
		"mode"_a = osgx::PickReadbackSync::Mode::CLICK,
		py::keep_alive<1, 3>(),
		"SYNC readback: reads from an osg::Image attached to the pick camera one frame after OSG's "
		"own internal glReadPixels. winW/winH are initial window dimensions; PickCameraSync refreshes "
		"them from its viewer camera's viewport to scale reduced-image mouse coordinates after resize."
	);

	py::class_<
		osgx::PickReadbackAsync,
		osgx::PickReadback,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgx::PickReadbackAsync>
	> pickReadbackAsync(
		m,
		"PickReadbackAsync",
		"ASYNC pick-ID readback: Texture2D attachment + PBO glGetTexImage, one frame of lag. "
		"Install as the pick camera's postDrawCallback.",
		py::multiple_inheritance()
	);

	py::enum_<osgx::PickReadbackAsync::Mode>(
		pickReadbackAsync,
		"Mode",
		"CLICK: PBO download happens once per requestPick(). CONTINUOUS: downloads every frame "
		"(negligible cost for a 1x1 texture), driving onEnter/onLeave."
	)
		.value("CLICK", osgx::PickReadbackAsync::Mode::CLICK)
		.value("CONTINUOUS", osgx::PickReadbackAsync::Mode::CONTINUOUS)
		.export_values()
	;

	pickReadbackAsync.def(
		py::init<osg::Texture2D*, int, int, osgx::PickReadbackAsync::Mode>(),
		"tex"_a,
		"imgW"_a,
		"imgH"_a,
		"mode"_a = osgx::PickReadbackAsync::Mode::CLICK,
		py::keep_alive<1, 2>(),
		"ASYNC readback: Texture2D attachment + PBO glGetTexImage, installed as a postDrawCallback "
		"on the pick camera. CLICK mode downloads once per requestPick(); CONTINUOUS downloads every "
		"frame (negligible for a 1x1 texture)."
	);

	py::class_<
		osgx::PickCameraSync,
		osg::NodeCallback,
		osg::ref_ptr<osgx::PickCameraSync>
	>(
		m,
		"PickCameraSync",
		"Keeps a pick camera's view/projection matched to the main viewer camera every update "
		"traversal - the mechanism that actually keeps the pick camera aimed correctly."
	)
		.def(
			py::init<osg::Camera*, bool, int, int, osgx::PickReadback*>(),
			"viewerCam"_a,
			"pick1x1"_a = false,
			"W"_a = 0,
			"H"_a = 0,
			"rb"_a = nullptr,
			py::keep_alive<1, 6>(),
			"Install via setUpdateCallback() on the pick camera; syncs its view/projection from "
			"viewerCam every update traversal. pick1x1=True also builds a sub-frustum projection "
			"centered on the cursor each frame (gluPickMatrix equivalent) - requires rb for the "
			"current mouse position. Chain other pick callbacks via setNestedCallback()."
		)
	;

	py::class_<
		osgx::PickHoverCallback,
		osg::NodeCallback,
		osg::ref_ptr<osgx::PickHoverCallback>
	>(
		m,
		"PickHoverCallback",
		"Polls a PickReadback's last ID on the update thread and fires onEnter/onLeave on "
		"transitions - the correct way to trigger scene-graph mutation from a hover event."
	)
		.def(
			py::init<osgx::PickReadback*>(),
			"rb"_a,
			py::keep_alive<1, 2>(),
			"Fires onEnter/onLeave on the update thread by polling rb.lastID() - the correct way to "
			"trigger scene graph modifications in response to hover events, regardless of which "
			"thread the readback itself runs on."
		)
	;

	py::class_<
		osgx::PickHandler,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::PickHandler>
	>(
		m,
		"PickHandler",
		"Routes click/move GUI events to a PickReadback's requestPick()/updateMouse()/reportClick()."
	)
		.def(
			py::init<osgx::PickReadback*, bool, bool>(),
			"rb"_a,
			"continuous"_a = false,
			"consumeEvents"_a = false,
			py::keep_alive<1, 2>(),
			"Forwards click/move events to rb. continuous=False: left-click calls rb.requestPick(x, "
			"y) (use with CLICK mode). continuous=True: MOVE events update cursor position, left-"
			"click calls rb.reportClick() (use with CONTINUOUS/1x1 sub-frustum picking). "
			"consumeEvents=True stops OSG's handler chain on left-click - use when picking must be "
			"exclusive (e.g. selection that shouldn't also rotate the camera)."
		)
	;

	m.def(
		"makePickCamera",
		py::overload_cast<
			int,
			int,
			osg::Image*,
			bool,
			osg::Shader*,
			osg::Shader*
		>(&osgx::makePickCamera),
		"w"_a, "h"_a, "image"_a = nullptr, "installProgram"_a = true,
		"vertHook"_a = nullptr, "fragHook"_a = nullptr,
		"SYNC-friendly variant: attach an osg::Image (or leave it None for an ASYNC-style "
		"renderbuffer-only camera with no automatic CPU readback - install your own PBO readback "
		"via a postDrawCallback). vertHook/fragHook default to a no-op vertex hook and a "
		"'uniform uint pickID' fragment hook (see osgx/Picking.hpp's hook-based shader design). "
		"installProgram=False skips building/installing the generic pick Program entirely (and "
		"ignores vertHook/fragHook) - use when a subtree under the returned camera installs its "
		"own Program and would otherwise have to fight this camera's OVERRIDE with PROTECTED "
		"(OSG resolves OVERRIDE shallowest-wins). Caller is responsible for addChild(scene), "
		"syncing view/projection each update traversal, and installing a readback callback."
	);

	m.def(
		"makePickCamera",
		py::overload_cast<
			int,
			int,
			osg::Texture2D*,
			bool,
			osg::Shader*,
			osg::Shader*
		>(&osgx::makePickCamera),
		"w"_a, "h"_a, "tex"_a, "installProgram"_a = true,
		"vertHook"_a = nullptr, "fragHook"_a = nullptr,
		"ASYNC variant: attaches `tex` for FBO rendering via glFramebufferTexture2D, no automatic "
		"CPU readback. Pair with PickReadbackAsync as the pick camera's postDrawCallback. "
		"installProgram - see the Image overload above; same meaning here."
	);
}

}
