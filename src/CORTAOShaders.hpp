#pragma once

// The CORTAO reference shaders (https://github.com/theagentd/CORTAO, v1.0.0, commit e5b9901),
// ported to plain GLSL for osgx::CORTAO: src/shaders/CORTAO{Base,Prepass,MainPass,FilterPass}.h
// with the CORTAO_GLSL/CORTAO_HLSL/CORTAO_16_BIT language layer removed (cortao_float3 -> vec3,
// cortao_fma() -> fma(), cortao_mul(a, b) -> (a * b), ...) and their #include lines dropped. The
// algorithm, its function names and its #if feature switches are unchanged, so each string still
// diffs cleanly against its upstream header. The pass callbacks (cortao_load*/sample*/gather*/
// write*) and main() functions are osgx's own, in CORTAO.cpp.
//
// MIT License
//
// Copyright (c) 2026 Daniel Isheden
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

namespace osgx::detail {

// CORTAOBase.h: packed-data helpers (normal encode/decode, depth unpack) and the hash/seed
// functions.
constexpr const char CORTAO_BASE_SRC[] = R"GLSL(
// The reference's GLSL language layer (CORTAO_GLSL, 32-bit types), reduced to the two helpers that
// are not plain GLSL. GL_EXT_control_flow_attributes enables [[unroll]] where a driver exposes it.
#ifdef GL_EXT_control_flow_attributes
#define cortao_unroll [[unroll]]
#else
#define cortao_unroll
#endif

uvec2 cortao_unpack16(uint x) {
	return uvec2(x & 0xFFFFu, x >> 16u);
}

#ifndef CORTAO_PACKED_NORMAL_BITS
#define CORTAO_PACKED_NORMAL_BITS 10
#endif

#if CORTAO_PACKED_NORMAL_BITS < 4 || CORTAO_PACKED_NORMAL_BITS > 20
#error "CORTAO_PACKED_NORMAL_BITS must be between 4 and 20, inclusive."
#endif

#if (CORTAO_PACKED_NORMAL_BITS & 1) == 1
#error "CORTAO_PACKED_NORMAL_BITS must be even when CORTAO_PACKED_DATA is CORTAO_PACKED_DATA_NORMAL."
#endif

const float CORTAO_PI = float(3.1415926535897932384626433832795);
const float CORTAO_PI2 = float(6.283185307179586476925286766559); // PI * 2.0
const float CORTAO_GOLDEN_ANGLE = float(2.3999632297286533222315555066336); // PI * (3.0 - sqrt(5.0))

uint cortao_seed(uvec2 coords) {
	uvec2 q = coords * uvec2(1719413u*929u, 140473u*2467u*11u);
	return q.x ^ q.y;
}

uint cortao_hash(uint n) {
	return n * (n ^ (n >> 15u));
}

uvec2 cortao_hash(uvec2 n) {
	return n * (n ^ (n >> 15u));
}

uint cortao_encodeNormal(vec3 normal, vec2 dither) {
	uint bitsPerComponent = uint(CORTAO_PACKED_NORMAL_BITS / 2);

	uint componentMask = (1u << bitsPerComponent) - 1u;
	uint componentMax = componentMask >> 1u;

	vec2 encodedNormal = normal.xy * inversesqrt(fma(normal.z, 2.0, 2.0));

	ivec2 values = ivec2(floor(fma(encodedNormal, vec2(componentMax, componentMax), dither)));

	return (uint(values.x) & componentMask) | ((uint(values.y) & componentMask) << bitsPerComponent);
}

vec3 cortao_decodeNormal(uint packedData) {
	uint bitsPerComponent = uint(CORTAO_PACKED_NORMAL_BITS / 2);

	uint componentMask = (1u << bitsPerComponent) - 1u;
	uint componentMax = componentMask >> 1u;

	uint bits = uint(32);

	ivec2 leftShift = ivec2(bits - uvec2(bitsPerComponent, bitsPerComponent*2));
	ivec2 rightShift = ivec2(bits - bitsPerComponent, bits - bitsPerComponent);
	ivec2 values = ivec2((ivec2(packedData, packedData) << leftShift) >> rightShift);

	vec2 e = vec2(values) * float(1.0 / float(componentMax));

	float z = fma(-e.x, e.x, fma(-e.y, e.y, float(1.0)));
	return vec3(e * (float(2.0) * sqrt(z)), fma(z, float(2.0), float(-1.0)));
}

float cortao_unpackDepth(uint packedData) {
	return uintBitsToFloat(packedData);
}

vec4 cortao_unpackDepth(uvec4 packedData) {
	return uintBitsToFloat(packedData);
}

uvec2 cortao_getResolution(uint packedResolution) {
	return cortao_unpack16(packedResolution);
}
)GLSL";

