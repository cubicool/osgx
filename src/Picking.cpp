#include "osgx/Picking.hpp"
#include "osgx/RTT.hpp"
#include "osgx/Shader.hpp"

#ifdef OSGX_PLATFORM
#include "osgx/Linux.hpp"
#endif

#include <osg/Notify>

#include <cstring>

namespace osgx {

namespace {

// ------------------------------------------------------------------------------------------------
// Pick shader strings - hook-based design, multi-shader-object linking
//
// Two shader objects per stage: core declares the hook prototype, hook provides the definition.
// Swap only the hook to change picking behavior without recompiling the core.
//
// pickVertexHook() - end of vertex stage; forward per-vertex attributes to the frag stage.
// Default: no-op (PICK_VERT_HOOK_NOOP).
//
// getPickID() - fragment stage; return the pick ID for this fragment.
// Default: reads uniform uint pickID (PICK_FRAG_HOOK_UNIFORM).
//
// makePickCamera() assembles these into a program and installs it with OVERRIDE, unless
// installProgram=false - see its own doc comment in osgx/Picking.hpp.
// ------------------------------------------------------------------------------------------------

// Core: declares the hook prototypes and provides the main() implementations.
constexpr const char* PICK_VERT_CORE = R"GLSL(
#version 430 core
in vec4 osg_Vertex;
uniform mat4 osg_ModelViewProjectionMatrix;
void pickVertexHook();
void main() {
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
	pickVertexHook();
}
)GLSL";

constexpr const char* PICK_FRAG_CORE = R"GLSL(
#version 430 core
out vec4 fragColor;
uint getPickID();
void main() {
	uint id = getPickID();
	fragColor = vec4(
		float( id & 0xFFu) / 255.0,
		float((id >> 8u) & 0xFFu) / 255.0,
		float((id >> 16u) & 0xFFu) / 255.0,
		float((id >> 24u) & 0xFFu) / 255.0
	);
}
)GLSL";

// Default hooks: no vertex forwarding; fragment reads uniform uint pickID.
//
// Each osg::Shader source is compiled as its OWN translation unit (glCompileShader runs on
// every shader object individually, before linking) - GLSL requires #version to be the first
// token if present at all, and defaults to 1.10 when absent. PICK_VERT_CORE/PICK_FRAG_CORE
// above declare #version 430 core, but these hook strings didn't, so `uint` (GLSL 1.30+) failed
// to compile despite the core shaders being fine - driver-dependent whether that's enforced
// per-object or only at link time, which is why this didn't necessarily fail everywhere.
constexpr const char* PICK_VERT_HOOK_NOOP = R"GLSL(
#version 430 core
void pickVertexHook() {}
)GLSL";

constexpr const char* PICK_FRAG_HOOK_UNIFORM = R"GLSL(
#version 430 core
uniform uint pickID;
uint getPickID() { return pickID; }
)GLSL";

// See registerPickShaderLibs()/Picking.hpp's own comment: a `vec4 osgx_encodePickID(uint id)`
// matching decodePickID()'s bit layout, published under the "osgx::picking" pragma namespace for
// a shader elsewhere (e.g. osgSlug's coverage-aware pick fragment) that can't reuse
// PICK_FRAG_CORE/PICK_FRAG_HOOK_UNIFORM above wholesale. Not used by makePickCamera() itself --
// its own inline encode in PICK_FRAG_CORE is already working code, left untouched.
constexpr const char* PICK_ENCODE_SRC = R"GLSL(
vec4 osgx_encodePickID(uint id) {
	return vec4(
		float( id & 0xFFu) / 255.0,
		float((id >> 8u) & 0xFFu) / 255.0,
		float((id >> 16u) & 0xFFu) / 255.0,
		float((id >> 24u) & 0xFFu) / 255.0
	);
}
)GLSL";

}

void registerPickShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"encode", "osgx_encodePickID", PICK_ENCODE_SRC}
	};

	::osgx::registerShaderLibs("osgx::picking", libs);
}

osg::ref_ptr<osg::Camera> makePickCamera(
	int w, int h,
	osg::Image* image,
	bool installProgram,
	osg::Shader* vertHook,
	osg::Shader* fragHook
) {
	auto cam = make_nref<RTT>("PickCamera", w, h);
	// Overrides RTT's PRE_RENDER default - this pass deliberately runs POST_RENDER (see
	// CLAUDE.md's osgx-picking writeup for why). ABSOLUTE_RF (the sharp edge the comment below
	// used to carry alone) is already RTT's own default, same reasoning as ever: without it the
	// camera composes view/projection with the parent transform stack, producing a wrong cull
	// frustum that clips all geometry.
	cam->setRenderOrder(osg::Camera::POST_RENDER);
	cam->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	cam->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // all-zero RGBA = ID 0 = no pick
	cam->setSmallFeatureCullingPixelSize(-1.0f);
	// Implicit renderbuffer - no readback needed, this only exists so overlapping pickable
	// geometry at different depths resolves nearest-wins during THIS pass instead of last-
	// drawn-wins. setClearMask() above already asked for GL_DEPTH_BUFFER_BIT; without an actual
	// attachment here that clear (and any depth test) is a silent no-op against a buffer that
	// was never created.
	cam->attach(osg::Camera::DEPTH_BUFFER, GL_DEPTH_COMPONENT24);

	if(image) {
		cam->attach(osg::Camera::COLOR_BUFFER, image);

		// CONTINUOUS/hover-mode readbacks (PickReadbackSync) poll this image every update
		// traversal, including before the pick camera has ever actually rendered - an
		// already-allocated image's memory is whatever allocateImage() left it as (garbage,
		// not zero), which decodes as a bogus nonzero pick ID and fires a spurious onEnter.
		// Zero it here, once, so ID 0 (background) holds until the first real render --
		// matching the camera's own all-zero clear color - instead of relying on every
		// caller to memset() it themselves (found independently in both the C++ and Python
		// hover examples).
		if(image->valid() && image->data()) {
			std::memset(image->data(), 0, image->getTotalSizeInBytesIncludingMipmaps());
		}
	}
	else cam->attach(osg::Camera::COLOR_BUFFER, GL_RGBA);

	if(!installProgram && (vertHook || fragHook)) {
		osg::notify(osg::WARN) <<
			"osgx::makePickCamera: vertHook/fragHook ignored - installProgram=false means no "
			"core program exists for them to attach to" << std::endl;
	}

	auto* ss = cam->getOrCreateStateSet();

	if(installProgram) {
		auto prog = make_nref<osg::Program>("pickProgram");

		auto* vc = new osg::Shader(osg::Shader::VERTEX, PICK_VERT_CORE);
		auto* vh = vertHook ? vertHook : new osg::Shader(osg::Shader::VERTEX, PICK_VERT_HOOK_NOOP);
		auto* fc = new osg::Shader(osg::Shader::FRAGMENT, PICK_FRAG_CORE);
		auto* fh = fragHook ? fragHook : new osg::Shader(osg::Shader::FRAGMENT, PICK_FRAG_HOOK_UNIFORM);

		vc->setName("pickVertCore"); vh->setName("pickVertHook");
		fc->setName("pickFragCore"); fh->setName("pickFragHook");

		prog->addShader(vc); prog->addShader(vh);
		prog->addShader(fc); prog->addShader(fh);

		ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
	}

	// BlendFunc(ONE,ZERO) with PROTECTED: osg::Text re-enables blend without
	// respecting OVERRIDE alone; PROTECTED prevents any child from overriding it. Kept
	// regardless of installProgram - correct pick-buffer semantics (opaque write, no
	// blending) apply no matter whose Program is actually running.
	auto* bf = new osg::BlendFunc(
		osg::BlendFunc::ONE, osg::BlendFunc::ZERO,
		osg::BlendFunc::ONE, osg::BlendFunc::ZERO
	);

	ss->setMode(GL_DITHER, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	ss->setAttributeAndModes(
		bf,
		osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED
	);

	return cam;
}

osg::ref_ptr<osg::Camera> makePickCamera(
	int w, int h,
	osg::Texture2D* tex,
	bool installProgram,
	osg::Shader* vertHook,
	osg::Shader* fragHook
) {
	tex->setTextureSize(w, h);
	tex->setInternalFormat(GL_RGBA);
	tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

	auto cam = make_nref<RTT>("PickCamera", w, h);
	// Overrides RTT's PRE_RENDER default - see the Image overload's identical override above.
	cam->setRenderOrder(osg::Camera::POST_RENDER);
	cam->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	cam->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f)); // all-zero RGBA = ID 0 = no pick
	cam->setSmallFeatureCullingPixelSize(-1.0f);
	cam->attach(osg::Camera::COLOR_BUFFER, tex);
	// See the Image overload's identical attachment above for why this is needed.
	cam->attach(osg::Camera::DEPTH_BUFFER, GL_DEPTH_COMPONENT24);

	if(!installProgram && (vertHook || fragHook)) {
		osg::notify(osg::WARN) <<
			"osgx::makePickCamera: vertHook/fragHook ignored - installProgram=false means no "
			"core program exists for them to attach to" << std::endl;
	}

	auto* ss = cam->getOrCreateStateSet();

	if(installProgram) {
		auto prog = make_nref<osg::Program>("pickProgram");

		auto* vc = new osg::Shader(osg::Shader::VERTEX, PICK_VERT_CORE);
		auto* vh = vertHook ? vertHook : new osg::Shader(osg::Shader::VERTEX, PICK_VERT_HOOK_NOOP);
		auto* fc = new osg::Shader(osg::Shader::FRAGMENT, PICK_FRAG_CORE);
		auto* fh = fragHook ? fragHook : new osg::Shader(osg::Shader::FRAGMENT, PICK_FRAG_HOOK_UNIFORM);

		vc->setName("pickVertCore"); vh->setName("pickVertHook");
		fc->setName("pickFragCore"); fh->setName("pickFragHook");

		prog->addShader(vc); prog->addShader(vh);
		prog->addShader(fc); prog->addShader(fh);

		ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
	}

	auto* bf = new osg::BlendFunc(
		osg::BlendFunc::ONE, osg::BlendFunc::ZERO,
		osg::BlendFunc::ONE, osg::BlendFunc::ZERO
	);

	ss->setAttributeAndModes(bf, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED);
	ss->setMode(GL_DITHER, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

	return cam;
}

