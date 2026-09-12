#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/GLExtensions>
#include <osg/Image>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/Program>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgGA/GUIEventHandler>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <cstdint>

namespace osgx {

// ================================================================================================
// Texture-based object-ID picking; the caller is responsible for:
//
// 1. Creating a pick camera (RTT FBO, ABSOLUTE_RF, pick shader with OVERRIDE).
// 2. Sharing the scene node as a child of both the pick camera and the main scene root.
// 3. Syncing the pick camera's view/projection to the main camera each update traversal (PickCameraSync).
// 4. Adding PickHandler as a viewer event handler.
//
// See examples/osgx-picking.cpp for a complete worked example including all three FBO
// modes (full WxH, --small-pick N, --pick-1x1 sub-frustum).
//
// NOTE: every RTT camera that owns its own view/projection matrices MUST call
// `cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF)`. Without it the cull frustum is composed
// with the parent transform stack, all geometry is silently clipped, and the FBO renders only the
// clear color.
// ================================================================================================

// Creates a POST_RENDER FBO camera wired for object-ID picking.
//
// ABSOLUTE_RF is set so the camera uses its own view/projection matrices independently
// of where it sits in the scene graph - without it the cull frustum is wrong and all
// geometry is silently clipped.
//
// image=nullptr (ASYNC mode): attach a renderbuffer instead of an osg::Image so the
// camera renders into an FBO without triggering OSG's internal glReadPixels. The caller
// installs a postDrawCallback (e.g. AsyncReadback) to issue its own PBO-based readback.
//
// The pick shader is a hook-based design (two shader objects per stage: core declares the
// hook prototype, hook provides the definition), so picking behavior can be swapped without
// recompiling the core. vertHook/fragHook default to a no-op vertex hook and a
// "uniform uint pickID" fragment hook.
//
// The caller is responsible for:
//
// - addChild(scene) on the returned camera
// - syncing view/projection from the main camera each update traversal
// - installing a readback callback (update NodeCallback for SYNC; postDrawCallback for ASYNC)
//
// installProgram=false skips building/installing the generic pick Program (and ignores
// vertHook/fragHook - there is no core to attach them to; passing either logs a warning) while
// still setting up everything else (FBO, ABSOLUTE_RF, clear, viewport, the picking-correct
// BlendFunc/DITHER overrides). Use this when a subtree under the returned camera owns its own
// vertex-layout contract and needs to install its own Program (e.g. a wrapper Group with
// ON|OVERRIDE) - without this flag, this camera's own OVERRIDE Program wins regardless (OSG
// resolves OVERRIDE shallowest-wins), forcing the subtree to fight back with PROTECTED. Skipping
// installation here is the real fix; PROTECTED downstream is now only a workaround for callers
// that don't use this flag.
osg::ref_ptr<osg::Camera> makePickCamera(
	int w, int h,
	osg::Image* image = nullptr,
	bool installProgram = true,
	osg::Shader* vertHook = nullptr,
	osg::Shader* fragHook = nullptr
);

// ASYNC variant: attach a Texture2D for FBO rendering. OSG uses glFramebufferTexture2D
// (renders directly into the texture) with no automatic CPU readback. The caller installs
// a postDrawCallback that uses glGetTexImage + PBO for zero-stall async readback - valid
// after FBO unbind since glGetTexImage reads from the texture object, not the framebuffer.
// makePickCamera configures the texture's size, format, and NEAREST filters.
//
// installProgram - see the Image overload above; same meaning here.
osg::ref_ptr<osg::Camera> makePickCamera(
	int w, int h,
	osg::Texture2D* tex,
	bool installProgram = true,
	osg::Shader* vertHook = nullptr,
	osg::Shader* fragHook = nullptr
);

// Decode a 32-bit pick ID from an RGBA pixel: R=bits[7:0], G=bits[15:8], B=bits[23:16], A=bits[31:24].
inline uint32_t decodePickID(const uint8_t* px) {
	return uint32_t(px[0]) | (uint32_t(px[1]) << 8) | (uint32_t(px[2]) << 16) | (uint32_t(px[3]) << 24);
}

// A GLSL `vec4 osgx_encodePickID(uint id)` matching decodePickID()'s bit layout above, registered
// under the "osgx::picking" pragma namespace (`#pragma osgx::picking encode`) via
// osgx::registerShaderLibs - see Shader.hpp. Exists so a shader that CAN'T reuse
// makePickCamera()'s own PICK_FRAG_CORE/PICK_FRAG_HOOK_UNIFORM wholesale (e.g. one that has to run
// its own coverage/discard test before it knows the final ID) still packs the ID exactly the way
// this file's own decodePickID() expects it, from one shared implementation instead of a
// hand-copied one drifting in another repo.
void registerPickShaderLibs();

// Hands out contiguous pick-ID ranges instead of a single ID - for a drawable whose fragment
// shader distinguishes more than one pickable "part" per draw call (e.g. a multi-layer composite
// shape rendered in one draw call, where a per-vertex layer index picks out the part). ID 0 stays
// reserved for "nothing hit" (the pick camera's clear color), so the first real ID is 1.
//
// Not thread-safe - allocation happens at scene-build time on a single thread, same as the rest
// of scene construction; nothing here is touched during rendering or readback.
class PickIDAllocator {
public:
	// Reserves `count` (>= 1) contiguous IDs and returns the first one. A caller with multiple
	// parts per drawable adds its own per-part offset (e.g. a 0-based layer index) to this base
	// to get that part's final ID.
	uint32_t alloc(uint32_t count = 1) {
		const uint32_t base = _next;

		_next += count > 0 ? count : 1;

		return base;
	}

private:
	uint32_t _next{1};
};

// Selects one pick ID from a flat NxN RGBA pixel buffer.
// pixels - row-major RGBA, n*n pixels, Y=0 at bottom-left (OpenGL convention)
// n - side length of the region; n=1 is the degenerate single-pixel case
using PickRule = std::function<uint32_t(const uint8_t*, int)>;

// Center pixel wins - semantics identical to n=1; larger n widens the rasterized region.
uint32_t pickCenter(const uint8_t* px, int n);

// Most-covered non-zero ID wins - useful in dense or overlapping scenes.
uint32_t pickMostCoverage(const uint8_t* px, int n);

// Non-zero ID nearest to center wins - good for snap-to-object / hover picking.
uint32_t pickNearestToCenter(const uint8_t* px, int n);

// Spirals outward from center ring by ring (Chebyshev distance), returning the first
// non-zero ID found. Equivalent result to pickNearestToCenter but exits on the first
// hit instead of scanning all N*N pixels - preferred default for all pick modes.
uint32_t spiralPick(const uint8_t* px, int n);

enum class ActionType { HOVER, CLICK };

// Shared state for SYNC and ASYNC pick readback variants.
//
// `virtual osg::Object` - NOT `virtual osg::Referenced`: OSG's own diamond-merge point for
// this exact multiple-inheritance shape is osg::Callback : public virtual Object (see
// osg/Callback) - osg::Object : public Referenced is a PLAIN, non-virtual edge one level
// further down. PickReadbackSync/Async each inherit this ALONGSIDE a second callback base
// (NodeCallback or Camera::DrawCallback) that reaches that SAME shared virtual Object through
// its own Callback base. Virtually inheriting Referenced directly here would NOT merge with
// that - it'd add a third, unrelated path to Referenced, which is ambiguous rather than
// shared (verified: it fails to compile with "virtual base osg::Referenced inaccessible ...
// due to ambiguity"). Virtually inheriting Object at the SAME level Callback does is what
// actually collapses the diamond into one shared subobject. Concrete classes still provide
// their OWN callback behavior (operator()) - this mixin only owns the shared onPick/onEnter/
// onLeave/mouse-position/lastID state.
//
// onPick(id, ActionType) - HOVER fires when hovered ID changes (including to 0=background);
//                           CLICK fires when a click resolves.
//                           May fire on any thread depending on readback mode - safe for
//                           logging/audio/non-scene-graph reactions only.
// onEnter(id)            - non-zero id entered. Always fired from the update thread via
// onLeave(id)               PickHoverCallback - safe for scene graph modifications.
// reportClick()          - call from PickHandler in CONTINUOUS mode to fire CLICK with the
//                           currently hovered ID.
struct PickReadback: public virtual osg::Object {
	std::function<void(uint32_t, ActionType)> onPick;
	std::function<void(uint32_t)> onEnter;
	std::function<void(uint32_t)> onLeave;