// CORTAOPrepass.h: cortao_prepass(), linear depth + packed normal into every mip of the packed
// buffer.
constexpr const char CORTAO_PREPASS_SRC[] = R"GLSL(
#ifndef CORTAO_PERSPECTIVE_PROJECTION
#define CORTAO_PERSPECTIVE_PROJECTION 0
#endif

#ifndef CORTAO_ORTHOGRAPHIC_PROJECTION
#define CORTAO_ORTHOGRAPHIC_PROJECTION 0
#endif

#if CORTAO_PERSPECTIVE_PROJECTION && CORTAO_ORTHOGRAPHIC_PROJECTION
#error "Please define exactly one of CORTAO_PERSPECTIVE_PROJECTION and CORTAO_ORTHOGRAPHIC_PROJECTION to 1"
#endif

#if !CORTAO_PERSPECTIVE_PROJECTION && !CORTAO_ORTHOGRAPHIC_PROJECTION
#error "Please define exactly one of CORTAO_PERSPECTIVE_PROJECTION and CORTAO_ORTHOGRAPHIC_PROJECTION to 1"
#endif

float cortao_loadDepth(ivec2 pixelCoords);
vec3 cortao_loadNormal(ivec2 pixelCoords);

void cortao_writePackedData(ivec2 pixelCoords, int mipLevel, uint data);

struct CORTAOPrepassParameters {
	uint packedResolution;
	float depthScale;
	float depthOffset;
	uint randomSeed;

	uint mipCount;
	uint _padding0;
	uint _padding1;
	uint _padding2;
};

void cortao_prepass(uvec2 pixelCoords, CORTAOPrepassParameters params) {
	// check if our pixel is out-of-bounds and early out in that case
	uvec2 resolution = cortao_getResolution(params.packedResolution);
	if(pixelCoords.x >= resolution.x || pixelCoords.y >= resolution.y) {
		return;
	}

	float depth = cortao_loadDepth(ivec2(pixelCoords));

	uint packedData = 0u;

	float skyBoxDepth = float(params.depthScale < 0.0); // Handles both standard and inverted depth
	if(depth != skyBoxDepth) {
		// Calculate the linear depth
#if CORTAO_PERSPECTIVE_PROJECTION
			float linearDepth = 1.0 / fma(depth, params.depthScale, params.depthOffset);
#else
			float linearDepth = fma(depth, params.depthScale, params.depthOffset);
#endif

		vec3 normal = cortao_loadNormal(ivec2(pixelCoords));

		// Pack a spheremap encoded normal
		uint seed = cortao_hash(cortao_seed(pixelCoords) + params.randomSeed);
		vec2 dither = vec2(cortao_unpack16(seed)) * (1.0 / float(0xFFFFu));
		uint mask = (1 << CORTAO_PACKED_NORMAL_BITS) - 1;
		uint packedNormal = cortao_encodeNormal(normal, dither);

		// Merge the linear depth and the packed normal
		packedData = (floatBitsToUint(linearDepth) & ~mask) | (packedNormal & mask);
	}

	cortao_writePackedData(ivec2(pixelCoords), 0, packedData);

	cortao_unroll for(int mipLevel = 1; mipLevel < 8; mipLevel++) {
		uvec2 nextCoords = pixelCoords >> 1u;

		uvec2 dither = uvec2(params.randomSeed, params.randomSeed) >> uvec2(mipLevel*2, mipLevel*2 + 1);

		bool wrongPixel = bool(((pixelCoords.x ^ nextCoords.y ^ dither.x) | (pixelCoords.y ^ nextCoords.x ^ dither.y)) & 0x1u);

		pixelCoords = nextCoords;
		resolution = max(resolution >> 1u, 1u);

		if(any(bvec4(mipLevel >= params.mipCount, wrongPixel, pixelCoords.x >= resolution.x, pixelCoords.y >= resolution.y))) {
			return;
		}

		cortao_writePackedData(ivec2(pixelCoords), mipLevel, packedData);
	}
}
)GLSL";

// CORTAOMainPass.h: cortao_mainPass(), the occluder sampling and ray occlusion. Two adjacent
// literals: MSVC limits a single string literal to 16 KB.
constexpr const char CORTAO_MAIN_PASS_SRC[] = R"GLSL(
// Preprocessor checks

#ifndef CORTAO_PERSPECTIVE_PROJECTION
#define CORTAO_PERSPECTIVE_PROJECTION 0
#endif

#ifndef CORTAO_ORTHOGRAPHIC_PROJECTION
#define CORTAO_ORTHOGRAPHIC_PROJECTION 0
#endif

#if CORTAO_PERSPECTIVE_PROJECTION && CORTAO_ORTHOGRAPHIC_PROJECTION
#error "Please define exactly one of CORTAO_PERSPECTIVE_PROJECTION and CORTAO_ORTHOGRAPHIC_PROJECTION to 1"
#endif

