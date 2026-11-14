#include "osgx/CORTAO.hpp"

// CORTAO_BASE_SRC/CORTAO_PREPASS_SRC/CORTAO_MAIN_PASS_SRC/CORTAO_FILTER_PASS_SRC: the reference
// shaders, ported to plain GLSL.
#include "CORTAOShaders.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BindImageTexture>
#include <osg/GL>
#include <osg/Math>
#include <osg/Notify>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace osgx {

namespace {

// Must match the occluder lookup table createOccluderLookupTable() builds below (16 rays ->
// GL_R16UI).
constexpr unsigned int RAY_COUNT = 16;

constexpr unsigned int MAX_MIP_COUNT = 8;
constexpr int PREPASS_WORKGROUP_SIZE = 8;
constexpr int MAIN_PASS_WORKGROUP_SIZE = 16;
// Each filter pass thread covers a 2x2 pixel block.
constexpr int FILTER_PASS_WORKGROUP_SIZE = 8;

// The main camera's render stage draws RenderBins in ascending order; all dispatches sort ahead
// of default-bin (0) content in whatever stage traverses CORTAO::node, in pass order.
constexpr int PREPASS_RENDER_BIN = -3;
constexpr int MAIN_PASS_RENDER_BIN = -2;
constexpr int FILTER_PASS_RENDER_BIN = -1;

// Shared header for every pass. GL_EXT_control_flow_attributes is what CORTAO_BASE_SRC checks for
// [[unroll]]; desktop GL drivers generally don't expose it, so the NVIDIA pragma asks for the same
// unrolling there (ignored elsewhere).
constexpr const char SHADER_HEADER[] = R"GLSL(#version 460 core

#extension GL_EXT_control_flow_attributes : enable
#pragma optionNV(unroll all)
)GLSL";

constexpr const char PREPASS_CALLBACKS[] = R"GLSL(
uniform sampler2D gNormal;
uniform sampler2D gPosition;
uniform uint cortao_randomSeed;
uniform uint cortao_mipCount;

layout(r32ui, binding = 0) uniform restrict writeonly uimage2D cortao_packedMips[8];

// Linear depth from eye-space position, returned as its reciprocal: with depthScale 1 and
// depthOffset 0 below, CORTAO's own perspective linearization (1.0 / (depth * scale + offset))
// turns it back into linear depth. Background (position 0) stays 0, which is CORTAO's own sky
// value for that parameter combination.
float cortao_loadDepth(ivec2 pixelCoords) {
	float z = -texelFetch(gPosition, pixelCoords, 0).z;

	return z > 0.0 ? 1.0 / z : 0.0;
}

vec3 cortao_loadNormal(ivec2 pixelCoords) {
	return normalize(texelFetch(gNormal, pixelCoords, 0).xyz);
}

void cortao_writePackedData(ivec2 pixelCoords, int mipLevel, uint data) {
	imageStore(cortao_packedMips[mipLevel], pixelCoords, uvec4(data, 0u, 0u, 0u));
}

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	uvec2 resolution = uvec2(textureSize(gPosition, 0));

	CORTAOPrepassParameters params;

	params.packedResolution = resolution.x | (resolution.y << 16u);
	params.depthScale = 1.0;
	params.depthOffset = 0.0;
	params.randomSeed = cortao_randomSeed;
	params.mipCount = cortao_mipCount;
	params._padding0 = 0u;
	params._padding1 = 0u;
	params._padding2 = 0u;

	cortao_prepass(gl_GlobalInvocationID.xy, params);
}
)GLSL";

constexpr const char MAIN_PASS_CALLBACKS[] = R"GLSL(
uniform usampler2D cortao_packedData;
uniform sampler2D gNormal;
uniform usampler3D cortao_occluderLookupTable;

layout(r8, binding = 0) uniform restrict writeonly image2D cortao_output;

#if OSGX_CORTAO_BENT_NORMAL
layout(rgba8_snorm, binding = 1) uniform restrict writeonly image2D cortao_bentNormalOutput;
#endif

