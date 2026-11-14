#include "osgx/GTAO.hpp"

// XEGTAO_COMMON_SRC/XEGTAO_NOISE_SRC/XEGTAO_PREFILTER_SRC/XEGTAO_MAIN_PASS_SRC/XEGTAO_DENOISE_SRC:
// XeGTAO, ported to GLSL.
#include "GTAOShaders.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BindImageTexture>
#include <osg/GL>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <string>

namespace osgx {

namespace {

constexpr int DEPTH_MIP_LEVELS = 5;

// The prefilter's 8x8 threads each cover a 2x2 block (16x16 pixels per workgroup); the denoise
// passes' 8x8 threads each cover 2 horizontal pixels (16x8).
constexpr int PREFILTER_PIXELS_PER_GROUP = 16;
constexpr int MAIN_PASS_PIXELS_PER_GROUP = 8;
constexpr int DENOISE_PIXELS_PER_GROUP_X = 16;
constexpr int DENOISE_PIXELS_PER_GROUP_Y = 8;

constexpr int PREFILTER_RENDER_BIN = -5;
constexpr int MAIN_PASS_RENDER_BIN = -4;
constexpr int DENOISE_RENDER_BIN = -3;
constexpr int FINAL_DENOISE_RENDER_BIN = -1;

constexpr unsigned int MAX_DENOISE_PASSES = 3;

// XeGTAO's GTAOSettings defaults and the denoise blur strength GTAOUpdateConstants() derives.
constexpr float DEFAULT_RADIUS_MULTIPLIER = 1.457f;
constexpr float DEFAULT_FALLOFF_RANGE = 0.615f;
constexpr float DEFAULT_SAMPLE_DISTRIBUTION_POWER = 2.0f;
constexpr float DEFAULT_THIN_OCCLUDER_COMPENSATION = 0.0f;
constexpr float DEFAULT_FINAL_VALUE_POWER = 2.2f;
constexpr float DEFAULT_DEPTH_MIP_SAMPLING_OFFSET = 3.30f;
constexpr float DENOISE_BLUR_BETA = 1.2f;
constexpr float DENOISE_DISABLED_BLUR_BETA = 1e4f;

constexpr const char SHADER_HEADER[] = R"GLSL(#version 460 core
)GLSL";

// GTAOUpdateConstants(), evaluated in GLSL against the live projection. The NDC-to-view terms map
// XeGTAO's top-down screen UV (0..1) and positive view depth onto osgx's eye space, mirrored in z:
// x = depth * (ndcX + P20) / P00 and y = depth * (ndcY + P21) / P11, with ndcY = 1 - 2 * uvY.
// Includes the off-center terms P20/P21, which XeGTAO itself assumes are 0.
constexpr const char CONSTANTS_SRC[] = R"GLSL(
uniform mat4 projectionMatrix;
uniform ivec2 gtao_viewportSize;
uniform float gtao_radius;
uniform float gtao_radiusMultiplier;
uniform float gtao_falloffRange;
uniform float gtao_sampleDistributionPower;
uniform float gtao_thinOccluderCompensation;
uniform float gtao_finalValuePower;
uniform float gtao_depthMipSamplingOffset;
uniform float gtao_denoiseBlurBeta;
uniform int gtao_noiseIndex;

// XeGTAO_ClampDepth()'s upper bound: background pixels end up at exactly this depth.
const float GTAO_BACKGROUND_DEPTH = 65504.0;

GTAOConstants osgx_gtao_constants() {
	GTAOConstants consts;

	consts.ViewportSize = gtao_viewportSize;
	consts.ViewportPixelSize = 1.0 / vec2(gtao_viewportSize);
	consts.NDCToViewMul = vec2(2.0 / projectionMatrix[0][0], -2.0 / projectionMatrix[1][1]);
	consts.NDCToViewAdd = vec2(
		(projectionMatrix[2][0] - 1.0) / projectionMatrix[0][0],
		(projectionMatrix[2][1] + 1.0) / projectionMatrix[1][1]
	);
	consts.NDCToViewMul_x_PixelSize = consts.NDCToViewMul * consts.ViewportPixelSize;
	consts.EffectRadius = gtao_radius;
	consts.EffectFalloffRange = gtao_falloffRange;
	consts.RadiusMultiplier = gtao_radiusMultiplier;
	consts.FinalValuePower = gtao_finalValuePower;
	consts.DenoiseBlurBeta = gtao_denoiseBlurBeta;
	consts.SampleDistributionPower = gtao_sampleDistributionPower;
	consts.ThinOccluderCompensation = gtao_thinOccluderCompensation;
	consts.DepthMIPSamplingOffset = gtao_depthMipSamplingOffset;
	consts.NoiseIndex = gtao_noiseIndex;

	return consts;
}
)GLSL";

