#include "osgx/GBuffer.hpp"
#include "osgx/IBL.hpp"
#include "osgx/RTT.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <array>
#include <cmath>
#include <random>
#include <span>

namespace osgx {

namespace {

GLint internalFormatFor(AttachmentFormat format) {
	switch(format) {
		case AttachmentFormat::RGBA8: return GL_RGBA;
		case AttachmentFormat::RGB16F: return GL_RGB16F;
		case AttachmentFormat::RGBA16F: return GL_RGBA16F;
		case AttachmentFormat::RGBA32F: return GL_RGBA32F;
	}

	return GL_RGBA;
}

constexpr int SSAO_KERNEL_SIZE = 16;
constexpr int SSAO_NOISE_SIZE = 4;

// Hemisphere-oriented sample kernel, quadratically clustered toward the origin - the standard
// SSAO kernel shape (Crysis-era; still the baseline technique today). Matches
// 11-sketchfab.py's generate_ssao_kernel() exactly, RNG choice aside (std::mt19937 here vs.
// numpy there - the shape is what matters, not bit-identical samples).
osg::ref_ptr<osg::Uniform> makeSSAOKernelUniform() {
	std::mt19937 rng(0);
	std::uniform_real_distribution<float> signedDist(-1.0f, 1.0f);
	std::uniform_real_distribution<float> smallZDist(0.05f, 1.0f);
	std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);

	auto* u = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "samples", SSAO_KERNEL_SIZE);

	for(int i = 0; i < SSAO_KERNEL_SIZE; i++) {
		osg::Vec3 v(signedDist(rng), signedDist(rng), smallZDist(rng));

		v.normalize();
		v *= unitDist(rng);

		const float scale = 0.1f + 0.9f * std::pow(static_cast<float>(i) / SSAO_KERNEL_SIZE, 2.0f);

		u->setElement(static_cast<unsigned int>(i), v * scale);
	}

	return u;
}

// Tiny tiled texture of random tangent-space rotation vectors - removes the visible banding a
// fixed kernel would otherwise leave (every pixel sampling identical relative directions).
osg::ref_ptr<osg::Texture2D> makeSSAONoiseTexture() {
	std::mt19937 rng(1);
	std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

	auto image = osgx::make_ref<osg::Image>();

	image->allocateImage(SSAO_NOISE_SIZE, SSAO_NOISE_SIZE, 1, GL_RGB, GL_FLOAT);

	auto* data = reinterpret_cast<float*>(image->data());

	for(int i = 0; i < SSAO_NOISE_SIZE * SSAO_NOISE_SIZE; i++) {
		data[i * 3 + 0] = dist(rng);
		data[i * 3 + 1] = dist(rng);
		data[i * 3 + 2] = 0.0f;
	}

	auto tex = osgx::make_ref<osg::Texture2D>();

	tex->setImage(image);
	tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	tex->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
	tex->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

	return tex;
}

// Shared boilerplate for SSAO's two fullscreen-quad RTT passes (raw sample + blur) - both are
// simple single-shader single-output passes, so one helper covers both rather than duplicating
// the camera/program/quad setup twice in create() below. osgx::RTT::fullscreenQuad() now owns
// the camera/program/quad/depth-state boilerplate this used to hand-roll directly.
osg::ref_ptr<osg::Camera> makeFullscreenRTTPass(
	const char* name,
	std::span<const osgx::detail::TextureInput> textures,
	osg::Texture2D* outputTexture,
	const char* fragmentShaderSrc,
	int width,
	int height
) {
	auto cam = osgx::RTT::fullscreenQuad(width, height, fragmentShaderSrc);

	cam->setName(name);
	cam->setClearColor(osg::Vec4(0.0, 0.0, 0.0, 0.0));
	cam->attach({{osg::Camera::COLOR_BUFFER0, outputTexture}});

	osgx::detail::bindTextureInputs(cam->getOrCreateStateSet(), textures);

	return cam;
}

constexpr const char SSAO_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core

#define NUM_SAMPLES 16

uniform sampler2D gNormal;
uniform sampler2D ssaoNoise;
uniform sampler2D gPosition;

uniform mat4 projectionMatrix;

uniform vec3 samples[NUM_SAMPLES];
uniform float ssaoRadius;
uniform float ssaoBias;

in vec2 vUV;

out vec4 fragColor;

