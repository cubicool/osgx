#include "osgx/Aura.hpp"
#include "osgx/IBL.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <array>
#include <span>

namespace osgx {

namespace {

constexpr const char SELECTION_VERTEX_SHADER[] = R"GLSL(
#version 430 core

in vec4 osg_Vertex;
uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat4 osg_ModelViewMatrix;

out float vEyeDepth;

void main() {
	vEyeDepth = (osg_ModelViewMatrix * osg_Vertex).z;
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

// MRT: COLOR_BUFFER0 = originalMask (unchanged contract), COLOR_BUFFER1 = originalDepth (new).
// One geometry pass either way -- adding a second render target here is what keeps the whole
// pipeline at 3 passes instead of a separate depth-capture pass.
constexpr const char SELECTION_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

in float vEyeDepth;

layout(location = 0) out vec4 fragMask;
layout(location = 1) out vec4 fragDepth;

void main() {
	fragMask = vec4(1.0);
	fragDepth = vec4(vEyeDepth, 0.0, 0.0, 1.0);
}
)GLSL";

// Propagates mask AND depth as VALUES through the nearest-neighbor search -- not a UV to re-sample
// later. dilatedX layout: r=found, g=depth, b=X-distance-so-far, a=unused.
constexpr const char DILATE_X_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform sampler2D auraOriginalMask;
uniform sampler2D auraOriginalDepth;
uniform int auraRadius;

in vec2 vUV;
out vec4 fragColor;

void main() {
	vec2 texel = 1.0 / vec2(textureSize(auraOriginalMask, 0));
	vec4 result = vec4(0.0);

	for(int distance = 0; distance <= 64; distance++) {
		if(distance > auraRadius) break;

		vec2 sourceUV = vUV - vec2(float(distance), 0.0) * texel;

		if(texture(auraOriginalMask, sourceUV).r > 0.5) {
			result = vec4(1.0, texture(auraOriginalDepth, sourceUV).r, float(distance), 0.0);

			break;
		}

		sourceUV = vUV + vec2(float(distance), 0.0) * texel;

		if(distance != 0 && texture(auraOriginalMask, sourceUV).r > 0.5) {
			result = vec4(1.0, texture(auraOriginalDepth, sourceUV).r, float(distance), 0.0);

			break;
		}
	}

	fragColor = result;
}
)GLSL";

// expanded layout: r=found, g=depth (propagated through from dilatedX, itself propagated through
// from originalDepth), b=unused, a=Chebyshev distance in pixels.
constexpr const char DILATE_Y_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform sampler2D auraDilatedX;
uniform int auraRadius;

in vec2 vUV;
out vec4 fragColor;

void main() {
	vec2 texel = 1.0 / vec2(textureSize(auraDilatedX, 0));
	vec4 result = vec4(0.0);
	float nearestDistance = float(auraRadius) + 1.0;

	for(int distance = 0; distance <= 64; distance++) {
		if(distance > auraRadius) break;

		vec4 candidate = texture(auraDilatedX, vUV - vec2(0.0, float(distance)) * texel);

		if(candidate.r > 0.5) {
			float candidateDistance = max(candidate.b, float(distance));

			if(candidateDistance < nearestDistance) {
				result = vec4(1.0, candidate.g, 0.0, candidateDistance);
				nearestDistance = candidateDistance;
			}
		}

		candidate = texture(auraDilatedX, vUV + vec2(0.0, float(distance)) * texel);

		if(distance != 0 && candidate.r > 0.5) {
			float candidateDistance = max(candidate.b, float(distance));

			if(candidateDistance < nearestDistance) {
				result = vec4(1.0, candidate.g, 0.0, candidateDistance);
				nearestDistance = candidateDistance;
			}
		}
	}

	fragColor = result;
}
)GLSL";

osg::ref_ptr<osg::Texture2D> makeTexture(
	int width,
	int height,
	GLint internalFormat,
	GLenum sourceFormat,
	GLenum sourceType
) {
	auto texture = osgx::make_ref<osg::Texture2D>();

	texture->setTextureSize(width, height);
	texture->setInternalFormat(internalFormat);
	texture->setSourceFormat(sourceFormat);
	texture->setSourceType(sourceType);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	texture->setDataVariance(osg::Object::DYNAMIC);

	return texture;
}