uniform mat4 projectionMatrix;
uniform uint cortao_randomSeed;
uniform float cortao_maxRadius;
uniform float cortao_maxScreenspaceRadiusHeightRatio;
uniform float cortao_mipBias;
uniform float cortao_maxSlopeCompensation;
uniform float cortao_backfaceThreshold;
uniform float cortao_backfaceBoost;

uint cortao_samplePackedData(vec2 texCoords, float mipmap) {
	return textureLod(cortao_packedData, texCoords, mipmap).r;
}

vec3 cortao_loadNormal(ivec2 pixelCoords) {
	return normalize(texelFetch(gNormal, pixelCoords, 0).xyz);
}

uint cortao_sampleOccluderLookupTable(vec3 texCoords) {
	return textureLod(cortao_occluderLookupTable, texCoords, 0.0).r;
}

void cortao_writeResult(
	ivec2 pixelCoords,
	float occlusion,
	vec3 bentNormal,
	mat3 tangentSpaceTransform,
	uint rayBits
) {
	imageStore(cortao_output, pixelCoords, vec4(occlusion, 0.0, 0.0, 0.0));

#if OSGX_CORTAO_BENT_NORMAL
	// Every ray occluded leaves the bent normal undefined (normalize of a zero sum); fall back to
	// the surface normal, the tangent frame's third column.
	vec3 direction = occlusion > 0.0 ? vec3(bentNormal) : vec3(tangentSpaceTransform[2]);

	imageStore(cortao_bentNormalOutput, pixelCoords, vec4(direction, occlusion));
#endif
}

// Morton-order thread placement inside the workgroup, for cache coherency. The reference main pass
// wrapper omits the initial 0x5555 mask, which maps a 16x16 workgroup's 256 threads onto only 64
// distinct pixels.
uvec2 cortao_deinterleave(uint index) {
	uvec2 p = (uvec2(index, index) >> uvec2(0, 1)) & 0x5555u;

	p = (p ^ (p >> 1u)) & 0x3333u;
	p = (p ^ (p >> 2u)) & 0x0f0fu;
	p = (p ^ (p >> 4u)) & 0x00ffu;

	return p;
}

layout(local_size_x = 16, local_size_y = 16) in;

void main() {
	uvec2 resolution = uvec2(imageSize(cortao_output));
	uvec2 pixelCoords = gl_WorkGroupID.xy * gl_WorkGroupSize.xy + cortao_deinterleave(gl_LocalInvocationIndex);

	if(any(greaterThanEqual(pixelCoords, resolution))) return;

	// cortao_mainPass() returns without writing for background pixels; write "unoccluded" for
	// those here so aoTexture never holds a stale value there.
	if(texelFetch(cortao_packedData, ivec2(pixelCoords), 0).r == 0u) {
		imageStore(cortao_output, ivec2(pixelCoords), vec4(1.0));

#if OSGX_CORTAO_BENT_NORMAL
		imageStore(cortao_bentNormalOutput, ivec2(pixelCoords), vec4(0.0, 0.0, 0.0, 1.0));
#endif

		return;
	}

	// CORTAOParameters.h's populateMainPassParameters(), evaluated against the live projection
	// (flipY = false for GL). P[2][0]/P[2][1] are the off-center terms; none of these depend on
	// near/far.
	mat4 inverseProjection = inverse(projectionMatrix);

	float projX = projectionMatrix[0][0];
	float projY = projectionMatrix[1][1];
	float projOffsetX = projectionMatrix[2][0];
	float projOffsetY = projectionMatrix[2][1];

	float invX = inverseProjection[0][0];
	float invY = inverseProjection[1][1];
	float invOffsetX = inverseProjection[3][0];
	float invOffsetY = inverseProjection[3][1];

	float maxApparentCircleRadius = invY * cortao_maxScreenspaceRadiusHeightRatio * 2.0;

	CORTAOMainPassParameters params;

	params.packedResolution = resolution.x | (resolution.y << 16u);
	params.randomSeed = cortao_randomSeed;
	params.normalBias = invX / float(resolution.x) * sqrt(2.0);
	params.radius = cortao_maxRadius;
	params.unprojectParams = vec4(2.0 * invX, 2.0 * invY, -invX + invOffsetX, -invY + invOffsetY);
	params.projectionParams = vec4(
		0.5 * projX,
		0.5 * projY,
		-0.5 + 0.5 * projOffsetX,
		-0.5 + 0.5 * projOffsetY
	);
	params.maxApparentCircleRadiusCoefficient = maxApparentCircleRadius / sqrt(
		maxApparentCircleRadius * maxApparentCircleRadius + 1.0
	);
	params.mipBias = log2(projY * float(resolution.y) * 0.5) + cortao_mipBias;
	params.inverseMaxSlopeCompensation = 1.0 / cortao_maxSlopeCompensation;
	params.backfaceThreshold = cortao_backfaceThreshold;
	params.backfaceBoost = cortao_backfaceBoost;
	params._padding0 = 0u;
	params._padding1 = 0u;
	params._padding2 = 0u;

	cortao_mainPass(pixelCoords, params);
}
)GLSL";