uint32_t pickCenter(const uint8_t* px, int n) {
	int half = n / 2;

	return decodePickID(px + (half * n + half) * 4);
}

uint32_t pickMostCoverage(const uint8_t* px, int n) {
	std::unordered_map<uint32_t, unsigned int> counts;

	for(int i = 0; i < n * n; i++) {
		uint32_t id = decodePickID(px + i * 4);

		if(id) counts[id]++;
	}

	if(counts.empty()) return 0;

	return std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) {
		return a.second < b.second;
	})->first;
}

uint32_t pickNearestToCenter(const uint8_t* px, int n) {
	int half = n / 2;
	int bestD = n * n + 1;
	uint32_t bestID = 0;

	for(int row = 0; row < n; row++) {
		for(int col = 0; col < n; col++) {
			uint32_t id = decodePickID(px + (row * n + col) * 4);

			if(!id) continue;

			int dx = col - half, dy = row - half;
			int d = dx * dx + dy * dy;

			if(d < bestD) { bestD = d; bestID = id; }
		}
	}

	return bestID;
}

uint32_t spiralPick(const uint8_t* px, int n) {
	int half = n / 2;
	uint32_t id = decodePickID(px + (half * n + half) * 4);

	if(id) return id;

	for(int r = 1; r <= half; r++) {
		// Top edge: row = half+r, col = half-r .. half+r
		for(int c = -r; c <= r; c++) {
			id = decodePickID(px + ((half + r) * n + (half + c)) * 4);

			if(id) return id;
		}

		// Right edge: col = half+r, row = half+r-1 .. half-r
		for(int rr = r - 1; rr >= -r; rr--) {
			id = decodePickID(px + ((half + rr) * n + (half + r)) * 4);

			if(id) return id;
		}

		// Bottom edge: row = half-r, col = half+r-1 .. half-r
		for(int c = r - 1; c >= -r; c--) {
			id = decodePickID(px + ((half - r) * n + (half + c)) * 4);

			if(id) return id;
		}

		// Left edge: col = half-r, row = half-r+1 .. half+r-1
		for(int rr = -r + 1; rr <= r - 1; rr++) {
			id = decodePickID(px + ((half + rr) * n + (half - r)) * 4);

			if(id) return id;
		}
	}

	return 0;
}