#if !CORTAO_PERSPECTIVE_PROJECTION && !CORTAO_ORTHOGRAPHIC_PROJECTION
#error "Please define exactly one of CORTAO_PERSPECTIVE_PROJECTION and CORTAO_ORTHOGRAPHIC_PROJECTION to 1"
#endif

#ifndef CORTAO_SAMPLE_COUNT
#error "Pleased define CORTAO_SAMPLE_COUNT to the number of samples to do per pixel."
#endif

#if CORTAO_SAMPLE_COUNT < 1
#error "CORTAO_SAMPLE_COUNT must be at least 1."
#endif

#ifndef CORTAO_RAY_COUNT
#error "Please define CORTAO_RAY_COUNT to the number of rays to simulate. Valid values are 8 to 32, inclusive. A good default is 16. If using CORTAO_OCCLUDER_LOOKUP_TABLE, this must match what the lookup table contains."
#endif

#if CORTAO_RAY_COUNT < 8 || CORTAO_RAY_COUNT > 32
#error "CORTAO_RAY_COUNT must be between 8 and 32, inclusive."
#endif

#ifndef CORTAO_OCCLUDER_LOOKUP_TABLE
#define CORTAO_OCCLUDER_LOOKUP_TABLE 0
#endif

#ifndef CORTAO_BENT_NORMAL_LOOKUP_TABLE
#define CORTAO_BENT_NORMAL_LOOKUP_TABLE 0
#endif

#ifndef CORTAO_DEBUG_HOOKS
#define CORTAO_DEBUG_HOOKS 0
#endif

// Functions to implement

vec3 cortao_loadNormal(ivec2 pixelCoords);

uint cortao_samplePackedData(vec2 texCoords, float mipmap);

#if CORTAO_OCCLUDER_LOOKUP_TABLE
	uint cortao_sampleOccluderLookupTable(vec3 texCoords);
#endif

#if CORTAO_BENT_NORMAL_LOOKUP_TABLE
	vec3 cortao_loadBentNormalLookupTable(uint rayBits);
#endif

void cortao_writeResult(ivec2 pixelCoords, float occlusion, vec3 bentNormal, mat3 tangentSpaceTransform, uint rayBits);

#if CORTAO_DEBUG_HOOKS
	void cortao_debugCenterPixel(vec3 position, vec3 normal, float radius);
	void cortao_debugCulledOccluder(uint index, vec3 occluderPosition);
	void cortao_debugPassedOccluder(uint index, vec3 occluderPosition, vec3 occluderNormal, float occluderRadius, uint rayBits);
#endif

// Public structs

struct CORTAOMainPassParameters {
	uint packedResolution;
	uint randomSeed;
	float normalBias;
	float radius;

	vec4 unprojectParams;

	vec4 projectionParams;

	float maxApparentCircleRadiusCoefficient;
	float mipBias;
	float inverseMaxSlopeCompensation;
	float backfaceThreshold;

	float backfaceBoost;
	uint _padding0;
	uint _padding1;
	uint _padding2;
};

// Public functions

vec3 cortao_rayDirection(uint rayIndex) {
	float angle = float(rayIndex) * CORTAO_GOLDEN_ANGLE;
	float radius = sqrt((float(rayIndex) + 0.5) / float(CORTAO_RAY_COUNT));
	vec3 rayDirection = vec3(cos(angle) * radius, sin(angle) * radius, sqrt(float(1.0) - radius*radius));
	return rayDirection;
}

// Internal CORTAO stuff

vec3 cortao_unprojectWithOffset(vec4 unprojectParameters, vec2 texCoords, float linearDepth, vec3 offset) {
	// Lerp between the frustum corner directions.
	// Optimized by precomputing the difference between the corners, to turn it into FMA.
	vec2 direction = fma(texCoords, unprojectParameters.xy, unprojectParameters.zw);

#if CORTAO_PERSPECTIVE_PROJECTION
		// Calculate the view space direction as direction * depth.
		// Also add an optional offset here, as it's free to do with FMA.
		return fma(vec3(direction, -1.0), vec3(linearDepth, linearDepth, linearDepth), offset);
#else
		// For orthographic projections, the "direction" is really the starting XY coordinates, and only Z is affected by depth.
		return vec3(direction, -linearDepth) + offset;
#endif
}

vec3 cortao_unproject(vec4 unprojectParameters, vec2 texCoords, float linearDepth) {
	return cortao_unprojectWithOffset(unprojectParameters, texCoords, linearDepth, vec3(0, 0, 0));
}

