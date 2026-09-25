#include "ShaderLibs.hpp"

#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Math>
#include <osg/Matrix>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>

namespace osgx {

namespace {

// Vertex-transform-only, empty-fragment Program installed on every directional shadow camera's
// own StateSet (ON|OVERRIDE) - see ShadowMap::create()'s own comment for why. Uses OSG's
// standard osg_Vertex/osg_ModelViewProjectionMatrix names (auto-bound by OSG, same as every other
// osgx/pyosg-lighting shader - no explicit addBindAttribLocation() needed) so it works unmodified
// against any subgraph, not just a specific vertex-attribute convention.
constexpr const char DEPTH_ONLY_VERTEX_SHADER[] = R"GLSL(
#version 460 core

in vec4 osg_Vertex;

uniform mat4 osg_ModelViewProjectionMatrix;

void main() {
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

constexpr const char DEPTH_ONLY_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

void main() {
}
)GLSL";

osg::ref_ptr<osg::Program> makeDepthOnlyProgram() {
	auto program = osgx::make_nref<osg::Program>("osgx_shadow_DepthOnly");

	program->addShader(new osg::Shader(osg::Shader::VERTEX, DEPTH_ONLY_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, DEPTH_ONLY_FRAGMENT_SHADER));

	return program;
}

// Shared by ShadowMap::create()/ShadowMap::reposition() - the only difference
// between "create" and "reposition" is whether a new camera/texture gets allocated around this
// math, not the math itself.
void computeDirectionalShadowMatrices(
	const osg::Vec3& lightDirection,
	const osg::Vec3& sceneBoundCenter,
	float sceneBoundRadius,
	const ShadowMapOptions& options,
	osg::Matrixd& lightView,
	osg::Matrixd& lightProj
) {
	osg::Vec3 dir = lightDirection;

	dir.normalize();

	const double extent = options.extent > 0.0f
		? double(options.extent)
		: double(sceneBoundRadius) * double(options.margin);
	const double distance = extent * 2.0;
	const osg::Vec3 lightPos = sceneBoundCenter - dir * float(distance);

	// Up vector (0,1,0), not (0,0,1) - a light direction nearly aligned with world-up produces a
	// degenerate lookAt with (0,0,1) (same failure mode 08-shadows.py/09-ibl.py both noted); (0,1,0)
	// sidesteps it for every direction any pyosg-lighting example has used.
	lightView = osg::Matrix::lookAt(lightPos, sceneBoundCenter, osg::Vec3(0.0, 1.0, 0.0));

	const double near_ = std::max(0.01, distance - extent);
	const double far_ = distance + extent;

	// Orthographic, not perspective - a directional light's rays are parallel by construction;
	// see ShadowMapOptions::extent's own comment for why a perspective frustum here is simply
	// wrong (not a style choice) for this light type.
	lightProj = osg::Matrix::ortho(-extent, extent, -extent, extent, near_, far_);
}

}

bool ShadowMap::valid() const {
	return camera.valid() && depthTexture.valid() && shadowMatrix.valid();
}

ShadowMap ShadowMap::create(
	const osg::Vec3& lightDirection,
	const osg::Vec3& sceneBoundCenter,
	float sceneBoundRadius,
	const ShadowMapOptions& options
) {
	ShadowMap result;

	computeDirectionalShadowMatrices(
		lightDirection, sceneBoundCenter, sceneBoundRadius, options, result.lightView, result.lightProj
	);

	result.depthTexture = osgx::make_ref<osg::Texture2D>();
	result.depthTexture->setTextureSize(options.size, options.size);
	result.depthTexture->setInternalFormat(GL_DEPTH_COMPONENT24);
	result.depthTexture->setSourceFormat(GL_DEPTH_COMPONENT);
	result.depthTexture->setSourceType(GL_FLOAT);
	result.depthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	result.depthTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	result.depthTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	// Render target (shadow camera) AND sampler input (the lighting pass's osgx_ShadowFactor())
	// - without DYNAMIC, OSG's default StateAttribute caching can treat this as unchanging after
	// its first successful bind and stop correctly re-applying it later. Same fix as
	// GBuffer.cpp's own color/depth textures.
	result.depthTexture->setDataVariance(osg::Object::DYNAMIC);

	// Locally osgx::RTT-typed (constructor + initializer-list attach()) - but ShadowMap::camera
	// itself stays osg::ref_ptr<osg::Camera> (see its own declaration) since it's exposed to the
	// Python bindings and osgx::RTT isn't a registered pybind11 type.
	auto camera = osgx::make_nref<osgx::RTT>(
		"osgx_shadow_DirectionalShadowMap", options.size, options.size
	);

	camera->setClearMask(GL_DEPTH_BUFFER_BIT);
	camera->setClearDepth(1.0);
	camera->attach({{osg::Camera::DEPTH_BUFFER, result.depthTexture}});
	// Depth-only: the old hand-rolled Python examples attached a dummy color texture here to work
	// around a since-irrelevant pybind11 binding gap (Camera::setDrawBuffer/setReadBuffer weren't
	// exposed to Python yet) - ordinary C++ calls, no workaround needed.
	camera->setDrawBuffer(GL_NONE);
	camera->setReadBuffer(GL_NONE);
	camera->setViewMatrix(result.lightView);
	camera->setProjectionMatrix(result.lightProj);
	// ON|OVERRIDE, no PROTECTED: wins over any Program a child subgraph sets on its OWN StateSet
	// with just ON (the convention every osgx::PBRScene/pyosg-lighting Program uses) --
	// see this function's own header comment for the full rationale.
	camera->getOrCreateStateSet()->setAttributeAndModes(
		makeDepthOnlyProgram(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
	);

	result.camera = camera;

	result.shadowMatrix = new osg::Uniform("osgx_shadowMatrix", osg::Matrixf::identity());
	result.bias = new osg::Uniform("osgx_shadowBias", options.bias);
	result.strength = new osg::Uniform("osgx_shadowStrength", options.strength);
	result.casterIndex = new osg::Uniform("osgx_shadowCasterIndex", 0);

	result.updateMatrix();

	return result;
}

void ShadowMap::updateMatrix() {
	if(!shadowMatrix) return;

	// OSG row-vector convention: worldPos * (lightView * lightProj) is the same composition GLSL's
	// osgx_shadowMatrix * vec4(worldPos, 1.0) performs once uploaded - see Shadow.hpp's file-level
	// comment for why this needs no main-camera term (unlike the eye-space hand-rolled examples).
	shadowMatrix->set(osg::Matrixf(lightView * lightProj));
}

void ShadowMap::reposition(
	const osg::Vec3& lightDirection,
	const osg::Vec3& sceneBoundCenter,
	float sceneBoundRadius,
	const ShadowMapOptions& options
) {
	if(!camera) return;

	computeDirectionalShadowMatrices(
		lightDirection, sceneBoundCenter, sceneBoundRadius, options, lightView, lightProj
	);

	camera->setViewMatrix(lightView);
	camera->setProjectionMatrix(lightProj);

	updateMatrix();
}

void registerShadowShaderLibs() {
	static const osgx::ShaderLib libs[] = {
		{"SHADOW_UNIFORMS", "osgx_shadowMap", SHADOW_UNIFORMS},
		{"SHADOW_FACTOR", "osgx_ShadowFactor", SHADOW_FACTOR},
	};

	::osgx::registerShaderLibs("osgx::shadow", libs);
}

}