constexpr const char FILTER_PASS_CALLBACKS[] = R"GLSL(
uniform usampler2D cortao_packedData;
uniform sampler2D cortao_rawOcclusion;

layout(r8, binding = 0) uniform restrict writeonly image2D cortao_filterOutput;

uniform float cortao_filterDepthSensitivity;
uniform float cortao_filterNormalSensitivity;

uvec4 cortao_gatherPackedData(vec2 texCoords, ivec2 offset) {
	return textureGatherOffset(cortao_packedData, texCoords, offset);
}

vec4 cortao_gatherSSAO(vec2 texCoords, ivec2 offset) {
	return textureGatherOffset(cortao_rawOcclusion, texCoords, offset);
}

void cortao_writeFilterOutput(ivec2 pixelCoords, float ssao) {
	imageStore(cortao_filterOutput, pixelCoords, vec4(ssao, 0.0, 0.0, 0.0));
}

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	uvec2 resolution = uvec2(imageSize(cortao_filterOutput));

	// cortao_filterPass() only writes non-background pixels of this thread's 2x2 block; write
	// "unoccluded" to all four first so background never holds a stale value (the filter's own
	// stores, later in the same invocation, win).
	uvec2 blockMax = gl_GlobalInvocationID.xy * 2u + 1u;

	for(uint i = 0u; i < 4u; i++) {
		uvec2 p = blockMax - uvec2(i & 1u, i >> 1u);

		if(all(lessThan(p, resolution))) imageStore(cortao_filterOutput, ivec2(p), vec4(1.0));
	}

	CORTAOFilterPassParameters params;

	params.packedResolution = resolution.x | (resolution.y << 16u);
	params.depthSensitivity = cortao_filterDepthSensitivity;
	params.normalSensitivity = cortao_filterNormalSensitivity;
	params._padding0 = 0u;

	cortao_filterPass(gl_GlobalInvocationID.xy, params);
}
)GLSL";

std::string prepassSource() {
	std::string src = SHADER_HEADER;

	src += R"GLSL(
#define CORTAO_PERSPECTIVE_PROJECTION 1
)GLSL";
	src += detail::CORTAO_BASE_SRC;
	src += detail::CORTAO_PREPASS_SRC;
	src += PREPASS_CALLBACKS;

	return src;
}

std::string mainPassSource(bool bentNormal) {
	std::string src = SHADER_HEADER;

	src += R"GLSL(
#pragma import_defines(CORTAO_SAMPLE_COUNT, OSGX_CORTAO_TILED_NOISE)

#define CORTAO_PERSPECTIVE_PROJECTION 1
#define CORTAO_OCCLUDER_LOOKUP_TABLE 1
#define OSGX_CORTAO_BENT_NORMAL )GLSL";
	src += bentNormal ? "1" : "0";
	src += R"GLSL(
#define CORTAO_RAY_COUNT )GLSL";
	src += std::to_string(RAY_COUNT);
	src += R"GLSL(

#ifndef CORTAO_SAMPLE_COUNT
	#define CORTAO_SAMPLE_COUNT 16
#endif
)GLSL";
	src += detail::CORTAO_BASE_SRC;
	// Filter::TILED: every per-pixel random value of the main pass comes from
	// cortao_seed(pixelCoords); masking the coordinates repeats them every 4x4 pixels. Defined after
	// CORTAO_BASE_SRC (so cortao_seed() itself is untouched) and, being self-referential, not
	// re-expanded: the inner call is the real function.
	src += R"GLSL(