// Reads the G-buffer at XeGTAO's top-down row; clamped like XeGTAO's point-clamp sampler.
// Background (position 0) becomes "infinitely" far, which XeGTAO_ClampDepth() then clamps.
constexpr const char PREFILTER_CALLBACKS[] = R"GLSL(
uniform sampler2D gPosition;

layout(r32f, binding = 0) uniform restrict writeonly image2D xegtao_outWorkingDepth[5];

float XeGTAO_LoadViewspaceDepth(ivec2 pixCoord) {
	ivec2 size = textureSize(gPosition, 0);
	ivec2 p = clamp(pixCoord, ivec2(0), size - 1);
	float depth = -texelFetch(gPosition, ivec2(p.x, size.y - 1 - p.y), 0).z;

	return depth > 0.0 ? depth : 3.402823466e+38;
}
)GLSL";

constexpr const char PREFILTER_MAIN[] = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	XeGTAO_PrefilterDepths16x16(gl_GlobalInvocationID.xy, gl_LocalInvocationID.xy, osgx_gtao_constants());
}
)GLSL";

// The normal at XeGTAO's top-down row, mirrored into its +z-forward view space. Background has no
// normal (0); any unit vector keeps its (overwritten) result finite.
constexpr const char MAIN_PASS_CALLBACKS[] = R"GLSL(
uniform sampler2D xegtao_srcWorkingDepth;
uniform sampler2D gNormal;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
layout(r32ui, binding = 0) uniform restrict writeonly uimage2D xegtao_outWorkingAOTerm;
#else
layout(r8ui, binding = 0) uniform restrict writeonly uimage2D xegtao_outWorkingAOTerm;
#endif

layout(r8, binding = 1) uniform restrict writeonly image2D xegtao_outWorkingEdges;

vec3 osgx_gtao_loadNormal(ivec2 pixCoord) {
	ivec2 size = textureSize(gNormal, 0);
	vec3 normal = texelFetch(gNormal, ivec2(pixCoord.x, size.y - 1 - pixCoord.y), 0).xyz;

	return dot(normal, normal) > 0.0 ? normalize(vec3(normal.xy, -normal.z)) : vec3(0.0, 0.0, -1.0);
}
)GLSL";

constexpr const char MAIN_PASS_MAIN[] = R"GLSL(
// vaGTAO.hlsl's CSGTAOLow/Medium/High/Ultra entry points.
#if OSGX_GTAO_QUALITY == 0
	#define OSGX_GTAO_SLICE_COUNT 1.0
	#define OSGX_GTAO_STEPS_PER_SLICE 2.0
#elif OSGX_GTAO_QUALITY == 1
	#define OSGX_GTAO_SLICE_COUNT 2.0
	#define OSGX_GTAO_STEPS_PER_SLICE 2.0
#elif OSGX_GTAO_QUALITY == 3
	#define OSGX_GTAO_SLICE_COUNT 9.0
	#define OSGX_GTAO_STEPS_PER_SLICE 3.0
#else
	#define OSGX_GTAO_SLICE_COUNT 3.0
	#define OSGX_GTAO_STEPS_PER_SLICE 3.0