void PickReadback::requestPick(int x, int y) {
	_x.store(x, std::memory_order_relaxed);
	_y.store(y, std::memory_order_relaxed);
	_requested.store(true, std::memory_order_release);
}

void PickReadback::updateMouse(int x, int y) {
	_x.store(x, std::memory_order_relaxed);
	_y.store(y, std::memory_order_relaxed);
}

void PickReadback::reportClick() {
	if(onPick) onPick(_lastID.load(std::memory_order_acquire), ActionType::CLICK);
}

void PickReadback::_fireHover(uint32_t id) const {
	_lastID.store(id, std::memory_order_release);

	if(id == _prevID) return;
	_prevID = id;

	if(onPick) onPick(id, ActionType::HOVER);
}

void PickReadback::_fireClick(uint32_t id) const {
	_lastID.store(id, std::memory_order_release);

	if(onPick) onPick(id, ActionType::CLICK);
}

void PickReadbackSync::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	bool doRead =
		(_mode == Mode::CONTINUOUS) ||
		_requested.exchange(false, std::memory_order_acq_rel)
	;

	if(doRead) {
		int imgW = _image->s();
		int imgH = _image->t();
		const uint8_t* data = _image->data();

		if(data) {
			int imgX, imgY;

			if(_mode == Mode::CONTINUOUS) {
				imgX = imgY = 0; // 1x1 FBO: the single pixel is always at [0,0]
			}

			else {
				// Window-absolute -> viewport-local: see PickReadback::setWindowOrigin's own
				// comment for why this subtraction has to happen before the imgW/_winW scale
				// below, not after.
				int wx = _x.load(std::memory_order_relaxed) - _winOrigin.x();
				int wy = _y.load(std::memory_order_relaxed) - _winOrigin.y();

				imgX = (imgW == _winW) ? wx : wx * imgW / _winW;
				imgY = (imgH == _winH) ? wy : wy * imgH / _winH;
				imgX = std::clamp(imgX, 0, imgW - 1);
				imgY = std::clamp(imgY, 0, imgH - 1);
			}

			int N = _pickSize;
			int cx = std::clamp(imgX, N/2, imgW - (N+1)/2);
			int cy = std::clamp(imgY, N/2, imgH - (N+1)/2);

			std::vector<uint8_t> region(static_cast<std::size_t>(N * N * 4));

			for(int row = 0; row < N; row++) {
				for(int col = 0; col < N; col++) {
					int srcIdx = ((cy - N/2 + row) * imgW + (cx - N/2 + col)) * 4;
					int dstIdx = (row * N + col) * 4;
					std::copy_n(data + srcIdx, 4, region.data() + dstIdx);
				}
			}

			uint32_t id = _rule(region.data(), N);

			if(_mode == Mode::CONTINUOUS) _fireHover(id);
			else _fireClick(id);
		}
	}

	traverse(node, nv);
}

