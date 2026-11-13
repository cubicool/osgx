#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

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
// `osgx::PBRLightingPass::Options::aoTexture` (PBRDeferred.hpp) - that seam was built
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
	// snapshot - SSAO reconstructs each sample's screen position via a forward projection, and it
	// must match the EXACT projection the gPosition/gNormal source camera actually drew with, not
	// a later read of mainCamera->getProjectionMatrix() - OSG's CullVisitor clamps each camera's
	// near/far privately during cull and never writes it back to the Camera object, so that read
	// is reliably stale/wrong the moment near/far auto-computes to anything other than what was
	// last explicitly set. Install an osgx::DepthProjectionCallback (Projection.hpp) as the
	// gPosition-producing camera's own postDrawCallback, constructed with this uniform's name
	// ("projectionMatrix", to match SSAO_FRAGMENT_SHADER_SRC's own declaration), and pass its
	// getProjection() straight through as this parameter - it captures the real osg::State
	// projection matrix at the moment that camera draws, every frame, with no manual copying.
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