void main() {
	vec3 rawN = texture(gNormal, vUV).rgb;

	// An unwritten pixel has a zero-length normal; normalize(0) would produce NaN, and there's no
	// real geometry to occlude anyway, so just report "no occlusion" immediately. Only .r survives
	// in the GL_R8 output texture, so the other channels here are unused.
	if(dot(rawN, rawN) < 0.0001) {
		fragColor = vec4(1.0);

		return;
	}

	vec3 N = normalize(rawN);
	vec3 fragPos = texture(gPosition, vUV).xyz;

	vec2 noiseScale = vec2(textureSize(gPosition, 0)) / float(textureSize(ssaoNoise, 0).x);
	vec3 rvec = texture(ssaoNoise, vUV * noiseScale).xyz;
	vec3 tangent = normalize(rvec - N * dot(rvec, N));
	vec3 bitangent = cross(N, tangent);
	mat3 TBN = mat3(tangent, bitangent, N);

	float occlusion = 0.0;

	for(int i = 0; i < NUM_SAMPLES; i++) {
		vec3 samplePos = fragPos + (TBN * samples[i]) * ssaoRadius;

		vec4 offset = projectionMatrix * vec4(samplePos, 1.0);

		offset.xyz /= offset.w;
		offset.xyz = offset.xyz * 0.5 + 0.5;

		float sampleDepth = texture(gPosition, offset.xy).z;
		float rangeCheck = smoothstep(0.0, 1.0, ssaoRadius / max(abs(fragPos.z - sampleDepth), 0.0001));

		occlusion += (sampleDepth >= samplePos.z + ssaoBias ? 1.0 : 0.0) * rangeCheck;
	}

	fragColor = vec4(vec3(1.0 - occlusion / float(NUM_SAMPLES)), 1.0);
}
)GLSL";

// Small fixed-radius box blur to denoise the hemisphere-kernel noise - deliberately not a
// separable/gaussian blur; a 4x4 box is cheap and the raw SSAO signal has no sharp edges worth
// preserving.
constexpr const char SSAO_BLUR_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core

uniform sampler2D ssaoRawTex;

in vec2 vUV;

out vec4 fragColor;

void main() {
	vec2 texel = 1.0 / vec2(textureSize(ssaoRawTex, 0));
	float sum = 0.0;

	for(int x = -2; x < 2; x++)
		for(int y = -2; y < 2; y++)
			sum += texture(ssaoRawTex, vUV + vec2(x, y) * texel).r;

	fragColor = vec4(vec3(sum / 16.0), 1.0);
}
)GLSL";

}

bool GBuffer::valid() const {
	return camera.valid() && !colorTextures.empty() && depthTexture.valid();
}

GBuffer GBuffer::create(
	osg::Node* node,
	int width,
	int height,
	std::span<const AttachmentFormat> colorFormats,
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

bool SSAO::valid() const {
	return rawCamera.valid() && blurCamera.valid() && aoTexture.valid();
}

SSAO SSAO::create(
	osg::Texture2D* normalTexture,
	osg::Texture2D* positionTexture,
	osg::Uniform* projectionMatrix,
	int width,
	int height,
	float radius,
	float bias
) {
	SSAO result;

	if(!normalTexture || !positionTexture || !projectionMatrix) return result;

	auto kernel = makeSSAOKernelUniform();
	auto noiseTex = makeSSAONoiseTexture();

	result.radius = new osg::Uniform("ssaoRadius", radius);
	result.bias = new osg::Uniform("ssaoBias", bias);

	auto rawTex = osgx::make_ref<osg::Texture2D>();

	rawTex->setTextureSize(width, height);
	rawTex->setInternalFormat(GL_R8);
	rawTex->setSourceFormat(GL_RED);
	rawTex->setSourceType(GL_UNSIGNED_BYTE);
	rawTex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	rawTex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	rawTex->setDataVariance(osg::Object::DYNAMIC);

	const std::array<osgx::detail::TextureInput, 3> rawTextures = {{
		{0, normalTexture, "gNormal"},
		{1, noiseTex, "ssaoNoise"},
		{2, positionTexture, "gPosition"}
	}};

	result.rawCamera = makeFullscreenRTTPass(
		"osgx_ssao_Raw", rawTextures, rawTex, SSAO_FRAGMENT_SHADER_SRC, width, height
	);

	auto* rawSS = result.rawCamera->getOrCreateStateSet();

	rawSS->addUniform(kernel);
	rawSS->addUniform(projectionMatrix);
	rawSS->addUniform(result.radius);
	rawSS->addUniform(result.bias);

	result.aoTexture = osgx::make_ref<osg::Texture2D>();
	result.aoTexture->setTextureSize(width, height);
	result.aoTexture->setInternalFormat(GL_R8);
	result.aoTexture->setSourceFormat(GL_RED);
	result.aoTexture->setSourceType(GL_UNSIGNED_BYTE);
	result.aoTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	result.aoTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	result.aoTexture->setDataVariance(osg::Object::DYNAMIC);

	const std::array<osgx::detail::TextureInput, 1> blurTextures = {{{0, rawTex, "ssaoRawTex"}}};

	result.blurCamera = makeFullscreenRTTPass(
		"osgx_ssao_Blur", blurTextures, result.aoTexture, SSAO_BLUR_FRAGMENT_SHADER_SRC, width, height
	);

	return result;
}

}