	void requestPick(int x, int y);
	void updateMouse(int x, int y);
	void reportClick();

	// Forces the tracked pick state back to "nothing hovered" (id 0) - both lastID() and the
	// internal onPick-hover dedup state. Meant for invalidating from OUTSIDE the pick camera's
	// own knowledge: the pick camera/sub-frustum only ever updates on a MOVE event, so if the
	// mouse leaves the window entirely (no GUIEventAdapter event fires for that at all - see
	// osgx::platform::isCursorInWindow()'s own comment), the last-hovered ID stays reported as
	// hovered forever, with the cursor nowhere near the window.
	//
	// PickCameraSync already calls this automatically every frame the cursor is outside the
	// window (when built with OSGX_PLATFORM) - nothing else needs to call it in the common
	// case. Public for callers driving picking without a PickCameraSync, or wanting to
	// invalidate for an unrelated reason (e.g. picking should pause while some other UI has
	// mouse capture). PickHoverCallback's own next poll of lastID() fires onLeave naturally
	// after this, exactly as if the pick camera itself had detected the transition. A no-op if
	// nothing was hovered.
	void invalidate() {
		_lastID.store(0, std::memory_order_release);
		_prevID = 0;
	}

	int mouseX() const { return _x.load(std::memory_order_relaxed); }
	int mouseY() const { return _y.load(std::memory_order_relaxed); }