uint cortao_computeRayOcclusion(
			float offsetDither,
			mat3 offsetTransform,
			mat3 tangentSpaceTransform,
			float mipBias,
			vec3 viewPosition,
			vec3 normal,
			vec3 viewDirection,
			float maxRadius,
			CORTAOMainPassParameters params
) {
	uint rayBits = 0u;

	// Hack to preload the occluder lookup table descriptor, which is marginally faster at higher sample counts.
#if CORTAO_OCCLUDER_LOOKUP_TABLE && CORTAO_SAMPLE_COUNT > 4
		rayBits = cortao_sampleOccluderLookupTable(vec3(0, 0, 0));
#endif

	// Don't unroll if we have too many samples, to prevent driver compilers from croaking.
#if CORTAO_SAMPLE_COUNT <= 64
	cortao_unroll
#endif
	for(uint i = 0u; i < CORTAO_SAMPLE_COUNT; i++) {
		// these are constant for an unrolled loop, so free

		// Calculate the golden angle spiral direction
		float a = float(i) * CORTAO_GOLDEN_ANGLE;
		vec2 spiral = vec2(cos(a), sin(a));

		float inverseSampleCount = 1.0 / float(CORTAO_SAMPLE_COUNT);

		// The skew is how much we suck the sample distribution into the center.
		float skew = 3.5;

		// Calculate an inner and outer radius for this sample. Dithering is used to choose a radius between these two values.
		float innerRadius = pow((float(i) + 0.0) * inverseSampleCount, skew*0.5);
		float outerRadius = pow((float(i) + 1.0) * inverseSampleCount, skew*0.5);

		// Precompute the inner and outer spiral positions. The dithered position will lie somewhere on a lerp between the two.
		vec2 innerSpiral = vec2(spiral * innerRadius);
		vec2 outerSpiral = vec2(spiral * outerRadius);

		// Calculate the area of the sampling circle that the sample is randomly moved over,
		// based on both a random rotation and a random radius dither to form a circular strip.
		// The weight is sqaure-rooted to account for the fact that the weight is applied to a radius, rather than an area.
		float sampleArea = outerRadius*outerRadius - innerRadius*innerRadius;
		float sampleWeight = sqrt(sampleArea); // The square root of the percentage of the area of the circle that the sample can be placed in

		// The mip level depends on how far away we are from the center.
		float sampleMipmap = log2(outerRadius);

		// dynamic stuff from here on

		// Optimized lerp between the inner and outer sample positions based on a random dither value.
		vec2 sampleOffset = fma(outerSpiral - innerSpiral, vec2(offsetDither, offsetDither), innerSpiral);

		// Apply the sample transformation matrix and do a perspective division.
		vec3 projectedCoords = (offsetTransform * vec3(sampleOffset, 1.0));
		vec2 sampleCoords = projectedCoords.xy;
#if CORTAO_PERSPECTIVE_PROJECTION
			sampleCoords /= projectedCoords.z;
#endif

		// Add the precomputed mip bias and the per-sample mip level together to get our mip level, based on logarithmic identities.
		float mipmap = mipBias + sampleMipmap;

		// Sample the center pixel, and unpack depth only for now.
		uint packedData = cortao_samplePackedData(sampleCoords, mipmap);
		float sampleDepth = cortao_unpackDepth(packedData);

		// Calculate this sample's view space position relative to the center pixel.
		vec3 dPos = cortao_unprojectWithOffset(params.unprojectParams, sampleCoords, sampleDepth, -viewPosition);

		// Do an early out if the sample ends up outside the hemisphere (radius and normal check).
		// Skybox samples and samples outside the screen have a linear depth of 0.0, which is always outside the hemisphere.
		float lengthSqrd = dot(dPos, dPos);
		if(lengthSqrd < maxRadius*maxRadius && dot(dPos, vec3(normal)) > 0.0) {
			// Calculate the occluder direction.
			float inverseLength = inversesqrt(lengthSqrd);
			vec3 direction = vec3(dPos * inverseLength);

			// Unpack the sample normal from the packed data we already read.
			vec3 sampleNormal = cortao_decodeNormal(packedData);

			// Calculate the slope compensation weight and use it to calculate the normalized visual occluder radius.
			float slopeCompensation = inversesqrt(max(dot(sampleNormal, viewDirection), float(params.inverseMaxSlopeCompensation)));
			float occluderRadius = (maxRadius * sampleWeight) * (slopeCompensation * inverseLength);

			// Heuristically check for backface occluders.
			float sampleDot = dot(sampleNormal, direction);
			if(sampleDot < float(params.backfaceThreshold)) {
				// Not a backface occluder. Apply a normal offset to prevent self-occlusion.
				// This moves the occluder circle by 1 radius into the occluder object. As this is done after occluder normalization,
				// Re-normalize the direction after we apply the offset to get the new direction, but keep the occluder radius the same.
				float adjustedOffset = float(-occluderRadius) / float(max(1.0 - occluderRadius * occluderRadius * 0.5, 0.000001));
				direction = normalize(fma(sampleNormal, vec3(adjustedOffset, adjustedOffset, adjustedOffset), direction));
			}

			else {
				// This is a backface occluder. Don't do the normal offset, and optionally boost the slope compensation to avoid light leaking.
				occluderRadius *= mix(float(1.0), slopeCompensation, float(params.backfaceBoost));
			}

			// Transform the occluder direction into the ray tracing tangent space.
			vec3 hemisphereDirection = (direction * tangentSpaceTransform);

			uint occluderRayBits;

#if CORTAO_OCCLUDER_LOOKUP_TABLE

			// Encode the normal as a spheremap projection, which is used as the lookup coordinates together with the occluder radius.
			float mul = inversesqrt(fma(hemisphereDirection.z, float(8.0), float(8.0)));
			vec2 sphereMapCoordinates = fma(hemisphereDirection.xy, vec2(mul, mul), vec2(0.5, 0.5));
			vec3 lookupCoords = vec3(sphereMapCoordinates, occluderRadius * sqrt(0.5));

			// Sphere map projection fails if the tangent space direction is (0, 0, -1), so ignore the invalid sample we do in that case.
			occluderRayBits = cortao_sampleOccluderLookupTable(lookupCoords) & (hemisphereDirection.z > float(-0.99) ? 0xFFFFFFFFu : 0u);
#else
			{
				// Calculate the angular size of the occluder. This wraps the occluder around the hemisphere,
				// matching the occluded area of the hemisphere with the area of the circle linearly, to improve the scalability of CORTAO.
				float occluderCosAngle = float(max(0.0, 1.0 - occluderRadius * occluderRadius * 0.5));

				// Manually check each ray against hte occluder's angular size.
				occluderRayBits = 0u;
				for(uint j = 0u; j < CORTAO_RAY_COUNT; j++) {
					vec3 rayDirection = cortao_rayDirection(j);
					if(dot(rayDirection, hemisphereDirection) > occluderCosAngle) {
						occluderRayBits |= 1u << j;
					}
				}
			}
#endif

			rayBits |= occluderRayBits;

#if CORTAO_DEBUG_HOOKS
			cortao_debugPassedOccluder(i, viewPosition + dPos, direction, occluderRadius, occluderRayBits);
#endif
		}

		else {
#if CORTAO_DEBUG_HOOKS
				cortao_debugCulledOccluder(i, viewPosition + dPos);
#endif
		}
	}

	return rayBits;
}
)GLSL"
R"GLSL(
void cortao_constructSamplingBasis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
	// Frisvad's arbitrary orthonormal tangent basis construction.
	float a = float(1.0) / (float(1.0) + normal.z);
	float b = normal.x * normal.y * a;
	tangent = vec3(fma(normal.x * normal.x, a, float(-1.0)), b, normal.x);
	bitangent = vec3(b, fma(normal.y * normal.y, a, float(-1.0)), normal.y);
}