#endif

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	const uvec2 pixCoord = gl_GlobalInvocationID.xy;
	const GTAOConstants consts = osgx_gtao_constants();

	if(any(greaterThanEqual(pixCoord, uvec2(consts.ViewportSize)))) return;

	XeGTAO_MainPass(
		pixCoord,
		OSGX_GTAO_SLICE_COUNT,
		OSGX_GTAO_STEPS_PER_SLICE,
		XeGTAO_SpatioTemporalNoise(pixCoord, uint(consts.NoiseIndex)),
		osgx_gtao_loadNormal(ivec2(pixCoord)),
		consts
	);
}
)GLSL";

// Intermediate passes write the next working term; the final pass writes osgx's outputs at
// osgx's bottom-up row, 1.0 (and a zero bent normal) on background, the bent normal mirrored back
// into osgx's eye space. A raw visibility above 1.0 (the final pass's XE_GTAO_OCCLUSION_TERM_SCALE
// rescale) saturates in the GL_R8 store.
constexpr const char DENOISE_CALLBACKS[] = R"GLSL(
uniform usampler2D xegtao_srcWorkingAOTerm;
uniform sampler2D xegtao_srcWorkingEdges;

#ifdef XE_GTAO_FINAL_APPLY
uniform sampler2D xegtao_srcWorkingDepth;

layout(r8, binding = 0) uniform restrict writeonly image2D gtao_output;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
layout(rgba8_snorm, binding = 1) uniform restrict writeonly image2D gtao_bentNormalOutput;
#endif

void XeGTAO_WriteOutput(uvec2 pixCoord, float visibility, vec3 bentNormal) {
	ivec2 size = imageSize(gtao_output);

	if(any(greaterThanEqual(pixCoord, uvec2(size)))) return;

	ivec2 outputCoord = ivec2(pixCoord.x, size.y - 1 - int(pixCoord.y));
	bool background = texelFetch(xegtao_srcWorkingDepth, ivec2(pixCoord), 0).r >= GTAO_BACKGROUND_DEPTH;

	visibility = background ? 1.0 : visibility;

	imageStore(gtao_output, outputCoord, vec4(visibility));

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	vec3 direction = background ? vec3(0.0) : vec3(bentNormal.xy, -bentNormal.z);

	imageStore(gtao_bentNormalOutput, outputCoord, vec4(direction, min(visibility, 1.0)));
#endif
}
#else
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
layout(r32ui, binding = 0) uniform restrict writeonly uimage2D xegtao_outWorkingAOTerm;
#else
layout(r8ui, binding = 0) uniform restrict writeonly uimage2D xegtao_outWorkingAOTerm;
#endif

void XeGTAO_WriteOutput(uvec2 pixCoord, float visibility, vec3 bentNormal) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	imageStore(xegtao_outWorkingAOTerm, ivec2(pixCoord), uvec4(XeGTAO_EncodeVisibilityBentNormal(visibility, bentNormal)));
#else
	imageStore(xegtao_outWorkingAOTerm, ivec2(pixCoord), uvec4(uint(visibility * 255.0 + 0.5)));
#endif
}
#endif
)GLSL";

// vaGTAO.hlsl's CSDenoisePass/CSDenoiseLastPass: 2 horizontal pixels per thread.
constexpr const char DENOISE_MAIN[] = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	const uvec2 pixCoordBase = gl_GlobalInvocationID.xy * uvec2(2, 1);

	XeGTAO_Denoise(pixCoordBase, osgx_gtao_constants(), XE_GTAO_FINAL_APPLY_VALUE);
}
)GLSL";

std::string commonSource(bool bentNormal) {
	std::string src = SHADER_HEADER;

	if(bentNormal) src += "#define XE_GTAO_COMPUTE_BENT_NORMALS\n";

	src += detail::XEGTAO_COMMON_SRC;
	src += CONSTANTS_SRC;

	return src;
}

std::string prefilterSource() {
	std::string src = commonSource(false);

	src += PREFILTER_CALLBACKS;
	src += detail::XEGTAO_PREFILTER_SRC;
	src += PREFILTER_MAIN;

	return src;
}