	// Set by PickHandler from GUIEventAdapter::getHandled() - true while some earlier handler
	// (e.g. osgx::imgui::Widget) already claimed the current mouse event, meaning mouseX()/
	// mouseY() are frozen at whatever they were the last time a REAL 3D-viewport event updated
	// them (see PickHandler::handle()'s own comment for why they're frozen rather than tracking
	// the cursor's true, now-over-some-other-UI position). A one-shot invalidate() at the moment
	// this becomes true isn't enough on its own: PickCameraSync's sub-frustum keeps re-aiming at
	// that same frozen-but-still-valid position every frame regardless of events, so the
	// continuous readback just re-derives the same hover a frame later. PickCameraSync checks
	// this every update traversal and calls invalidate() continuously while set, the same way it
	// already does for "cursor left the window" (isCursorInWindow()) - see its own comment.
	void setSuspended(bool suspended) const { _suspended.store(suspended, std::memory_order_relaxed); }
	bool isSuspended() const { return _suspended.load(std::memory_order_relaxed); }

	// PickCameraSync refreshes these from the live viewer-camera viewport every update
	// traversal. Readbacks that convert window coordinates to a reduced RTT target override
	// them; 1x1 continuous pickers need no extra state.
	//
	// setWindowOrigin exists because mouseX()/mouseY() (via requestPick()/updateMouse(), fed
	// by PickHandler from raw GUIEventAdapter coordinates) are WINDOW-absolute, but every pick
	// target (the pick camera's own viewport, a reduced RTT image, ...) is sized and addressed
	// relative to the MAIN viewer camera's viewport - which is only the whole window when that
	// viewport's origin is (0, 0). An app that gives part of the window to something else (an
	// ImGui dock, a split view, ...) and confines the 3D camera's own viewport to the rest needs
	// this correction or every pick target is silently offset by exactly that viewport's origin.
	virtual void setWindowSize(int, int) {}
	virtual void setWindowOrigin(int, int) {}
	uint32_t lastID() const { return _lastID.load(std::memory_order_acquire); }

protected:
	void _fireHover(uint32_t id) const;
	void _fireClick(uint32_t id) const;

	mutable std::atomic<int> _x{0}, _y{0};
	mutable std::atomic<bool> _requested{false};
	mutable std::atomic<bool> _suspended{false};
	mutable std::atomic<uint32_t> _lastID{0};
	mutable uint32_t _prevID{0};
};

// SYNC readback: NodeCallback that reads from an osg::Image attached to the pick camera.
//
// OSG reads the FBO into the image inside RenderStage::drawImplementation while the FBO is
// still bound. We sample image->data() one frame later in the update traversal - invisible
// latency for click-only or continuous hover picking.
class PickReadbackSync: public PickReadback, public osg::NodeCallback {
public:
	enum class Mode { CLICK, CONTINUOUS };

	// winW / winH - initial window dimensions. PickCameraSync refreshes them
	// from its viewer camera's live viewport, so reduced image targets keep
	// mapping mouse coordinates correctly after a resize.
	PickReadbackSync(
		int pickSize,
		PickRule rule,
		osg::Image* image,
		int winW,
		int winH,
		Mode mode = Mode::CLICK
	):
	_pickSize(pickSize),
	_rule(std::move(rule)),
	_winW(winW),
	_winH(winH),
	_mode(mode),
	_image(image) {}

	void setWindowSize(int width, int height) override {
		_winW = width;
		_winH = height;
	}