void cortao_constructTangentBasis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
	// Also, frisvad's arbitrary orthonormal tangent basis construction, but copy-pasted and slightly tweaked.
	float a = float(1.0) / (float(1.0) + normal.z);
	float b = normal.x * normal.y * a;
	tangent = vec3(fma(normal.x * normal.x, a, float(-1.0)), b, normal.x);
	bitangent = vec3(b, fma(normal.y * normal.y, a, float(-1.0)), normal.y);
}

float cortao_resolveOcclusion(uint rayBits) {
	// Calculate occlusion as 1.0 - occluded rays / total rays
	uint numBits = bitCount(rayBits);
	return fma(float(numBits), float(-1.0) / float(CORTAO_RAY_COUNT), float(1.0)); //1.0 - numBits/CORTAO_RAY_COUNT;
}

vec3 cortao_resolveBentNormal(uint rayBits) {
#if CORTAO_BENT_NORMAL_LOOKUP_TABLE
		return cortao_loadBentNormalLookupTable(rayBits);
#else
		// Calculate the bent normal as the sum of all unoccluded rays.
		vec3 bentNormal = vec3(0, 0, 0);
		cortao_unroll for(uint i = 0; i < CORTAO_RAY_COUNT; i++) {
			vec3 rayDirection = cortao_rayDirection(i);

			bentNormal += rayDirection * ((rayBits & (1u << i)) == 0u ? float(1.0) : float(0.0));
		}
		return normalize(bentNormal); // Normalize the bent normal in the end.
#endif
}

