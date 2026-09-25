#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Node>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <span>
#include <vector>

namespace osgx {

// ================================================================================================
// Generic deferred G-buffer camera setup. Not PBR/glTF-specific: the proven MRT shape
// `examples/pyosg-mrt.py` validated (OpenSceneGraph.py, 2026-07-11 - one geometry pass writing
// several SIMULTANEOUS color attachments via `layout(location = n) out`, plus a real depth
// attachment), generalized from that example's fixed two-color-buffer toon-shading demo into a
// reusable helper any deferred consumer can call - osgx::PBRGBuffer::create() (PBRDeferred.hpp)
// is built on top of this, not a separate mechanism,
// and a non-PBR deferred shader (e.g. a toon pipeline like pyosg-mrt.py's own) can use it
// directly too. Lives directly under `osgx::`, not its own namespace - it's not a separate
// opt-in subsystem (its own #include outside the umbrella, its own CMake link target) the way
// osgx::debug/imgui/platform/gltf/ktx2 are; see TODO.md's namespace-boundary decision.
// ================================================================================================

// Texture internal-format presets for one G-buffer color attachment - matches pyosg-mrt.py's
// own validated conventions exactly: GL_RGBA for ordinary LDR color/albedo, GL_RGB16F for
// signed [-1,1] data (a view-space normal being the motivating case) so no encode/decode remap
// is needed at write or read time, GL_RGBA16F for HDR color (an emissive buffer, which can
// legitimately exceed 1.0 before tonemapping). RGBA32F is for real eye-space position data --
// see 11-sketchfab.py's own `gPosition` attachment, written straight from the vertex shader
// instead of reconstructed from depth + an inverse-projection matrix in the lighting pass; that
// reconstruction is fundamentally unreliable across nested PRE_RENDER cameras (each one clamps
// its own private near/far during its own cull pass and never writes the result back onto the
// Camera object, so a projection matrix read off a DIFFERENT camera after the fact does not
// necessarily match what actually wrote the depth buffer).
enum class AttachmentFormat {
	RGBA8,
	RGB16F,
	RGBA16F,
	RGBA32F
};

// One populated G-buffer: `camera` is the PRE_RENDER FBO pass that writes `colorTextures`
// (indexed exactly as passed to create(), i.e. colorTextures[i] is
// COLOR_BUFFER<i>) plus `depthTexture`. The caller still owns adding `camera` to the rendered
// scene graph - this struct only owns creating it.
struct GBuffer {
	osg::ref_ptr<osg::Camera> camera;
	std::vector<osg::ref_ptr<osg::Texture2D>> colorTextures;
	osg::ref_ptr<osg::Texture2D> depthTexture;

	bool valid() const;

	// Builds a PRE_RENDER FBO camera with `node` (a real 3D scene subgraph, NOT a fullscreen quad
	// - this is a geometry pass) as its child, writing `colorFormats.size()` simultaneous color
	// attachments (COLOR_BUFFER0..N, requiring `layout(location = n) out` declarations in the
	// caller's own fragment shader, one per format in order) plus a real GL_DEPTH_COMPONENT24
	// depth attachment. `referenceFrame` defaults to RELATIVE_RF (composes with `node`'s own
	// ancestor transforms/camera, the normal case for a G-buffer pass parented under the real
	// scene) - pass ABSOLUTE_RF explicitly for a camera that should own its own fixed
	// view/projection instead (e.g. a shadow-map-style pass; osgx::ShadowMap::create() does this
	// itself rather than going through this helper, since it is depth-only with no color
	// attachments at all).
	static GBuffer create(
		osg::Node* node,
		int width,
		int height,
		std::span<const AttachmentFormat> colorFormats,
		osg::Transform::ReferenceFrame referenceFrame=osg::Transform::RELATIVE_RF
	);
};

// Hemisphere-kernel screen-space ambient occlusion, operating on any G-buffer's view-space
// normal + position channels - nothing else, so it stays independent of any particular
// G-buffer's full channel layout (glTF-material-shaped, hand-authored, or otherwise). Ported from
// the exact, live-validated `OpenSceneGraph.py/examples/pyosg-lighting/11-sketchfab.py`
// implementation: a 16-sample hemisphere kernel + a small tiled tangent-space noise-rotation
// texture (the classic Crysis-era technique, still the accepted baseline today), a raw RTT pass
// reading the G-buffer's normal/position directly (no separate depth texture needed - eye-space
// position already carries real depth in its Z, same reasoning `GBuffer`'s own
// `AttachmentFormat::RGBA32F` comment gives), then a small fixed-radius box-blur RTT pass to
// denoise it. `aoTexture` (the blurred result) is a single-channel `GL_R8` texture in `[0, 1]`
// (1.0 = fully unoccluded) that plugs directly into
// `osgx::PBRLightingPassOptions::aoTexture` (PBRDeferred.hpp) - that seam was built
// exactly for this - or any other consumer wanting a generic screen-space occlusion mask.
struct SSAO {
	osg::ref_ptr<osg::Camera> rawCamera;
	osg::ref_ptr<osg::Camera> blurCamera;
	osg::ref_ptr<osg::Texture2D> aoTexture;
	// Live-tunable: call ->set(value) at any time (e.g. from an ImGui slider), no pass rebuild
	// needed - both are plain uniforms read fresh every draw.
	osg::ref_ptr<osg::Uniform> radius;
	osg::ref_ptr<osg::Uniform> bias;

	bool valid() const;

	// `normalTexture`/`positionTexture` must be VIEW-space (matching `PBRGBuffer::
	// normalTexture`/`positionTexture`'s own contract) - the hemisphere kernel and the forward
	// re-projection below both assume it.
	//
	// `projectionMatrix` is a CALLER-OWNED uniform this pass reads every draw, not a one-time
	// snapshot - SSAO reconstructs each sample's screen position via a forward projection, and
	// (same reasoning as `PBRLightingPass`'s own view-matrix uniforms) the real matrix isn't
	// meaningfully established until well after this call returns, and can change every frame
	// besides. Keep its value fresh yourself from the same per-frame `preDrawCallback` that updates
	// any other deferred-pass matrix uniforms you already have (see `PBRLightingPass::update()`'s
	// own comment for why that must be a `PRE_RENDER` `preDrawCallback`, not application code after
	// `viewer.frame()` returns) - e.g. `projectionMatrix->set(osg::Matrixf(mainCamera->
	// getProjectionMatrix()))`.
	//
	// `radius`/`bias` seed the two returned uniforms' initial values; both are scale-dependent
	// (a sane `radius` is a small fraction of the scene's own bounding radius, not a fixed
	// constant - see `osgx-gbuffer-comic.cpp`'s `hatchFrequency` for the same "derive from the
	// model's own bounds" precedent) so callers should compute them from the scene being rendered
	// rather than relying on the defaults for anything but a quick first look.
	static SSAO create(
		osg::Texture2D* normalTexture,
		osg::Texture2D* positionTexture,
		osg::Uniform* projectionMatrix,
		int width,
		int height,
		float radius=0.5f,
		float bias=0.02f
	);
};

}