	void setWindowOrigin(int x, int y) override {
		_winX = x;
		_winY = y;
	}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	int _pickSize;
	PickRule _rule;
	int _winW, _winH;
	int _winX{0}, _winY{0};
	Mode _mode;
	osg::ref_ptr<osg::Image> _image;
};

// ASYNC readback: Camera::DrawCallback using a Texture2D attachment + PBO glGetTexImage.
// Install as postDrawCallback on the pick camera.
//
// The pick camera must attach a Texture2D (not osg::Image) so OSG renders directly into
// the texture with no automatic CPU readback. glGetTexImage reads from the texture object --
// FBO binding is irrelevant after unbind - so the postDrawCallback timing is correct.
// TODO: This is only PBO-delayed readback, not strictly non-stalling async: the next-frame
// glMapBuffer() can still block if the transfer has not completed. Fix by adding a fence
// (glFenceSync + zero-timeout glClientWaitSync polling) or a deeper PBO ring before mapping.
//
// CLICK mode: one async download per requestPick() call.
// CONTINUOUS mode: download every frame (negligible for a 1x1 texture).
class PickReadbackAsync: public PickReadback, public osg::Camera::DrawCallback {
public:
	enum class Mode { CLICK, CONTINUOUS };

	PickReadbackAsync(osg::Texture2D* tex, int imgW, int imgH, Mode mode = Mode::CLICK):
	_tex(tex), _imgW(imgW), _imgH(imgH), _mode(mode) {}

	void operator()(osg::RenderInfo& ri) const override;

private:
	osg::ref_ptr<osg::Texture2D> _tex;
	int _imgW, _imgH;
	Mode _mode;

	mutable GLuint _pbo{0};
	mutable bool _init{false};
	mutable bool _inFlight{false};
	mutable int _pickX{0}, _pickY{0};
};

// NodeCallback that syncs a pick camera's view/projection from the viewer camera each
// update traversal. Install directly on the pick camera via setUpdateCallback(); chain
// other pick callbacks (e.g. PickReadbackSync) via setNestedCallback().
//
// When built with OSGX_PLATFORM: also transparently invalidates rb every frame the cursor is
// outside the window (platform::isCursorInWindow()) - see PickReadback::invalidate()'s own
// comment for why that's needed at all. No caller opt-in; a safe no-op on a non-X11
// GraphicsContext or a non-OSGX_PLATFORM build.
//
// pick1x1=true: also builds a sub-frustum projection centered on the cursor each frame
// (gluPickMatrix equivalent). The live viewer-camera viewport supplies its dimensions AND
// origin (via rb->setWindowSize()/setWindowOrigin()), so the cursor projection stays correct
// after a window resize AND when that viewport doesn't start at (0, 0) - e.g. an app that
// confines the 3D camera to part of the window (a docked UI panel taking the rest) needs the
// origin correction or picking is silently offset by exactly that viewport's own (x, y).
// Requires rb for the current mouse position.
class PickCameraSync: public osg::NodeCallback {
public:
	PickCameraSync(
		osg::Camera* viewerCam,
		bool pick1x1 = false,
		int W = 0,
		int H = 0,
		PickReadback* rb = nullptr
	):
	_viewerCam(viewerCam),
	_pick1x1(pick1x1),
	_W(W),
	_H(H),
	_rb(rb) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	osg::observer_ptr<osg::Camera> _viewerCam;
	bool _pick1x1;
	int _W, _H;
	PickReadback* _rb;
};

// NodeCallback that fires onEnter/onLeave on the update thread by polling lastID().
//
// Install on any node in the update traversal alongside or instead of PickReadbackSync.
// The readback (SYNC or ASYNC) updates _lastID atomically; this callback detects transitions
// and fires callbacks safely regardless of which thread the readback runs on.
//
// This is the correct way to trigger scene graph modifications (setMatrix, setColor, etc.)
// in response to hover events - onEnter/onLeave fired directly from PickReadbackAsync's
// draw callback would race with the cull thread.
class PickHoverCallback: public osg::NodeCallback {
public:
	explicit PickHoverCallback(PickReadback* rb): _rb(rb) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	PickReadback* _rb;
	uint32_t _prevID{0};
};

// GUIEventHandler that forwards click/move events to any PickReadback variant.
// continuous=false - left-click calls requestPick(x, y); use with CLICK mode.
// continuous=true - MOVE events update cursor position; left-click queries lastID().
// Use with CONTINUOUS mode (1x1 sub-frustum picking).
//
// consumeEvents=true - returns true on left-click, stopping OSG's handler chain.
// Use when picking should be exclusive (e.g. object selection that must not also
// rotate the camera). Default false (additive - pick and camera manipulator both
// receive the click).
class PickHandler: public osgGA::GUIEventHandler {
public:
	// rb is non-owning; kept alive by the camera's callback ref for its lifetime.
	explicit PickHandler(PickReadback* rb, bool continuous=false, bool consumeEvents=false):
	_rb(rb),
	_continuous(continuous),
	_consume(consumeEvents) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override;

private:
	PickReadback* _rb;

	bool _continuous;
	bool _consume;
};

}