void cortao_mainPass(uvec2 pixelCoords, CORTAOMainPassParameters params) {
	// check if our pixel is out-of-bounds and early out in that case
	uvec2 resolution = cortao_getResolution(params.packedResolution);
	if(pixelCoords.x >= resolution.x || pixelCoords.y >= resolution.y) {
		return;
	}

	vec2 texCoords = (vec2(pixelCoords) + 0.5) / vec2(resolution);

	float linearDepth = cortao_unpackDepth(cortao_samplePackedData(texCoords, 0.0));
	if(linearDepth == 0.0) {
		// This is a skybox pixel, ignore it.
		return;
	}

	vec3 normal = cortao_loadNormal(ivec2(pixelCoords));

	vec3 viewPosition = cortao_unproject(params.unprojectParams, texCoords, linearDepth);

	// Setup a set of random variables used during the setup code
	uvec2 random = cortao_hash(cortao_seed(pixelCoords) + cortao_unpack16(params.randomSeed));

	float randomSampleRotation = float(random.x) * (float(CORTAO_PI2) / float(0xFFFFFFFFu));
	float offsetDither = float(random.y) * (1.0 / float(0xFFFFFFFFu));
	float randomTangentRotation = float(float(random.x ^ random.y ^ params.randomSeed) * (float(CORTAO_PI2) / float(0xFFFFFFFFu)));
	vec2 sampleRotation = vec2(sin(randomSampleRotation), cos(randomSampleRotation));

#if CORTAO_PERSPECTIVE_PROJECTION

		// To enforce a maximum screenspace radius, we precompute the maximum radius as a linear function of depth.
		float clampedRadius = min(params.radius, linearDepth * params.maxApparentCircleRadiusCoefficient);

		// As we get closer to the sampling sphere, it starts to appear as a larger and larger surface due to the horizon.
		// This code calculates the required expanded circle radius to sample the entire sphere.
		float inverseCircleDistance = inversesqrt(linearDepth*linearDepth - clampedRadius*clampedRadius);
		float apparentCircleRadius = clampedRadius * inverseCircleDistance;
		float circleRadius = apparentCircleRadius * linearDepth;

		vec3 viewDirection = -normalize(viewPosition);

		// Precompute the constant portion of the mip bias.
		// Due to our sample projection, the sample pattern becomes stretched closer to the edge of the screen. Increase the mip bias to compensate.
		float stretchMultiplier = inversesqrt(viewDirection.z);
		float baseMipBias = log2(apparentCircleRadius * stretchMultiplier) + params.mipBias;

		// Push the center pixel's position out slightly in the direction of the normal to avoid self-shadowing.
		float pixelSize = params.normalBias * linearDepth;
		viewPosition = fma(vec3(normal), vec3(pixelSize, pixelSize, pixelSize), viewPosition);

		// Build a 2D samnple rotation matrix.
		// Instead of scaling the circle tangents later, we bake the radius scaling in here, as it's cheaper to precompute.
		vec2 scaledRotation = sampleRotation * circleRadius;
		mat3 rotationScaleMatrix = mat3(
			scaledRotation.x, -scaledRotation.y, 0.0,
			scaledRotation.y, scaledRotation.x, 0.0,
			0.0, 0.0, 1.0
		);

		// Construct the 3D sampling circle's tangents.
		vec3 right;
		vec3 up;
		cortao_constructSamplingBasis(viewDirection, right, up);

		// Randomly flip the right vector.
		// This randomly switches between a clockwise and counter-clockwise sample pattern spiral, which cancels out some errors.
		if((random.x & 1u) == 0u)
		{
			right = -right;
		}

		// Construct a 3x3 matrix that takes 2D sample positions and transforms them onto the 3D circle.
		mat3 circleMatrix = mat3(
			right,
			up,
			viewPosition
		);

		// A simplified, truncated projection matrix.
		// Only supports FOV and off-center projection; nothing else:
		// The NDC --> texture space transformation is baked into these constants on the CPU side.
		mat3 projectionMatrix = mat3(
			params.projectionParams.x, 0.0, 0.0,
			0.0, params.projectionParams.y, 0.0,
			params.projectionParams.zw, -1.0
		);

		// Multiply the 3 matrices together
		mat3 offsetTransform = (projectionMatrix * (circleMatrix * rotationScaleMatrix));

#else

		// In ortho projections, many parameters end up as constants.
		float clampedRadius = params.radius;
		vec3 viewDirection = vec3(0, 0, 1);
		float baseMipBias = params.mipBias;

		// Push the center pixel's position out slightly in the direction of the normal to avoid self-shadowing.
		float pixelSize = params.normalBias;
		viewPosition = fma(vec3(normal), vec3(pixelSize, pixelSize, pixelSize), viewPosition);

		// Build the simplified orthographic sample transformation.
		// This is really just a 3x2 matrix, but use a 3x3 matrix to match the perspective codepath.
		// Assume that the compiler can optimize away the division by 1.0 in the sample loop.
		mat3 rotationScaleMatrix = mat3(
			sampleRotation.x, -sampleRotation.y, 0.0,
			sampleRotation.y, sampleRotation.x, 0.0,
			0.0, 0.0, 1.0
		);

		mat3 projectionMatrix = mat3(
				clampedRadius * params.projectionParams.x, 0.0, 0.0,
				0.0, clampedRadius * params.projectionParams.y, 0.0,
				texCoords, 1.0
		);

		mat3 offsetTransform = (projectionMatrix * rotationScaleMatrix);
#endif

	// Generate a tangent basis for the ray directions
	vec3 t;
	vec3 bt;
	cortao_constructTangentBasis(normal, t, bt);
#if 1
	// Randomly rotate the tangent and bitangent around the normal
	float s = sin(randomTangentRotation);
	float c = cos(randomTangentRotation);
	vec3 tangent = c * t + s * bt;
	vec3 bitangent = c * bt - s * t;
#else
	// It's cheaper to not do the rotation, but this is only acceptable with many rays and few samples, so disabled for now.
	vec3 tangent = t;
	vec3 bitangent = bt;
#endif
	mat3 tangentSpaceTransform = mat3(tangent, bitangent, normal);


#if CORTAO_DEBUG_HOOKS
		cortao_debugCenterPixel(viewPosition, normal, clampedRadius);
#endif

	uint rayBits = cortao_computeRayOcclusion(offsetDither, offsetTransform, tangentSpaceTransform, baseMipBias, viewPosition, normal, vec3(viewDirection), clampedRadius, params);

	float occlusion = cortao_resolveOcclusion(rayBits);
	vec3 bentNormal = (tangentSpaceTransform * cortao_resolveBentNormal(rayBits)); // Transform tangent space bent normal back to view space.

	cortao_writeResult(ivec2(pixelCoords), occlusion, bentNormal, tangentSpaceTransform, rayBits);
}
)GLSL";

