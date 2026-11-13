#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Node>
#include <osg/Texture2D>
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
// One populated G-buffer: `camera` is the PRE_RENDER FBO pass that writes `colorTextures`
// (indexed exactly as passed to create(), i.e. colorTextures[i] is
// COLOR_BUFFER<i>) plus `depthTexture`. The caller still owns adding `camera` to the rendered
// scene graph - this struct only owns creating it.
struct GBuffer {
	enum class AttachmentFormat {
		RGBA8,
		RGB16F,
		RGBA16F,
		RGBA32F
	};

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

}