void PickReadbackAsync::operator()(osg::RenderInfo& ri) const {
	auto& state = *ri.getState();
	auto* ext = state.get<osg::GLExtensions>();
	std::size_t bufSize = static_cast<std::size_t>(_imgW * _imgH * 4);

	if(!_init) {
		ext->glGenBuffers(1, &_pbo);
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
		ext->glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bufSize), nullptr, GL_STREAM_READ);
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		_init = true;
	}

	if(_inFlight) {
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
		auto* ptr = static_cast<const uint8_t*>(
			ext->glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)
		);
		if(ptr) {
			int px = std::clamp(_pick.x(), 0, _imgW - 1);
			int py = std::clamp(_pick.y(), 0, _imgH - 1);
			uint32_t id = decodePickID(ptr + (py * _imgW + px) * 4);
			if(_mode == Mode::CLICK) _fireClick(id);
			else _fireHover(id);
			ext->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		_inFlight = false;
	}

	bool doDownload =
		(_mode == Mode::CONTINUOUS) ||
		_requested.exchange(false, std::memory_order_acq_rel);

	if(doDownload) {
		_pick.set(_x.load(std::memory_order_relaxed), _y.load(std::memory_order_relaxed));
		_tex->apply(state);
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, _pbo);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		ext->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		_inFlight = true;
	}
}

void PickCameraSync::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(auto* vc = _viewerCam.get()) {
		auto* cam = static_cast<osg::Camera*>(node);
		int width = _W;
		int height = _H;
		int originX = 0;
		int originY = 0;

		if(const auto* viewport = vc->getViewport()) {
			width = static_cast<int>(viewport->width());
			height = static_cast<int>(viewport->height());
			originX = static_cast<int>(viewport->x());
			originY = static_cast<int>(viewport->y());
		}

		cam->setViewMatrix(vc->getViewMatrix());

		if(_rb && width > 0 && height > 0) {
			_rb->setWindowSize(width, height);
			_rb->setWindowOrigin(originX, originY);
		}

#ifdef OSGX_PLATFORM
		// Transparent, no caller opt-in needed: OSG's own event stream has no "pointer left the
		// window" event at all (see platform::isCursorInWindow()'s own comment), so continuous/
		// hover picking has no way to notice on its own that the cursor is gone and clear
		// itself - the sub-frustum below would otherwise keep re-aiming at the last MOVE
		// event's position forever, reporting stale hover indefinitely. Checked every update
		// traversal, right here, since this callback already runs every frame regardless of
		// whether anything is actually picking-relevant that frame. isCursorInWindow() fails
		// open (true) on a non-X11 GraphicsContext, so this is a safe no-op everywhere else.
		if(_rb && !platform::isCursorInWindow(vc)) _rb->invalidate();
#endif

		// Same shape as the isCursorInWindow() case just above, for a different reason: some
		// other UI (ImGui) currently has mouse capture, so PickHandler stopped updating
		// mouseX()/mouseY() and they're frozen at the last real 3D-viewport position - see
		// PickReadback::setSuspended()'s own comment for why a single invalidate() at the moment
		// capture started isn't enough. Called every frame, unconditionally, for as long as
		// isSuspended() stays true, so the sub-frustum below re-aiming at that frozen (but still
		// valid) spot can never survive to be observed as a hover.
		if(_rb && _rb->isSuspended()) _rb->invalidate();

		// A THIRD, independent guard, needed even with isSuspended() above: mouseX()/mouseY()
		// are window-absolute, and nothing guarantees they land inside THIS camera's own
		// viewport just because they came from a real, freshly-handled event. Concretely:
		// osgx::imgui::Widget::handle() reads io.WantCaptureMouse in the SAME call where it
		// just fed the new position to io.AddMousePosEvent() - Dear ImGui only recomputes
		// WantCaptureMouse inside NewFrame() (run later, from Widget's own PreDraw callback),
		// so that read is one ImGui frame stale. The very first MOVE event whose coordinates
		// land on an ImGui panel is still evaluated against the PREVIOUS frame's hover result
		// (still false), so it comes through with ea.getHandled()==false, isSuspended() never
		// gets set for it, and PickHandler stores that real-but-out-of-viewport coordinate as
		// mouseX()/mouseY() - not frozen at the last good 3D position at all, but pinned to
		// that one bad sample, indefinitely if no further MOVE event happens to correct it.
		// Checking the coordinate directly against the viewport has no such lag, since it
		// doesn't depend on any other handler's state - only on this frame's own geometry.
		bool cursorInViewport = false;

		if(_rb && width > 0 && height > 0) {
			int localX = _rb->mouseX() - originX;
			int localY = _rb->mouseY() - originY;

			cursorInViewport = localX >= 0 && localX < width && localY >= 0 && localY < height;

			if(!cursorInViewport) _rb->invalidate();
		}

		if(_pick1x1 && _rb && width > 0 && height > 0 && cursorInViewport) {
			// mouseX()/mouseY() are window-absolute; the sub-frustum below is built relative
			// to THIS camera's own viewport, so the cursor position has to be viewport-local
			// too, or the sub-frustum aims at the wrong point whenever the main viewport's
			// origin isn't (0, 0) - see PickReadback::setWindowOrigin's own comment.
			double cx = (_rb->mouseX() - originX) + 0.5;
			double cy = (_rb->mouseY() - originY) + 0.5;
			double W = static_cast<double>(width);
			double H = static_cast<double>(height);

			osg::Matrix sub(W, 0, 0, 0, 0, H, 0, 0, 0, 0, 1, 0, W - 2.0*cx, H - 2.0*cy, 0, 1);

			cam->setProjectionMatrix(vc->getProjectionMatrix() * sub);
		}

		else {
			cam->setProjectionMatrix(vc->getProjectionMatrix());
		}
	}

	traverse(node, nv);
}

void PickHoverCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	uint32_t id = _rb->lastID();

	if(id != _prevID) {
		uint32_t prev = _prevID;
		_prevID = id;

		if(prev != 0 && _rb->onLeave) _rb->onLeave(prev);
		if(id   != 0 && _rb->onEnter) _rb->onEnter(id);
	}

	traverse(node, nv);
}

bool PickHandler::handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) {
	// Some earlier handler (osgx::imgui::Widget, which registers itself at the FRONT of the
	// handler list - see Widget's own constructor) already claimed this event, e.g. the cursor
	// is over an ImGui panel. Match the convention every other OSG handler follows (see
	// StatsHandler/HelpHandler/the stock manipulators) and bail immediately, instead of also
	// updating pick state from a mouse position that isn't really "over the 3D scene". Recorded
	// via setSuspended() rather than a one-shot invalidate() here - PickCameraSync re-invalidates
	// continuously every frame for as long as this stays true, which is what actually prevents
	// the sub-frustum from re-detecting a hover at the now-frozen mouseX()/mouseY() a frame
	// later; see PickReadback::setSuspended()'s own comment.
	_rb->setSuspended(ea.getHandled());

	if(ea.getHandled()) return false;

	int x = static_cast<int>(ea.getX());
	int y = static_cast<int>(ea.getY());

	if(ea.getEventType() == osgGA::GUIEventAdapter::MOVE && _continuous) {
		_rb->updateMouse(x, y);

		return false;
	}

	if(
		ea.getEventType() == osgGA::GUIEventAdapter::PUSH &&
		ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON
	) {
		if(_continuous) _rb->reportClick();
		else _rb->requestPick(x, y);

		if(_consume) return true;
	}

	return false;
}

}