#ifdef OSGX_CORTAO_TILED_NOISE
	#define cortao_seed(c) cortao_seed((c) & 3u)
#endif
)GLSL";
	src += detail::CORTAO_MAIN_PASS_SRC;
	src += MAIN_PASS_CALLBACKS;

	return src;
}

// CORTAO_LARGE_FILTER/CORTAO_FILTER_SYMMETRY default to 0 inside CORTAO_FILTER_PASS_SRC when
// absent, so the StateSet defines only ever need the value "1" (setDefine() vs. removeDefine()).
std::string filterPassSource() {
	std::string src = SHADER_HEADER;

	src += R"GLSL(
#pragma import_defines(CORTAO_LARGE_FILTER, CORTAO_FILTER_SYMMETRY)
)GLSL";
	src += detail::CORTAO_BASE_SRC;
	src += detail::CORTAO_FILTER_PASS_SRC;
	src += FILTER_PASS_CALLBACKS;

	return src;
}

// Filter::TILED (and NONE, as its copy variant): per pixel, a depth/normal-aware average of the raw
// occlusion over the 4x4 window at offsets -2..+1, which holds each of the main pass's 16 tile
// phases exactly once. Depth error is measured against the center's local depth plane (one-sided
// screen-space slopes, the flatter side), so slanted surfaces still average fully.
constexpr const char TILED_FILTER_SHADER[] = R"GLSL(
#pragma import_defines(OSGX_CORTAO_FILTER_COPY)

uniform usampler2D cortao_packedData;
uniform sampler2D cortao_rawOcclusion;

layout(r8, binding = 0) uniform restrict writeonly image2D cortao_filterOutput;

uniform float cortao_filterDepthSensitivity;
uniform float cortao_filterNormalSensitivity;

float osgx_cortao_depthAt(ivec2 p, ivec2 resolution) {
	return cortao_unpackDepth(texelFetch(cortao_packedData, clamp(p, ivec2(0), resolution - 1), 0).r);
}

// The flatter of the two one-sided depth differences along `step`; a background (0) neighbor
// never wins.
float osgx_cortao_slope(ivec2 p, ivec2 step, float center, ivec2 resolution) {
	float before = osgx_cortao_depthAt(p - step, resolution);
	float after = osgx_cortao_depthAt(p + step, resolution);
	float a = before > 0.0 ? center - before : 1e30;
	float b = after > 0.0 ? after - center : 1e30;
	float slope = abs(a) < abs(b) ? a : b;

	return abs(slope) < 1e29 ? slope : 0.0;
}

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
	ivec2 resolution = imageSize(cortao_filterOutput);
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);

	if(any(greaterThanEqual(p, resolution))) return;

	uint centerPacked = texelFetch(cortao_packedData, p, 0).r;

	if(centerPacked == 0u) {
		imageStore(cortao_filterOutput, p, vec4(1.0));

		return;
	}

#ifdef OSGX_CORTAO_FILTER_COPY
	imageStore(cortao_filterOutput, p, vec4(texelFetch(cortao_rawOcclusion, p, 0).r));
#else
	float centerDepth = cortao_unpackDepth(centerPacked);
	vec3 centerNormal = vec3(cortao_decodeNormal(centerPacked));
	vec2 slope = vec2(
		osgx_cortao_slope(p, ivec2(1, 0), centerDepth, resolution),
		osgx_cortao_slope(p, ivec2(0, 1), centerDepth, resolution)
	);

	float sum = 0.0;
	float weightSum = 0.0;

	for(int y = -2; y < 2; y++) {
		for(int x = -2; x < 2; x++) {
			ivec2 q = clamp(p + ivec2(x, y), ivec2(0), resolution - 1);
			uint packedData = texelFetch(cortao_packedData, q, 0).r;

			if(packedData == 0u) continue;

			float depthError = abs(
				cortao_unpackDepth(packedData) - (centerDepth + dot(vec2(x, y), slope))
			) / centerDepth;
			float normalError = 1.0 - dot(vec3(cortao_decodeNormal(packedData)), centerNormal);
			float weight = exp2(
				-(depthError * cortao_filterDepthSensitivity + normalError * cortao_filterNormalSensitivity)
			);

			sum += weight * texelFetch(cortao_rawOcclusion, q, 0).r;
			weightSum += weight;
		}
	}

	// The center tap always contributes weight 1.
	imageStore(cortao_filterOutput, p, vec4(sum / weightSum));