// CORTAOFilterPass.h: cortao_filterPass(), the sparse 5/13-tap edge-aware filter (Filter::SPARSE).
constexpr const char CORTAO_FILTER_PASS_SRC[] = R"GLSL(
#ifndef CORTAO_FILTER_BENT_NORMAL
#define CORTAO_FILTER_BENT_NORMAL 0
#endif

#ifndef CORTAO_LARGE_FILTER
#define CORTAO_LARGE_FILTER 0
#endif

#ifndef CORTAO_FILTER_SYMMETRY
#define CORTAO_FILTER_SYMMETRY 0
#endif

uvec4 cortao_gatherPackedData(vec2 texCoords, ivec2 offset);

#if CORTAO_FILTER_BENT_NORMAL
	mat4 cortao_gatherSSAO(vec2 texCoords, ivec2 offset);
	void cortao_writeFilterOutput(ivec2 pixelCoords, vec4 ssao);
#else
	vec4 cortao_gatherSSAO(vec2 texCoords, ivec2 offset);
	void cortao_writeFilterOutput(ivec2 pixelCoords, float ssao);
#endif

struct CORTAOFilterPassParameters {
	uint packedResolution;
	float depthSensitivity;
	float normalSensitivity;
	uint _padding0;
};

struct CORTAOWeightParams {
	vec4 depthMul;
	float depthOffset;
	vec4 deltaX;
	vec4 deltaY;

	ivec4 centerNormalX;
	ivec4 centerNormalY;

	float normalErrorMul;
};

ivec4 cortao_unpackNormalX(uvec4 packedData) {
	uint bits = uint(32);
	int normalBits = int(CORTAO_PACKED_NORMAL_BITS/2);

	return (ivec4(packedData) << (bits - normalBits)) >> (bits - normalBits);
}

ivec4 cortao_unpackNormalY(uvec4 packedData) {
	uint bits = uint(32);
	int normalBits = int(CORTAO_PACKED_NORMAL_BITS/2);

	return (ivec4(packedData) << (bits - normalBits*2)) >> (bits - normalBits);
}

vec4 calculateWeight(CORTAOWeightParams params, uvec4 packedData, ivec2 offset) {
	vec4 sampleDepth = vec4(fma(cortao_unpackDepth(packedData), params.depthMul, params.depthOffset.xxxx));
	vec4 error = abs(fma(params.deltaX, vec4(offset.xxxx), fma(params.deltaY, vec4(offset.yyyy), sampleDepth)));

	ivec4 normalX = cortao_unpackNormalX(packedData) - params.centerNormalX;
	ivec4 normalY = cortao_unpackNormalY(packedData) - params.centerNormalY;
	vec4 normalError = vec4(normalX*normalX + normalY*normalY); // can't use FMA here...
	error = fma(normalError, params.normalErrorMul.xxxx, error);

	return exp2(-error);
}

#if CORTAO_FILTER_BENT_NORMAL
	mat4 cortao_filterAccumulate(mat4 ssaoSum, vec2 texCoords, ivec2 offset, vec4 weight) {
		mat4 data = cortao_gatherSSAO(texCoords, offset);
		cortao_unroll for(uint i = 0u; i < 4u; i++) {
			ssaoSum[i] = fma(data[i], weight[i].xxxx, ssaoSum[i]);
		}
		return ssaoSum;
	}
#else
	vec4 cortao_filterAccumulate(vec4 ssaoSum, vec2 texCoords, ivec2 offset, vec4 weight) {
		return fma(cortao_gatherSSAO(texCoords, offset), weight, ssaoSum);
	}
