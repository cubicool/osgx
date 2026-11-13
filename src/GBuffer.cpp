#include "osgx/GBuffer.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>

OSGX_ENABLE_WARNINGS

#include <span>

namespace osgx {

namespace {

GLint internalFormatFor(GBuffer::AttachmentFormat format) {
	switch(format) {
		case GBuffer::AttachmentFormat::RGBA8: return GL_RGBA;
		case GBuffer::AttachmentFormat::RGB16F: return GL_RGB16F;
		case GBuffer::AttachmentFormat::RGBA16F: return GL_RGBA16F;
		case GBuffer::AttachmentFormat::RGBA32F: return GL_RGBA32F;
	}

	return GL_RGBA;
}

}

bool GBuffer::valid() const {
	return camera.valid() && !colorTextures.empty() && depthTexture.valid();
}

GBuffer GBuffer::create(
	osg::Node* node,
	int width,
	int height,
	std::span<const GBuffer::AttachmentFormat> colorFormats,
	osg::Transform::ReferenceFrame referenceFrame
) {
	GBuffer result;

	if(!node || colorFormats.empty()) return result;

	for(const auto format: colorFormats) {
		auto tex = osgx::make_ref<osg::Texture2D>();

		tex->setTextureSize(width, height);
		tex->setInternalFormat(internalFormatFor(format));
		tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
		tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
		tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
		tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
		// Every one of these is BOTH a render target (written here, every frame) AND a sampler
		// input in a later pass (SSAO/lighting) - without DYNAMIC, OSG's default StateAttribute
		// caching can treat a texture as unchanging after its first successful bind and stop
		// correctly re-applying it on later frames (same fix already required for
		// osgx-gbuffer.cpp's own hdr_color_tex/ao_tex-style RTT textures, and for every RTT
		// texture in pyosg-lighting's own examples - see 11-sketchfab.py).
		tex->setDataVariance(osg::Object::DYNAMIC);

		result.colorTextures.push_back(tex);
	}

	result.depthTexture = osgx::make_ref<osg::Texture2D>();
	result.depthTexture->setTextureSize(width, height);
	result.depthTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
	result.depthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
	result.depthTexture->setSourceType(GL_FLOAT);
	result.depthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	result.depthTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	result.depthTexture->setDataVariance(osg::Object::DYNAMIC);

	// Locally osgx::RTT-typed (constructor takes referenceFrame directly - GBuffer's own default
	// of RELATIVE_RF is exactly RTT's documented second shape, see RTT.hpp) - but GBuffer::camera
	// itself stays osg::ref_ptr<osg::Camera> (Python-bound; see ShadowMap's identical reasoning).
	auto camera = osgx::make_nref<osgx::RTT>("osgx_gbuffer_GeometryPass", width, height, referenceFrame);

	camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	camera->setClearColor(osg::Vec4(0.0, 0.0, 0.0, 0.0));
	// MUST stay the default COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES, not DO_NOT_COMPUTE_NEAR_FAR.
	// This camera is RELATIVE_RF with an identity local projection, so its effective near/far is
	// otherwise whatever the ENCLOSING camera's own projection happens to carry - usually an
	// arbitrary/stale default (e.g. a fresh Viewer's near=1/far=10000), since a model living only
	// under this nested RTT camera never contributes to the enclosing camera's own near/far
	// auto-compute. A real-world-scale model (e.g. a glTF prop a few centimeters across) then sits
	// entirely inside that default near plane and is silently clipped to nothing - every G-buffer
	// attachment comes back blank. Auto-compute here privately tightens near/far to whatever THIS
	// camera's own subtree actually contains, every frame, without writing back to the shared
	// Camera object (same mechanism SSAO::create()'s own comment in SSAO.hpp, re:
	// DepthProjectionCallback, describes) - this is what keeps a tiny model visible regardless of
	// what the enclosing camera's projection says.
	//
	// Tradeoff: if a large receiver (e.g. a floor) shares this camera alongside a much smaller
	// subject, auto-compute measures both, and the receiver can push the far plane out far enough
	// to crush depth precision for the subject. DO_NOT_COMPUTE_NEAR_FAR "fixed" that by disabling
	// the tightening outright, but that is what caused the silent-clipping bug above - a global,
	// content-blind flag is the wrong tool for a scene-composition problem. If the precision
	// tradeoff above resurfaces, tune it via the real scene it shows up in: `camera` is exposed on
	// the returned GBuffer, so a caller can set a tighter camera->setNearFarRatio() (or split the
	// receiver onto its own pass) once there is an actual case to tune against, rather than this
	// function guessing a value blind.

	// Dynamic attachment count (caller-chosen colorFormats span) - AttachmentList's initializer-
	// list shape needs a compile-time-fixed count, so this stays a loop over RTT's inherited
	// attach() rather than one declarative call (see ShadowMap/Aura for the fixed-count case).
	for(std::size_t i = 0; i < result.colorTextures.size(); i++) {
		camera->attach(
			static_cast<osg::Camera::BufferComponent>(osg::Camera::COLOR_BUFFER0 + i),
			result.colorTextures[i]
		);
	}

	camera->attach(osg::Camera::DEPTH_BUFFER, result.depthTexture);
	camera->addChild(node);

	result.camera = camera;

	return result;
}

}