#endif
}
)GLSL";

std::string tiledFilterSource() {
	std::string src = SHADER_HEADER;

	src += detail::CORTAO_BASE_SRC;
	src += TILED_FILTER_SHADER;

	return src;
}

unsigned int mipCountFor(int width, int height) {
	const auto largest = static_cast<unsigned int>(std::max(width, height));

	unsigned int count = 1;

	while(count < MAX_MIP_COUNT && (largest >> count) > 0) count++;

	return count;
}

// Spheremap decode matching the reference generator's decodeSphereMap(): (x, y) in [0, 1]^2 to a
// unit direction; anything outside the unit disk collapses to (0, 0, -1).
osg::Vec3d decodeSphereMap(double x, double y) {
	const double ex = x * 2.0 - 1.0;
	const double ey = y * 2.0 - 1.0;
	const double z = std::max(1.0 - ex * ex - ey * ey, 0.0);
	const double mul = 2.0 * std::sqrt(z);

	return osg::Vec3d(ex * mul, ey * mul, z * 2.0 - 1.0);
}

}

osg::ref_ptr<osg::Image> CORTAO::createOccluderLookupTable(
	unsigned int rayCount,
	unsigned int directionResolution,
	unsigned int radiusResolution
) {
	GLenum dataType = 0;
	GLint internalFormat = 0;

	switch(rayCount) {
		case 8: dataType = GL_UNSIGNED_BYTE; internalFormat = GL_R8UI; break;
		case 16: dataType = GL_UNSIGNED_SHORT; internalFormat = GL_R16UI; break;
		case 32: dataType = GL_UNSIGNED_INT; internalFormat = GL_R32UI; break;
		default: break;
	}

	if(!dataType || !directionResolution || !radiusResolution) {
		OSG_WARN
			<< "osgx::CORTAO::createOccluderLookupTable: unsupported rayCount " << rayCount
			<< " (8, 16 or 32) or zero resolution" << std::endl
		;

		return nullptr;
	}

	const double goldenAngle = osg::PI * (3.0 - std::sqrt(5.0));

	std::vector<osg::Vec3d> rays(rayCount);

	for(std::size_t i = 0; i < rayCount; i++) {
		const double angle = static_cast<double>(i) * goldenAngle;
		const double radius = std::sqrt((static_cast<double>(i) + 0.5) / rayCount);
		const double x = std::cos(angle) * radius;
		const double y = std::sin(angle) * radius;

		rays[i] = osg::Vec3d(x, y, std::sqrt(std::max(1.0 - x * x - y * y, 0.0)));
	}

	const std::size_t sliceSize = static_cast<std::size_t>(directionResolution) * directionResolution;
	const double maxOccluderRadius = std::sqrt(2.0);

	std::vector<std::uint32_t> bits(sliceSize * radiusResolution, 0u);

	struct Closest {
		std::size_t index;
		double cosAngle;
	};

	for(std::size_t z = 0; z < radiusResolution; z++) {
		const double occluderRadius = maxOccluderRadius * (static_cast<double>(z) + 0.5) / radiusResolution;
		const double occluderCosAngle = 1.0 - occluderRadius * occluderRadius * 0.5;
		const std::size_t offset = z * sliceSize;

		// Per ray, the direction texel pointing closest to it - always marked as blocking that ray
		// below, so every ray is hit by at least one texel of every radius slice.
		std::vector<Closest> closest(rayCount, {std::numeric_limits<std::size_t>::max(), -1.0});

		for(std::size_t y = 0; y < directionResolution; y++) {
			for(std::size_t x = 0; x < directionResolution; x++) {
				const auto direction = decodeSphereMap(
					(static_cast<double>(x) + 0.5) / directionResolution,
					(static_cast<double>(y) + 0.5) / directionResolution
				);

				if(direction.z() == -1.0) continue;

				const std::size_t index = y * directionResolution + x;

				for(std::size_t i = 0; i < rayCount; i++) {
					const double cosAngle = direction * rays[i];

					if(cosAngle > closest[i].cosAngle) closest[i] = {index, cosAngle};

					if(cosAngle > occluderCosAngle) bits[offset + index] |= 1u << i;
				}
			}
		}

		for(std::size_t i = 0; i < rayCount; i++) {
			if(closest[i].index != std::numeric_limits<std::size_t>::max()) {
				bits[offset + closest[i].index] |= 1u << i;
			}
		}
	}

	auto image = osgx::make_nref<osg::Image>("osgx_cortao_OccluderLookupTable");

	image->allocateImage(
		static_cast<int>(directionResolution),
		static_cast<int>(directionResolution),
		static_cast<int>(radiusResolution),
		GL_RED_INTEGER_EXT,
		dataType
	);
	image->setInternalTextureFormat(internalFormat);

	for(std::size_t i = 0; i < bits.size(); i++) {
		switch(dataType) {
			case GL_UNSIGNED_BYTE:
				reinterpret_cast<std::uint8_t*>(image->data())[i] = static_cast<std::uint8_t>(bits[i]);
				break;

			case GL_UNSIGNED_SHORT:
				reinterpret_cast<std::uint16_t*>(image->data())[i] = static_cast<std::uint16_t>(bits[i]);
				break;

			default:
				reinterpret_cast<std::uint32_t*>(image->data())[i] = bits[i];
				break;
		}
	}

	return image;
}