#endif

void cortao_filterPass(uvec2 threadCoords, CORTAOFilterPassParameters parameters) {
	uvec2 resolution = cortao_getResolution(parameters.packedResolution);

	uvec2 pixelCoords = (threadCoords << uvec2(1, 1)) | uvec2(1, 1);

	// ">" is correct here, as we write to pixelCoords.x-1 as well.
	if(pixelCoords.x > resolution.x || pixelCoords.y > resolution.y) {
		return;
	}

	vec2 texCoords = vec2(pixelCoords) / vec2(resolution);


	uvec4 centerPackedData = cortao_gatherPackedData(texCoords, ivec2(0, 0));

	if(centerPackedData.x == 0u && centerPackedData.y == 0u && centerPackedData.z == 0u && centerPackedData.w == 0u) {
		return;
	}

	vec4 centerDepth = cortao_unpackDepth(centerPackedData);
	vec4 inverseCenterDepth = 1.0 / centerDepth;

	vec4 deltaX = vec4((centerDepth.xxww - centerDepth.yyzz) * inverseCenterDepth);
	vec4 deltaY = vec4((centerDepth.wzzw - centerDepth.xyyx) * inverseCenterDepth);

	CORTAOWeightParams weightParams;

	weightParams.depthMul = parameters.depthSensitivity * inverseCenterDepth;
	weightParams.depthOffset = -parameters.depthSensitivity;
	weightParams.deltaX = deltaX * float(parameters.depthSensitivity);
	weightParams.deltaY = deltaY * float(parameters.depthSensitivity);

	weightParams.centerNormalX = cortao_unpackNormalX(centerPackedData);
	weightParams.centerNormalY = cortao_unpackNormalY(centerPackedData);

	uint normalBits = uint(CORTAO_PACKED_NORMAL_BITS/2);
	weightParams.normalErrorMul = float(parameters.normalSensitivity * (1.0 / float(1u << normalBits)));

#if CORTAO_FILTER_BENT_NORMAL
		mat4 ssaoSum = mat4(
			0, 0, 0, 0,
			0, 0, 0, 0,
			0, 0, 0, 0,
			0, 0, 0, 0
		);
#else
		vec4 ssaoSum = vec4(0, 0, 0, 0);
#endif

	ssaoSum = cortao_filterAccumulate(ssaoSum, texCoords, ivec2(0, 0), vec4(1, 1, 1, 1));
	vec4 weightSum = vec4(1, 1, 1, 1);

#if CORTAO_LARGE_FILTER
		uint sampleCount = 6;
		ivec2 offsets[6];
		offsets[0] = ivec2(3, 0);
		offsets[1] = ivec2(6, 0);
		offsets[2] = ivec2(3, 3);
		offsets[3] = ivec2(0, 3);
		offsets[4] = ivec2(0, 6);
		offsets[5] = ivec2(3, -3);

#else
		uint sampleCount = 2;
		ivec2 offsets[2];
		offsets[0] = ivec2(3, 0);
		offsets[1] = ivec2(0, 3);
#endif

	 cortao_unroll for(uint i = 0u; i < sampleCount; i++) {
		ivec2 offset = offsets[i];

		vec4 weightA = calculateWeight(weightParams, cortao_gatherPackedData(texCoords, -offset), -offset);
		vec4 weightB = calculateWeight(weightParams, cortao_gatherPackedData(texCoords, +offset), +offset);

#if CORTAO_FILTER_SYMMETRY
			vec4 minWeight = min(weightA, weightB);
			weightA = minWeight;
			weightB = minWeight;
			weightSum = fma(minWeight, float(2.0).xxxx, weightSum);
#else
			weightSum += weightA + weightB;
#endif

		ssaoSum = cortao_filterAccumulate(ssaoSum, texCoords, -offset, weightA);
		ssaoSum = cortao_filterAccumulate(ssaoSum, texCoords, +offset, weightB);
	}

	vec4 inverseWeight = float(1.0) / vec4(weightSum);
	cortao_unroll for(uint i = 0u; i < 4u; i++) {
		ssaoSum[i] *= inverseWeight[i];
	}

	uvec4 coords = uvec4(pixelCoords - 1u, pixelCoords);
	uvec2 outputCoordArray[4];
	outputCoordArray[0] = coords.xw;
	outputCoordArray[1] = coords.zw;
	outputCoordArray[2] = coords.zy;
	outputCoordArray[3] = coords.xy;

	cortao_unroll for(uint i = 0u; i < 4u; i++) {
		uvec2 outputCoords = outputCoordArray[i];

		if(centerPackedData[i] != 0u && outputCoords.x < resolution.x && outputCoords.y < resolution.y) {
			cortao_writeFilterOutput(ivec2(outputCoords), ssaoSum[i]);
		}
	}
}
)GLSL";

}