std::string mainPassSource(bool bentNormal) {
	std::string src = commonSource(bentNormal);

	src += R"GLSL(
#pragma import_defines(OSGX_GTAO_QUALITY)
)GLSL";
	src += detail::XEGTAO_NOISE_SRC;
	src += MAIN_PASS_CALLBACKS;
	src += detail::XEGTAO_MAIN_PASS_SRC;
	src += MAIN_PASS_MAIN;

	return src;
}

std::string denoiseSource(bool bentNormal, bool finalApply) {
	std::string src = commonSource(bentNormal);

	if(finalApply) src += "#define XE_GTAO_FINAL_APPLY\n";

	src += DENOISE_CALLBACKS;
	src += detail::XEGTAO_DENOISE_SRC;
	src += DENOISE_MAIN;

	return src;
}

// A point-sampled, clamped working texture (XeGTAO's g_samplerPointClamp).
osg::ref_ptr<osg::Texture2D> makeWorkingTexture(
	const std::string& name,
	int width,
	int height,
	GLint internalFormat,
	GLenum sourceFormat,
	GLenum sourceType
) {
	auto texture = ComputePass::createTexture(name, width, height, internalFormat);

	texture->setSourceFormat(sourceFormat);
	texture->setSourceType(sourceType);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);

	return texture;
}

}

bool GTAO::valid() const {
	return node.valid() &&
		prefilterPass.valid() &&
		mainPass.valid() &&
		finalDenoisePass.valid() &&
		aoTexture.valid();
}

void GTAO::setQuality(Quality quality) {
	if(!mainPass.valid()) return;

	mainPass->getOrCreateStateSet()->setDefine("OSGX_GTAO_QUALITY", std::to_string(static_cast<int>(quality)));
}

GTAO::Quality GTAO::getQuality() const {
	if(!mainPass.valid() || !mainPass->getStateSet()) return Quality::HIGH;

	const auto* define = mainPass->getStateSet()->getDefinePair("OSGX_GTAO_QUALITY");

	return define ? static_cast<Quality>(std::stoi(define->first)) : Quality::HIGH;
}

void GTAO::setDenoisePasses(unsigned int passes) {
	if(!finalDenoisePass.valid() || !_denoiseBlurBeta.valid()) return;

	_denoisePasses = std::min(passes, MAX_DENOISE_PASSES);

	// Even without denoising a single last pass runs, to write the outputs (blur disabled).
	const unsigned int intermediatePasses = std::max(_denoisePasses, 1u) - 1;

	for(unsigned int i = 0; i < denoisePasses.size(); i++) {
		denoisePasses[i]->setNodeMask(i < intermediatePasses ? ~0u : 0u);
	}

	// The main pass writes workingTextures[0]; each intermediate pass flips to the other one.
	finalDenoisePass->bindTexture(0, workingTextures[intermediatePasses % 2], "xegtao_srcWorkingAOTerm");

	_denoiseBlurBeta->set(_denoisePasses == 0 ? DENOISE_DISABLED_BLUR_BETA : DENOISE_BLUR_BETA);
}

unsigned int GTAO::getDenoisePasses() const {
	return _denoisePasses;
}

GTAO GTAO::create(
	osg::Texture2D* normalTexture,
	osg::Texture2D* positionTexture,
	osg::Uniform* projectionMatrix,
	int width,
	int height
) {
	return create(normalTexture, positionTexture, projectionMatrix, width, height, Options());
}