bool CORTAO::valid() const {
	return node.valid() && prepass.valid() && mainPass.valid() && aoTexture.valid();
}

void CORTAO::setSampleCount(unsigned int count) {
	if(!mainPass.valid()) return;

	mainPass->getOrCreateStateSet()->setDefine("CORTAO_SAMPLE_COUNT", std::to_string(std::max(count, 1u)));
}

unsigned int CORTAO::getSampleCount() const {
	if(!mainPass.valid() || !mainPass->getStateSet()) return 0;

	const auto* define = mainPass->getStateSet()->getDefinePair("CORTAO_SAMPLE_COUNT");

	return define ? static_cast<unsigned int>(std::stoul(define->first)) : 0;
}

void CORTAO::setLargeFilter(bool large) {
	if(filterPass.valid()) filterPass->setDefineEnabled("CORTAO_LARGE_FILTER", large);
}

bool CORTAO::getLargeFilter() const {
	return filterPass.valid() && filterPass->isDefineEnabled("CORTAO_LARGE_FILTER");
}

void CORTAO::setFilterSymmetry(bool symmetry) {
	if(filterPass.valid()) filterPass->setDefineEnabled("CORTAO_FILTER_SYMMETRY", symmetry);
}

bool CORTAO::getFilterSymmetry() const {
	return filterPass.valid() && filterPass->isDefineEnabled("CORTAO_FILTER_SYMMETRY");
}

void CORTAO::setFilter(Filter filter) {
	if(!filterPass.valid() || !mainPass.valid() || !aoTexture.valid()) return;

	const int width = aoTexture->getTextureWidth();
	const int height = aoTexture->getTextureHeight();

	// SPARSE threads cover a 2x2 pixel block each.
	if(filter == Filter::SPARSE) {
		filterPass->setProgram(sparseFilterProgram);
		filterPass->setComputeGroupsFor(
			(width + 1) / 2,
			(height + 1) / 2,
			FILTER_PASS_WORKGROUP_SIZE,
			FILTER_PASS_WORKGROUP_SIZE
		);
	}

	else {
		filterPass->setProgram(tiledFilterProgram);
		filterPass->setComputeGroupsFor(
			width,
			height,
			FILTER_PASS_WORKGROUP_SIZE,
			FILTER_PASS_WORKGROUP_SIZE
		);
	}

	filterPass->setDefineEnabled("OSGX_CORTAO_FILTER_COPY", filter == Filter::NONE);
	mainPass->setDefineEnabled("OSGX_CORTAO_TILED_NOISE", filter == Filter::TILED);
}