osg::ref_ptr<osg::Camera> makeDilationPass(
	const char* name,
	std::span<const osgx::detail::TextureInput> inputs,
	osg::Texture2D* output,
	osg::Uniform* radius,
	const char* fragmentShader,
	int renderOrder
) {
	auto camera = osgx::RTT::fullscreenQuad(
		output->getTextureWidth(), output->getTextureHeight(), fragmentShader
	);

	camera->setName(name);
	camera->setRenderOrder(osg::Camera::PRE_RENDER, renderOrder);
	camera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
	camera->attach({{osg::Camera::COLOR_BUFFER0, output}});

	auto* stateSet = camera->getOrCreateStateSet();

	osgx::detail::bindTextureInputs(stateSet, inputs);
	stateSet->addUniform(radius);

	return camera;
}

}

bool Aura::valid() const {
	return selectionCamera.valid()
		&& dilateXCamera.valid()
		&& dilateYCamera.valid()
		&& originalMask.valid()
		&& originalDepth.valid()
		&& dilatedX.valid()
		&& expanded.valid()
		&& radius.valid()
	;
}

Aura Aura::create(int width, int height, int radiusPixels) {
	Aura result;

	if(width <= 0 || height <= 0) return result;

	result.originalMask = makeTexture(width, height, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
	result.originalDepth = makeTexture(width, height, GL_R32F, GL_RED, GL_FLOAT);
	result.dilatedX = makeTexture(width, height, GL_RGBA16F, GL_RGBA, GL_FLOAT);
	result.expanded = makeTexture(width, height, GL_RGBA16F, GL_RGBA, GL_FLOAT);
	result.radius = new osg::Uniform("auraRadius", std::clamp(radiusPixels, 0, 64));

	auto program = osgx::make_nref<osg::Program>("osgx_aura_SelectionMask");

	program->addShader(new osg::Shader(osg::Shader::VERTEX, SELECTION_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, SELECTION_FRAGMENT_SHADER));

	// RELATIVE_RF (RTT's non-default reference frame -- see RTT.hpp's own comment): this camera
	// never sets its own view/projection, so it renders the selected node from exactly the same
	// viewpoint as whatever camera it ends up under in the scene graph, via ordinary cull-time
	// matrix composition. Locally osgx::RTT-typed for the constructor/attach() conveniences; the
	// Aura::selectionCamera field itself stays osg::ref_ptr<osg::Camera> (Python-bound).
	auto selectionCamera = osgx::make_nref<osgx::RTT>(
		"osgx_aura_SelectionMask", width, height, osg::Transform::RELATIVE_RF
	);

	selectionCamera->setRenderOrder(osg::Camera::PRE_RENDER, 1);
	selectionCamera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	selectionCamera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
	selectionCamera->attach({
		{osg::Camera::COLOR_BUFFER0, result.originalMask},
		{osg::Camera::COLOR_BUFFER1, result.originalDepth}
	});

	// A selected node commonly already owns its normal material Program. PROTECTED makes this
	// flat mask Program authoritative for this camera without mutating that visible scene state.
	selectionCamera->getOrCreateStateSet()->setAttributeAndModes(
		program, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED
	);

	result.selectionCamera = selectionCamera;

	const std::array<osgx::detail::TextureInput, 2> dilateXInputs = {{
		{ 0, result.originalMask, "auraOriginalMask" },
		{ 1, result.originalDepth, "auraOriginalDepth" }
	}};
	const std::array<osgx::detail::TextureInput, 1> dilateYInputs = {{
		{ 0, result.dilatedX, "auraDilatedX" }
	}};

	result.dilateXCamera = makeDilationPass(
		"osgx_aura_DilateX", dilateXInputs, result.dilatedX, result.radius, DILATE_X_FRAGMENT_SHADER, 2
	);
	result.dilateYCamera = makeDilationPass(
		"osgx_aura_DilateY", dilateYInputs, result.expanded, result.radius, DILATE_Y_FRAGMENT_SHADER, 3
	);

	return result;
}

}