GTAO GTAO::create(
	osg::Texture2D* normalTexture,
	osg::Texture2D* positionTexture,
	osg::Uniform* projectionMatrix,
	int width,
	int height,
	const Options& options
) {
	GTAO result;

	if(!normalTexture || !positionTexture || !projectionMatrix || width <= 0 || height <= 0) return result;

	result.depthTexture = makeWorkingTexture("osgx_gtao_Depth", width, height, GL_R32F, GL_RED, GL_FLOAT);
	result.depthTexture->setNumMipmapLevels(DEPTH_MIP_LEVELS);
	result.depthTexture->setUseHardwareMipMapGeneration(false);
	result.depthTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST_MIPMAP_NEAREST);

	result.edgesTexture = makeWorkingTexture("osgx_gtao_Edges", width, height, GL_R8, GL_RED, GL_UNSIGNED_BYTE);

	const GLint workingFormat = options.bentNormal ? GL_R32UI : GL_R8UI;
	const GLenum workingType = options.bentNormal ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE;

	for(std::size_t i = 0; i < result.workingTextures.size(); i++) {
		result.workingTextures[i] = makeWorkingTexture(
			"osgx_gtao_Working" + std::to_string(i),
			width,
			height,
			workingFormat,
			GL_RED_INTEGER_EXT,
			workingType
		);
	}

	result.aoTexture = ComputePass::createTexture("osgx_gtao_AO", width, height, GL_R8);

	if(options.bentNormal) {
		result.bentNormalTexture = ComputePass::createTexture(
			"osgx_gtao_BentNormal",
			width,
			height,
			GL_RGBA8_SNORM
		);
	}

	result.radius = new osg::Uniform("gtao_radius", options.radius);
	result.radiusMultiplier = new osg::Uniform("gtao_radiusMultiplier", DEFAULT_RADIUS_MULTIPLIER);
	result.falloffRange = new osg::Uniform("gtao_falloffRange", DEFAULT_FALLOFF_RANGE);
	result.sampleDistributionPower = new osg::Uniform(
		"gtao_sampleDistributionPower",
		DEFAULT_SAMPLE_DISTRIBUTION_POWER
	);
	result.thinOccluderCompensation = new osg::Uniform(
		"gtao_thinOccluderCompensation",
		DEFAULT_THIN_OCCLUDER_COMPENSATION
	);
	result.finalValuePower = new osg::Uniform("gtao_finalValuePower", DEFAULT_FINAL_VALUE_POWER);
	result.depthMipSamplingOffset = new osg::Uniform(
		"gtao_depthMipSamplingOffset",
		DEFAULT_DEPTH_MIP_SAMPLING_OFFSET
	);
	result.noiseIndex = new osg::Uniform("gtao_noiseIndex", 0);
	result._denoiseBlurBeta = new osg::Uniform("gtao_denoiseBlurBeta", DENOISE_BLUR_BETA);

	// Every pass builds the same GTAOConstants, so the uniforms live on the parent node.
	result.node = osgx::make_nref<osg::Group>("osgx_gtao");

	auto* nodeSS = result.node->getOrCreateStateSet();

	nodeSS->addUniform(projectionMatrix);
	nodeSS->addUniform(new osg::Uniform("gtao_viewportSize", width, height));
	nodeSS->addUniform(result.radius);
	nodeSS->addUniform(result.radiusMultiplier);
	nodeSS->addUniform(result.falloffRange);
	nodeSS->addUniform(result.sampleDistributionPower);
	nodeSS->addUniform(result.thinOccluderCompensation);
	nodeSS->addUniform(result.finalValuePower);
	nodeSS->addUniform(result.depthMipSamplingOffset);
	nodeSS->addUniform(result.noiseIndex);
	nodeSS->addUniform(result._denoiseBlurBeta);

	// Prefilter: G-buffer position -> depthTexture's 5 mips.
	result.prefilterPass = new ComputePass(
		"osgx_gtao_PrefilterPass",
		ComputePass::createProgram("osgx_gtao_PrefilterPass", prefilterSource())
	);
	result.prefilterPass->setComputeGroupsFor(
		width,
		height,
		PREFILTER_PIXELS_PER_GROUP,
		PREFILTER_PIXELS_PER_GROUP
	);
	result.prefilterPass->setRenderBin(PREFILTER_RENDER_BIN);
	result.prefilterPass->bindTexture(0, positionTexture, "gPosition");

	for(int level = 0; level < DEPTH_MIP_LEVELS; level++) {
		result.prefilterPass->bindImage(
			static_cast<GLuint>(level),
			result.depthTexture,
			osg::BindImageTexture::WRITE_ONLY,
			GL_R32F,
			level
		);
	}

	// Main: depthTexture + G-buffer normal -> workingTextures[0] + edgesTexture.
	result.mainPass = new ComputePass(
		"osgx_gtao_MainPass",
		ComputePass::createProgram("osgx_gtao_MainPass", mainPassSource(options.bentNormal))
	);
	result.mainPass->setComputeGroupsFor(
		width,
		height,
		MAIN_PASS_PIXELS_PER_GROUP,
		MAIN_PASS_PIXELS_PER_GROUP
	);
	result.mainPass->setRenderBin(MAIN_PASS_RENDER_BIN);
	result.mainPass->bindTexture(0, result.depthTexture, "xegtao_srcWorkingDepth");
	result.mainPass->bindTexture(1, normalTexture, "gNormal");
	result.mainPass->bindImage(
		0,
		result.workingTextures[0],
		osg::BindImageTexture::WRITE_ONLY,
		static_cast<GLenum>(workingFormat)
	);
	result.mainPass->bindImage(1, result.edgesTexture, osg::BindImageTexture::WRITE_ONLY, GL_R8);

	result.setQuality(options.quality);

	// Denoise: workingTextures[0] -> [1] -> [0] (as many as are enabled), then the final pass into
	// aoTexture (+ bentNormalTexture).
	auto denoiseProgram = ComputePass::createProgram(
		"osgx_gtao_DenoisePass",
		denoiseSource(options.bentNormal, false)
	);

	for(std::size_t i = 0; i < result.denoisePasses.size(); i++) {
		auto pass = osgx::make_ref<ComputePass>("osgx_gtao_DenoisePass" + std::to_string(i), denoiseProgram);

		pass->setComputeGroupsFor(width, height, DENOISE_PIXELS_PER_GROUP_X, DENOISE_PIXELS_PER_GROUP_Y);
		pass->setRenderBin(DENOISE_RENDER_BIN + static_cast<int>(i));
		pass->bindTexture(0, result.workingTextures[i], "xegtao_srcWorkingAOTerm");
		pass->bindTexture(1, result.edgesTexture, "xegtao_srcWorkingEdges");
		pass->bindImage(
			0,
			result.workingTextures[(i + 1) % 2],
			osg::BindImageTexture::WRITE_ONLY,
			static_cast<GLenum>(workingFormat)
		);

		result.denoisePasses[i] = pass;
	}

	result.finalDenoisePass = new ComputePass(
		"osgx_gtao_FinalDenoisePass",
		ComputePass::createProgram("osgx_gtao_FinalDenoisePass", denoiseSource(options.bentNormal, true))
	);
	result.finalDenoisePass->setComputeGroupsFor(
		width,
		height,
		DENOISE_PIXELS_PER_GROUP_X,
		DENOISE_PIXELS_PER_GROUP_Y
	);
	result.finalDenoisePass->setRenderBin(FINAL_DENOISE_RENDER_BIN);
	result.finalDenoisePass->bindTexture(1, result.edgesTexture, "xegtao_srcWorkingEdges");
	result.finalDenoisePass->bindTexture(2, result.depthTexture, "xegtao_srcWorkingDepth");
	result.finalDenoisePass->bindImage(0, result.aoTexture, osg::BindImageTexture::WRITE_ONLY, GL_R8);

	if(options.bentNormal) {
		result.finalDenoisePass->bindImage(
			1,
			result.bentNormalTexture,
			osg::BindImageTexture::WRITE_ONLY,
			GL_RGBA8_SNORM
		);
	}

	result.node->addChild(result.prefilterPass);
	result.node->addChild(result.mainPass);
	result.node->addChild(result.denoisePasses[0]);
	result.node->addChild(result.denoisePasses[1]);
	result.node->addChild(result.finalDenoisePass);

	// Binds the final pass's input texture.
	result.setDenoisePasses(options.denoisePasses);

	return result;
}

}