CORTAO::Filter CORTAO::getFilter() const {
	if(!filterPass.valid()) return Filter::NONE;

	if(filterPass->getProgram() == sparseFilterProgram.get()) return Filter::SPARSE;

	return filterPass->isDefineEnabled("OSGX_CORTAO_FILTER_COPY") ? Filter::NONE : Filter::TILED;
}

CORTAO CORTAO::create(
	osg::Texture2D* normalTexture,
	osg::Texture2D* positionTexture,
	osg::Uniform* projectionMatrix,
	int width,
	int height
) {
	return create(normalTexture, positionTexture, projectionMatrix, width, height, Options());
}

CORTAO CORTAO::create(
	osg::Texture2D* normalTexture,
	osg::Texture2D* positionTexture,
	osg::Uniform* projectionMatrix,
	int width,
	int height,
	const Options& options
) {
	CORTAO result;

	if(!normalTexture || !positionTexture || !projectionMatrix || width <= 0 || height <= 0) return result;

	auto lutImage = createOccluderLookupTable(RAY_COUNT);

	if(!lutImage) return result;

	const unsigned int mipCount = mipCountFor(width, height);

	// Packed linear depth + spheremap normal, every mip written by the prepass's own image stores
	// (never glGenerateMipmap). Sampled NEAREST with a 0 border: 0 is CORTAO's "no data" value,
	// so off-screen samples read as sky.
	result.packedTexture = osgx::make_nref<osg::Texture2D>("osgx_cortao_PackedData");
	result.packedTexture->setTextureSize(width, height);
	result.packedTexture->setInternalFormat(GL_R32UI);
	result.packedTexture->setSourceFormat(GL_RED_INTEGER_EXT);
	result.packedTexture->setSourceType(GL_UNSIGNED_INT);
	result.packedTexture->setNumMipmapLevels(mipCount);
	result.packedTexture->setUseHardwareMipMapGeneration(false);
	result.packedTexture->setResizeNonPowerOfTwoHint(false);
	result.packedTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST_MIPMAP_NEAREST);
	result.packedTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	result.packedTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER);
	result.packedTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_BORDER);
	result.packedTexture->setBorderColor(osg::Vec4d(0.0, 0.0, 0.0, 0.0));
	result.packedTexture->setDataVariance(osg::Object::DYNAMIC);

	result.rawAoTexture = ComputePass::createTexture("osgx_cortao_RawAO", width, height, GL_R8);

	if(options.bentNormal) {
		result.bentNormalTexture = ComputePass::createTexture(
			"osgx_cortao_BentNormal",
			width,
			height,
			GL_RGBA8_SNORM
		);
	}

	result.occluderLookupTable = osgx::make_nref<osg::Texture3D>("osgx_cortao_OccluderLookupTable");
	result.occluderLookupTable->setImage(lutImage);
	result.occluderLookupTable->setResizeNonPowerOfTwoHint(false);
	result.occluderLookupTable->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
	result.occluderLookupTable->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
	result.occluderLookupTable->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	result.occluderLookupTable->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	result.occluderLookupTable->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

	// Defaults from CORTAOConfigurationParameters (CORTAOParameters.h).
	result.maxRadius = new osg::Uniform("cortao_maxRadius", options.maxRadius);
	result.maxScreenspaceRadiusHeightRatio = new osg::Uniform("cortao_maxScreenspaceRadiusHeightRatio", 0.2f);
	result.mipBias = new osg::Uniform("cortao_mipBias", -4.0f);
	result.maxSlopeCompensation = new osg::Uniform("cortao_maxSlopeCompensation", 4.0f);
	result.backfaceThreshold = new osg::Uniform("cortao_backfaceThreshold", 0.5f);
	result.backfaceBoost = new osg::Uniform("cortao_backfaceBoost", 0.5f);
	result.filterDepthSensitivity = new osg::Uniform("cortao_filterDepthSensitivity", 512.0f);
	result.filterNormalSensitivity = new osg::Uniform("cortao_filterNormalSensitivity", 16.0f);
	result.randomSeed = new osg::Uniform("cortao_randomSeed", 0u);

	// Prepass: writes packedTexture's mips, which the main and filter passes then sample.
	result.prepass = new ComputePass(
		"osgx_cortao_Prepass",
		ComputePass::createProgram("osgx_cortao_Prepass", prepassSource())
	);
	result.prepass->setComputeGroupsFor(width, height, PREPASS_WORKGROUP_SIZE, PREPASS_WORKGROUP_SIZE);
	result.prepass->setRenderBin(PREPASS_RENDER_BIN);

	for(unsigned int level = 0; level < mipCount; level++) {
		result.prepass->bindImage(
			level,
			result.packedTexture,
			osg::BindImageTexture::WRITE_ONLY,
			GL_R32UI,
			static_cast<int>(level)
		);
	}

	result.prepass->bindTexture(0, normalTexture, "gNormal");
	result.prepass->bindTexture(1, positionTexture, "gPosition");

	auto* prepassSS = result.prepass->getOrCreateStateSet();

	prepassSS->addUniform(result.randomSeed);
	prepassSS->addUniform(new osg::Uniform("cortao_mipCount", mipCount));

	// Main pass: writes rawAoTexture (+ bentNormalTexture).
	result.mainPass = new ComputePass(
		"osgx_cortao_MainPass",
		ComputePass::createProgram("osgx_cortao_MainPass", mainPassSource(options.bentNormal))
	);
	result.mainPass->setComputeGroupsFor(
		width,
		height,
		MAIN_PASS_WORKGROUP_SIZE,
		MAIN_PASS_WORKGROUP_SIZE
	);
	result.mainPass->setRenderBin(MAIN_PASS_RENDER_BIN);
	result.mainPass->bindImage(0, result.rawAoTexture, osg::BindImageTexture::WRITE_ONLY, GL_R8);

	if(options.bentNormal) {
		result.mainPass->bindImage(
			1,
			result.bentNormalTexture,
			osg::BindImageTexture::WRITE_ONLY,
			GL_RGBA8_SNORM
		);
	}

	result.mainPass->bindTexture(0, result.packedTexture, "cortao_packedData");
	result.mainPass->bindTexture(1, normalTexture, "gNormal");
	result.mainPass->bindTexture(2, result.occluderLookupTable, "cortao_occluderLookupTable");

	auto* mainSS = result.mainPass->getOrCreateStateSet();

	mainSS->addUniform(projectionMatrix);
	mainSS->addUniform(result.randomSeed);
	mainSS->addUniform(result.maxRadius);
	mainSS->addUniform(result.maxScreenspaceRadiusHeightRatio);
	mainSS->addUniform(result.mipBias);
	mainSS->addUniform(result.maxSlopeCompensation);
	mainSS->addUniform(result.backfaceThreshold);
	mainSS->addUniform(result.backfaceBoost);

	result.setSampleCount(options.sampleCount);

	result.node = osgx::make_nref<osg::Group>("osgx_cortao");
	result.node->addChild(result.prepass);
	result.node->addChild(result.mainPass);

	// Filter pass: rawAoTexture -> aoTexture; program and dispatch size set by setFilter() below.
	result.aoTexture = ComputePass::createTexture("osgx_cortao_AO", width, height, GL_R8);
	result.sparseFilterProgram = ComputePass::createProgram(
		"osgx_cortao_SparseFilterPass",
		filterPassSource()
	);
	result.tiledFilterProgram = ComputePass::createProgram(
		"osgx_cortao_TiledFilterPass",
		tiledFilterSource()
	);

	result.filterPass = new ComputePass("osgx_cortao_FilterPass");
	result.filterPass->setRenderBin(FILTER_PASS_RENDER_BIN);
	result.filterPass->bindImage(0, result.aoTexture, osg::BindImageTexture::WRITE_ONLY, GL_R8);
	result.filterPass->bindTexture(0, result.packedTexture, "cortao_packedData");
	result.filterPass->bindTexture(1, result.rawAoTexture, "cortao_rawOcclusion");

	auto* filterSS = result.filterPass->getOrCreateStateSet();

	filterSS->addUniform(result.filterDepthSensitivity);
	filterSS->addUniform(result.filterNormalSensitivity);

	// The reference GLSL filter wrapper's own defaults (SPARSE only).
	result.setLargeFilter(true);
	result.setFilterSymmetry(true);
	result.setFilter(options.filter);

	result.node->addChild(result.filterPass);

	return result;
}

}
