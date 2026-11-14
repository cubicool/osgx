#pragma once

// XeGTAO (https://github.com/GameTechDev/XeGTAO, v1.30, commit a5b1686), ported from HLSL to GLSL
// for osgx::GTAO: Source/Rendering/Shaders/XeGTAO.h (constants, Hilbert index) and XeGTAO.hlsli
// (prefilter, main pass, denoise), plus vaGTAO.hlsl's SpatioTemporalNoise(). Function names,
// structure and math are XeGTAO's; the HLSL-isms are translated (lpfloat -> float, frac -> fract,
// saturate -> clamp, GatherRed -> textureGather, groupshared -> shared, mul(M, v) -> v * M for the
// one matrix built element by element). Debug-visualization code and the unused NDC-depth and
// normal-from-depth paths are left out; XE_GTAO_USE_DEFAULT_CONSTANTS is 0 (every heuristic is a
// live uniform) and depths are fp32 (XE_GTAO_FP32_DEPTHS).
//
// Coordinates: XeGTAO is written for D3D - screen rows top-down, view space +z forward. Its working
// textures (depth mips, AO term, edges) are stored in exactly that layout here, so its math, gather
// component orders and edge layout apply unmodified; osgx's own callbacks in GTAO.cpp flip rows on
// the way in (G-buffer reads) and on the way out (the final output), and mirror view z (normal in,
// bent normal out).
//
// MIT License
//
// Copyright (C) 2016-2021, Intel Corporation
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

// XeGTAO.h's GTAOConstants and constants, plus the XeGTAO.hlsli helpers every pass shares.
constexpr const char XEGTAO_COMMON_SRC[] = R"GLSL(
#define XE_GTAO_DEPTH_MIP_LEVELS 5
#define XE_GTAO_NUMTHREADS_X 8
#define XE_GTAO_NUMTHREADS_Y 8

#define XE_GTAO_OCCLUSION_TERM_SCALE (1.5)

#define XE_GTAO_PI (3.1415926535897932384626433832795)
#define XE_GTAO_PI_HALF (1.5707963267948966192313216916398)

#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)
#define XE_HILBERT_AREA (XE_HILBERT_WIDTH * XE_HILBERT_WIDTH)

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	#define AOTermType vec4
#else
	#define AOTermType float
#endif

struct GTAOConstants {
	ivec2 ViewportSize;
	vec2 ViewportPixelSize;
	vec2 NDCToViewMul;
	vec2 NDCToViewAdd;
	vec2 NDCToViewMul_x_PixelSize;
	float EffectRadius;
	float EffectFalloffRange;
	float RadiusMultiplier;
	float FinalValuePower;
	float DenoiseBlurBeta;
	float SampleDistributionPower;
	float ThinOccluderCompensation;
	float DepthMIPSamplingOffset;
	int NoiseIndex;
};

// From https://www.shadertoy.com/view/3tB3z3 - except we're using R2 here
uint XeGTAO_HilbertIndex(uint posX, uint posY) {
	uint index = 0u;

	for(uint curLevel = XE_HILBERT_WIDTH / 2u; curLevel > 0u; curLevel /= 2u) {
		uint regionX = uint((posX & curLevel) > 0u);
		uint regionY = uint((posY & curLevel) > 0u);

		index += curLevel * curLevel * ((3u * regionX) ^ regionY);

		if(regionY == 0u) {
			if(regionX == 1u) {
				posX = uint((XE_HILBERT_WIDTH - 1u)) - posX;
				posY = uint((XE_HILBERT_WIDTH - 1u)) - posY;
			}

			uint temp = posX;

			posX = posY;
			posY = temp;
		}
	}

	return index;
}

vec4 XeGTAO_R8G8B8A8_UNORM_to_FLOAT4(uint packedInput) {
	vec4 unpackedOutput;

	unpackedOutput.x = float(packedInput & 0x000000ffu) / 255.0;
	unpackedOutput.y = float((packedInput >> 8) & 0x000000ffu) / 255.0;
	unpackedOutput.z = float((packedInput >> 16) & 0x000000ffu) / 255.0;
	unpackedOutput.w = float(packedInput >> 24) / 255.0;

	return unpackedOutput;
}

uint XeGTAO_FLOAT4_to_R8G8B8A8_UNORM(vec4 unpackedInput) {
	return (
		(uint(clamp(unpackedInput.x, 0.0, 1.0) * 255.0 + 0.5)) |
		(uint(clamp(unpackedInput.y, 0.0, 1.0) * 255.0 + 0.5) << 8) |
		(uint(clamp(unpackedInput.z, 0.0, 1.0) * 255.0 + 0.5) << 16) |
		(uint(clamp(unpackedInput.w, 0.0, 1.0) * 255.0 + 0.5) << 24)
	);
}

// Inputs are screen XY and viewspace depth, output is viewspace position
vec3 XeGTAO_ComputeViewspacePosition(const vec2 screenPos, const float viewspaceDepth, const GTAOConstants consts) {
	vec3 ret;

	ret.xy = (consts.NDCToViewMul * screenPos.xy + consts.NDCToViewAdd) * viewspaceDepth;
	ret.z = viewspaceDepth;

	return ret;
}

vec4 XeGTAO_CalculateEdges(const float centerZ, const float leftZ, const float rightZ, const float topZ, const float bottomZ) {
	vec4 edgesLRTB = vec4(leftZ, rightZ, topZ, bottomZ) - centerZ;

	float slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
	float slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
	vec4 edgesLRTBSlopeAdjusted = edgesLRTB + vec4(slopeLR, -slopeLR, slopeTB, -slopeTB);

	edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTBSlopeAdjusted));

	return clamp(1.25 - edgesLRTB / (centerZ * 0.011), 0.0, 1.0);
}

// packing/unpacking for edges; 2 bits per edge mean 4 gradient values (0, 0.33, 0.66, 1) for
// smoother transitions!
float XeGTAO_PackEdges(vec4 edgesLRTB) {
	edgesLRTB = round(clamp(edgesLRTB, 0.0, 1.0) * 2.9);

	return dot(edgesLRTB, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

// http://h14s.p5r.org/2012/09/0x5f3759df.html, [Drobot2014a] Low Level Optimizations for GCN,
// https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf slide 63
float XeGTAO_FastSqrt(float x) {
	return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(x) >> 1));
}

// input [-1, 1] and output [0, PI], from
// https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
float XeGTAO_FastACos(float inX) {
	const float PI = 3.141593;
	const float HALF_PI = 1.570796;
	float x = abs(inX);
	float res = -0.156583 * x + HALF_PI;

	res *= XeGTAO_FastSqrt(1.0 - x);

	return (inX >= 0.0) ? res : PI - res;
}

uint XeGTAO_EncodeVisibilityBentNormal(float visibility, vec3 bentNormal) {
	return XeGTAO_FLOAT4_to_R8G8B8A8_UNORM(vec4(bentNormal * 0.5 + 0.5, visibility));
}

void XeGTAO_DecodeVisibilityBentNormal(const uint packedValue, out float visibility, out vec3 bentNormal) {
	vec4 decoded = XeGTAO_R8G8B8A8_UNORM_to_FLOAT4(packedValue);

	// could normalize - don't want to since it's done so many times, better to do it at the final
	// step only
	bentNormal = decoded.xyz * 2.0 - 1.0;
	visibility = decoded.w;
}
)GLSL";

// vaGTAO.hlsl's SpatioTemporalNoise(), Hilbert curve driving R2 (without TAA, temporalIndex is
// always 0).
constexpr const char XEGTAO_NOISE_SRC[] = R"GLSL(
vec2 XeGTAO_SpatioTemporalNoise(uvec2 pixCoord, uint temporalIndex) {
	uint index = XeGTAO_HilbertIndex(pixCoord.x, pixCoord.y);

	// why 288? tried out a few and that's the best so far (with XE_HILBERT_LEVEL 6U) - but there's
	// probably better :)
	index += 288u * (temporalIndex % 64u);

	// R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
	return vec2(fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114)));
}
)GLSL";

// XeGTAO_PrefilterDepths16x16(): 8x8 threads, each writing a 2x2 block of MIP 0 and one texel of
// MIP 1; MIPs 2-4 reduce through shared memory. Reads osgx's XeGTAO_LoadViewspaceDepth(), writes
// xegtao_outWorkingDepth[0..4].
constexpr const char XEGTAO_PREFILTER_SRC[] = R"GLSL(
// weighted average depth filter
float XeGTAO_DepthMIPFilter(float depth0, float depth1, float depth2, float depth3, const GTAOConstants consts) {
	float maxDepth = max(max(depth0, depth1), max(depth2, depth3));

	const float depthRangeScaleFactor = 0.75; // found empirically :)
	const float effectRadius = depthRangeScaleFactor * consts.EffectRadius * consts.RadiusMultiplier;
	const float falloffRange = consts.EffectFalloffRange * effectRadius;
	const float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);

	// fadeout precompute optimisation
	const float falloffMul = -1.0 / falloffRange;
	const float falloffAdd = falloffFrom / falloffRange + 1.0;

	float weight0 = clamp((maxDepth - depth0) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight1 = clamp((maxDepth - depth1) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight2 = clamp((maxDepth - depth2) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight3 = clamp((maxDepth - depth3) * falloffMul + falloffAdd, 0.0, 1.0);

	float weightSum = weight0 + weight1 + weight2 + weight3;

	return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weightSum;
}

// XeGTAO clamps to the fp16 range in every configuration Vanilla builds (its #ifdef tests a macro
// that is always defined, as 0 or 1), so 65504 is also the effective sky/background depth.
float XeGTAO_ClampDepth(float depth) {
	return clamp(depth, 0.0, 65504.0);
}

shared float g_scratchDepths[8][8];

void XeGTAO_PrefilterDepths16x16(uvec2 dispatchThreadID, uvec2 groupThreadID, const GTAOConstants consts) {
	// MIP 0
	const uvec2 baseCoord = dispatchThreadID;
	const uvec2 pixCoord = baseCoord * 2u;
	float depth0 = XeGTAO_ClampDepth(XeGTAO_LoadViewspaceDepth(ivec2(pixCoord) + ivec2(0, 0)));
	float depth1 = XeGTAO_ClampDepth(XeGTAO_LoadViewspaceDepth(ivec2(pixCoord) + ivec2(1, 0)));
	float depth2 = XeGTAO_ClampDepth(XeGTAO_LoadViewspaceDepth(ivec2(pixCoord) + ivec2(0, 1)));
	float depth3 = XeGTAO_ClampDepth(XeGTAO_LoadViewspaceDepth(ivec2(pixCoord) + ivec2(1, 1)));

	imageStore(xegtao_outWorkingDepth[0], ivec2(pixCoord + uvec2(0, 0)), vec4(depth0));
	imageStore(xegtao_outWorkingDepth[0], ivec2(pixCoord + uvec2(1, 0)), vec4(depth1));
	imageStore(xegtao_outWorkingDepth[0], ivec2(pixCoord + uvec2(0, 1)), vec4(depth2));
	imageStore(xegtao_outWorkingDepth[0], ivec2(pixCoord + uvec2(1, 1)), vec4(depth3));

	// MIP 1
	float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3, consts);

	imageStore(xegtao_outWorkingDepth[1], ivec2(baseCoord), vec4(dm1));
	g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;

	memoryBarrierShared();
	barrier();

	// MIP 2
	if(all(equal(groupThreadID.xy % 2u, uvec2(0u)))) {
		float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
		float inTR = g_scratchDepths[groupThreadID.x + 1u][groupThreadID.y + 0u];
		float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 1u];
		float inBR = g_scratchDepths[groupThreadID.x + 1u][groupThreadID.y + 1u];

		float dm2 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);

		imageStore(xegtao_outWorkingDepth[2], ivec2(baseCoord / 2u), vec4(dm2));
		g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
	}

	memoryBarrierShared();
	barrier();

	// MIP 3
	if(all(equal(groupThreadID.xy % 4u, uvec2(0u)))) {
		float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
		float inTR = g_scratchDepths[groupThreadID.x + 2u][groupThreadID.y + 0u];
		float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 2u];
		float inBR = g_scratchDepths[groupThreadID.x + 2u][groupThreadID.y + 2u];

		float dm3 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);

		imageStore(xegtao_outWorkingDepth[3], ivec2(baseCoord / 4u), vec4(dm3));
		g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
	}

	memoryBarrierShared();
	barrier();

	// MIP 4
	if(all(equal(groupThreadID.xy % 8u, uvec2(0u)))) {
		float inTL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 0u];
		float inTR = g_scratchDepths[groupThreadID.x + 4u][groupThreadID.y + 0u];
		float inBL = g_scratchDepths[groupThreadID.x + 0u][groupThreadID.y + 4u];
		float inBR = g_scratchDepths[groupThreadID.x + 4u][groupThreadID.y + 4u];

		float dm4 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR, consts);

		imageStore(xegtao_outWorkingDepth[4], ivec2(baseCoord / 8u), vec4(dm4));
	}
}
)GLSL";

// XeGTAO_MainPass(): per pixel, sliceCount slices x stepsPerSlice steps of horizon search over
// xegtao_srcWorkingDepth, writing the packed visibility (+ bent normal) term and the packed edges.
constexpr const char XEGTAO_MAIN_PASS_SRC[] = R"GLSL(
void XeGTAO_OutputWorkingTerm(const uvec2 pixCoord, float visibility, vec3 bentNormal) {
	visibility = clamp(visibility / float(XE_GTAO_OCCLUSION_TERM_SCALE), 0.0, 1.0);

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	imageStore(xegtao_outWorkingAOTerm, ivec2(pixCoord), uvec4(XeGTAO_EncodeVisibilityBentNormal(visibility, bentNormal)));
#else
	imageStore(xegtao_outWorkingAOTerm, ivec2(pixCoord), uvec4(uint(visibility * 255.0 + 0.5)));
#endif
}

// "Efficiently building a matrix to rotate one vector to another"
// http://cs.brown.edu/research/pubs/pdfs/1999/Moller-1999-EBA.pdf / https://dl.acm.org/doi/10.1080/10867651.1999.10487509
// (using https://github.com/assimp/assimp/blob/master/include/assimp/matrix3x3.inl#L275 as a code
// reference as it seems to be best). Elements are assigned with HLSL's [row][column] indices,
// which GLSL reads as [column][row]: the result is the transpose of XeGTAO's matrix, so XeGTAO's
// mul(M, v) is v * M here.
mat3 XeGTAO_RotFromToMatrix(vec3 from, vec3 to) {
	const float e = dot(from, to);
	const float f = abs(e);

	// WARNING: This has not been tested/worked through, especially not for 16bit floats; seems to
	// work in our special use case (from is always {0, 0, -1}) but wouldn't use it in general
	if(f > float(1.0 - 0.0003)) return mat3(1, 0, 0, 0, 1, 0, 0, 0, 1);

	const vec3 v = cross(from, to);
	/* ... use this hand optimized version (9 mults less) */
	const float h = (1.0) / (1.0 + e); /* optimization by Gottfried Chen */
	const float hvx = h * v.x;
	const float hvz = h * v.z;
	const float hvxy = hvx * v.y;
	const float hvxz = hvx * v.z;
	const float hvyz = hvz * v.y;

	mat3 mtx;

	mtx[0][0] = e + hvx * v.x;
	mtx[0][1] = hvxy - v.z;
	mtx[0][2] = hvxz + v.y;

	mtx[1][0] = hvxy + v.z;
	mtx[1][1] = e + h * v.y * v.y;
	mtx[1][2] = hvyz - v.x;

	mtx[2][0] = hvxz - v.y;
	mtx[2][1] = hvyz + v.x;
	mtx[2][2] = e + hvz * v.z;

	return mtx;
}

void XeGTAO_MainPass(
	const uvec2 pixCoord,
	float sliceCount,
	float stepsPerSlice,
	const vec2 localNoise,
	vec3 viewspaceNormal,
	const GTAOConstants consts
) {
	vec2 normalizedScreenPos = (vec2(pixCoord) + 0.5) * consts.ViewportPixelSize;

	vec4 valuesUL = textureGather(xegtao_srcWorkingDepth, vec2(pixCoord) * consts.ViewportPixelSize);
	vec4 valuesBR = textureGatherOffset(xegtao_srcWorkingDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1));

	// viewspace Z at the center
	float viewspaceZ = valuesUL.y;

	// viewspace Zs left top right bottom
	const float pixLZ = valuesUL.x;
	const float pixTZ = valuesUL.z;
	const float pixRZ = valuesBR.z;
	const float pixBZ = valuesBR.x;

	vec4 edgesLRTB = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);

	imageStore(xegtao_outWorkingEdges, ivec2(pixCoord), vec4(XeGTAO_PackEdges(edgesLRTB)));

	// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer
	// imprecision; offset depends on depth texture format used (this is the FP32 one)
	viewspaceZ *= 0.99999;

	const vec3 pixCenterPos = XeGTAO_ComputeViewspacePosition(normalizedScreenPos, viewspaceZ, consts);
	const vec3 viewVec = normalize(-pixCenterPos);

	const float effectRadius = consts.EffectRadius * consts.RadiusMultiplier;
	const float sampleDistributionPower = consts.SampleDistributionPower;
	const float thinOccluderCompensation = consts.ThinOccluderCompensation;
	const float falloffRange = consts.EffectFalloffRange * effectRadius;

	const float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);

	// fadeout precompute optimisation
	const float falloffMul = -1.0 / falloffRange;
	const float falloffAdd = falloffFrom / falloffRange + 1.0;

	float visibility = 0.0;
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	vec3 bentNormal = vec3(0.0);
#else
	vec3 bentNormal = viewspaceNormal;
#endif

	// see "Algorithm 1" in
	// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
	{
		const float noiseSlice = localNoise.x;
		const float noiseSample = localNoise.y;

		// quality settings / tweaks / hacks
		// if the offset is under approx pixel size (pixelTooCloseThreshold), push it out to the
		// minimum distance
		const float pixelTooCloseThreshold = 1.3;

		// approx viewspace pixel size at pixCoord; approximation of NDCToViewspace(
		// normalizedScreenPos.xy + consts.ViewportPixelSize.xy, pixCenterPos.z ).xy - pixCenterPos.xy;
		const vec2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ * consts.NDCToViewMul_x_PixelSize;

		float screenspaceRadius = effectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;

		// fade out for small screen radii
		visibility += clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;

		// this is the min distance to start sampling from to avoid sampling from the center pixel
		// (no useful data obtained from sampling center pixel)
		const float minS = pixelTooCloseThreshold / screenspaceRadius;

		for(float slice = 0.0; slice < sliceCount; slice++) {
			float sliceK = (slice + noiseSlice) / sliceCount;
			// lines 5, 6 from the paper
			float phi = sliceK * XE_GTAO_PI;
			float cosPhi = cos(phi);
			float sinPhi = sin(phi);
			vec2 omega = vec2(cosPhi, -sinPhi);

			// convert to screen units (pixels) for later use
			omega *= screenspaceRadius;

			// line 8 from the paper
			const vec3 directionVec = vec3(cosPhi, sinPhi, 0.0);

			// line 9 from the paper
			const vec3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);

			// line 10 from the paper
			// axisVec is orthogonal to directionVec and viewVec, used to define projectedNormal
			const vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

			// line 11 from the paper
			vec3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);

			// line 13 from the paper
			float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));

			// line 14 from the paper
			float projectedNormalVecLength = length(projectedNormalVec);
			float cosNorm = clamp(dot(projectedNormalVec, viewVec) / projectedNormalVecLength, 0.0, 1.0);

			// line 15 from the paper
			float n = signNorm * XeGTAO_FastACos(cosNorm);

			// this is a lower weight target; not using -1 as in the original paper because it is
			// under horizon, so a 'weight' has different meaning based on the normal
			const float lowHorizonCos0 = cos(n + XE_GTAO_PI_HALF);
			const float lowHorizonCos1 = cos(n - XE_GTAO_PI_HALF);

			// lines 17, 18 from the paper, manually unrolled the 'side' loop
			float horizonCos0 = lowHorizonCos0;
			float horizonCos1 = lowHorizonCos1;
			// XeGTAO's "step", renamed: GLSL has a built-in step().
			for(float stepIndex = 0.0; stepIndex < stepsPerSlice; stepIndex++) {
				// R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/)
				const float stepBaseNoise = float(slice + stepIndex * stepsPerSlice) * 0.6180339887498948482;
				float stepNoise = fract(noiseSample + stepBaseNoise);

				// approx line 20 from the paper, with added noise
				float s = (stepIndex + stepNoise) / stepsPerSlice;

				// additional distribution modifier
				s = pow(s, sampleDistributionPower);

				// avoid sampling center pixel
				s += minS;

				// approx lines 21-22 from the paper, unrolled
				vec2 sampleOffset = s * omega;

				float sampleOffsetLength = length(sampleOffset);

				// note: when sampling, using point_point_point or point_point_linear sampler works,
				// but linear_linear_linear will cause unwanted interpolation between neighbouring
				// depth values on the same MIP level!
				const float mipLevel = clamp(
					log2(sampleOffsetLength) - consts.DepthMIPSamplingOffset,
					0.0,
					float(XE_GTAO_DEPTH_MIP_LEVELS)
				);

				// Snap to pixel center (more correct direction math, avoids artifacts due to
				// sampling pos not matching depth texel center - messes up slope - but adds other
				// artifacts due to them being pushed off the slice). Also use full precision for
				// high res cases.
				sampleOffset = round(sampleOffset) * consts.ViewportPixelSize;

				vec2 sampleScreenPos0 = normalizedScreenPos + sampleOffset;
				float SZ0 = textureLod(xegtao_srcWorkingDepth, sampleScreenPos0, mipLevel).x;
				vec3 samplePos0 = XeGTAO_ComputeViewspacePosition(sampleScreenPos0, SZ0, consts);

				vec2 sampleScreenPos1 = normalizedScreenPos - sampleOffset;
				float SZ1 = textureLod(xegtao_srcWorkingDepth, sampleScreenPos1, mipLevel).x;
				vec3 samplePos1 = XeGTAO_ComputeViewspacePosition(sampleScreenPos1, SZ1, consts);

				vec3 sampleDelta0 = (samplePos0 - pixCenterPos);
				vec3 sampleDelta1 = (samplePos1 - pixCenterPos);
				float sampleDist0 = length(sampleDelta0);
				float sampleDist1 = length(sampleDelta1);

				// approx lines 23, 24 from the paper, unrolled
				vec3 sampleHorizonVec0 = sampleDelta0 / sampleDist0;
				vec3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

				// any sample out of radius should be discarded - also use fallof range for smooth
				// transitions; this is a modified idea from "4.3 Implementation details, Bounding
				// the sampling area"; this is our own thickness heuristic that relies on sooner
				// discarding samples behind the center
				float falloffBase0 = length(vec3(sampleDelta0.x, sampleDelta0.y, sampleDelta0.z * (1.0 + thinOccluderCompensation)));
				float falloffBase1 = length(vec3(sampleDelta1.x, sampleDelta1.y, sampleDelta1.z * (1.0 + thinOccluderCompensation)));
				float weight0 = clamp(falloffBase0 * falloffMul + falloffAdd, 0.0, 1.0);
				float weight1 = clamp(falloffBase1 * falloffMul + falloffAdd, 0.0, 1.0);

				// sample horizon cos
				float shc0 = dot(sampleHorizonVec0, viewVec);
				float shc1 = dot(sampleHorizonVec1, viewVec);

				// discard unwanted samples
				shc0 = mix(lowHorizonCos0, shc0, weight0);
				shc1 = mix(lowHorizonCos1, shc1, weight1);

				// thickness heuristic - see "4.3 Implementation details, Height-field assumption
				// considerations" (XeGTAO's active variant: completely disabled)
				horizonCos0 = max(horizonCos0, shc0);
				horizonCos1 = max(horizonCos1, shc1);
			}

			// I can't figure out the slight overdarkening on high slopes, so I'm adding this fudge -
			// in the training set, 0.05 is close (PSNR 21.34) to disabled (PSNR 21.45)
			projectedNormalVecLength = mix(projectedNormalVecLength, 1.0, 0.05);

			// line ~27, unrolled
			float h0 = -XeGTAO_FastACos(horizonCos1);
			float h1 = XeGTAO_FastACos(horizonCos0);

			float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
			float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
			float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);

			visibility += localVisibility;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
			// see "Algorithm 2 Extension that computes bent normals b."
			float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
			float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) - 3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
			vec3 localBentNormal = vec3(directionVec.x * t0, directionVec.y * t0, -t1);

			localBentNormal = (localBentNormal * XeGTAO_RotFromToMatrix(vec3(0.0, 0.0, -1.0), viewVec)) * projectedNormalVecLength;
			bentNormal += localBentNormal;
#endif
		}

		visibility /= sliceCount;
		visibility = pow(visibility, consts.FinalValuePower);
		// disallow total occlusion (which wouldn't make any sense anyhow since pixel is visible but
		// also helps with packing bent normals)
		visibility = max(0.03, visibility);

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
		bentNormal = normalize(bentNormal);
#endif
	}

	XeGTAO_OutputWorkingTerm(pixCoord, visibility, bentNormal);
}
)GLSL";

// XeGTAO_Denoise(): edge-aware 3x3 blur of the working term, 2 horizontal pixels per thread;
// finalApply (XE_GTAO_FINAL_APPLY) is the last pass, which rescales by
// XE_GTAO_OCCLUSION_TERM_SCALE. Reads xegtao_srcWorkingAOTerm/xegtao_srcWorkingEdges, writes
// through osgx's XeGTAO_WriteOutput().
constexpr const char XEGTAO_DENOISE_SRC[] = R"GLSL(
#ifdef XE_GTAO_FINAL_APPLY
	const bool XE_GTAO_FINAL_APPLY_VALUE = true;
#else
	const bool XE_GTAO_FINAL_APPLY_VALUE = false;
#endif

vec4 XeGTAO_UnpackEdges(float _packedVal) {
	uint packedVal = uint(_packedVal * 255.5);
	vec4 edgesLRTB;

	// there's really no need for mask (as it's an 8 bit input) but I'll leave it in so it doesn't
	// cause any trouble in the future
	edgesLRTB.x = float((packedVal >> 6) & 0x03u) / 3.0;
	edgesLRTB.y = float((packedVal >> 4) & 0x03u) / 3.0;
	edgesLRTB.z = float((packedVal >> 2) & 0x03u) / 3.0;
	edgesLRTB.w = float((packedVal >> 0) & 0x03u) / 3.0;

	return clamp(edgesLRTB, 0.0, 1.0);
}

void XeGTAO_AddSample(AOTermType ssaoValue, float edgeValue, inout AOTermType sum, inout float sumWeight) {
	float weight = edgeValue;

	sum += (weight * ssaoValue);
	sumWeight += weight;
}

void XeGTAO_Output(uvec2 pixCoord, AOTermType outputValue, const bool finalApply) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	float visibility = outputValue.w * ((finalApply) ? float(XE_GTAO_OCCLUSION_TERM_SCALE) : 1.0);
	vec3 bentNormal = normalize(outputValue.xyz);

	XeGTAO_WriteOutput(pixCoord, visibility, bentNormal);
#else
	outputValue *= (finalApply) ? float(XE_GTAO_OCCLUSION_TERM_SCALE) : 1.0;

	XeGTAO_WriteOutput(pixCoord, outputValue, vec3(0.0));
#endif
}

void XeGTAO_DecodeGatherPartial(const uvec4 packedValue, out AOTermType outDecoded[4]) {
	for(int i = 0; i < 4; i++) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
		XeGTAO_DecodeVisibilityBentNormal(packedValue[i], outDecoded[i].w, outDecoded[i].xyz);
#else
		outDecoded[i] = float(packedValue[i]) / 255.0;
#endif
	}
}

void XeGTAO_Denoise(const uvec2 pixCoordBase, const GTAOConstants consts, const bool finalApply) {
	const float blurAmount = (finalApply) ? consts.DenoiseBlurBeta : consts.DenoiseBlurBeta / 5.0;
	const float diagWeight = 0.85 * 0.5;

	AOTermType aoTerm[2]; // pixel pixCoordBase and pixel pixCoordBase + int2( 1, 0 )
	vec4 edgesC_LRTB[2];
	float weightTL[2];
	float weightTR[2];
	float weightBL[2];
	float weightBR[2];

	// gather edge and visibility quads, used later
	const vec2 gatherCenter = vec2(pixCoordBase.x, pixCoordBase.y) * consts.ViewportPixelSize;
	vec4 edgesQ0 = textureGatherOffset(xegtao_srcWorkingEdges, gatherCenter, ivec2(0, 0));
	vec4 edgesQ1 = textureGatherOffset(xegtao_srcWorkingEdges, gatherCenter, ivec2(2, 0));
	vec4 edgesQ2 = textureGatherOffset(xegtao_srcWorkingEdges, gatherCenter, ivec2(1, 2));

	AOTermType visQ0[4];
	AOTermType visQ1[4];
	AOTermType visQ2[4];
	AOTermType visQ3[4];

	XeGTAO_DecodeGatherPartial(textureGatherOffset(xegtao_srcWorkingAOTerm, gatherCenter, ivec2(0, 0)), visQ0);
	XeGTAO_DecodeGatherPartial(textureGatherOffset(xegtao_srcWorkingAOTerm, gatherCenter, ivec2(2, 0)), visQ1);
	XeGTAO_DecodeGatherPartial(textureGatherOffset(xegtao_srcWorkingAOTerm, gatherCenter, ivec2(0, 2)), visQ2);
	XeGTAO_DecodeGatherPartial(textureGatherOffset(xegtao_srcWorkingAOTerm, gatherCenter, ivec2(2, 2)), visQ3);

	for(int side = 0; side < 2; side++) {
		const uvec2 pixCoord = uvec2(pixCoordBase.x + uint(side), pixCoordBase.y);

		vec4 edgesL_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.x) : (edgesQ0.y));
		vec4 edgesT_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.z) : (edgesQ1.w));
		vec4 edgesR_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ1.x) : (edgesQ1.y));
		vec4 edgesB_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ2.w) : (edgesQ2.z));

		edgesC_LRTB[side] = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.y) : (edgesQ1.x));

		// Edges aren't perfectly symmetrical: edge detection algorithm does not guarantee that a
		// left edge on the right pixel will match the right edge on the left pixel (although they
		// will match in majority of cases). This line further enforces the symmetricity, creating a
		// slightly sharper blur. Works real nice with TAA.
		edgesC_LRTB[side] *= vec4(edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z);

		// this allows some small amount of AO leaking from neighbours if there are 3 or 4 edges;
		// this reduces both spatial and temporal aliasing
		const float leak_threshold = 2.5;
		const float leak_strength = 0.5;
		float edginess = (clamp(4.0 - leak_threshold - dot(edgesC_LRTB[side], vec4(1.0)), 0.0, 1.0) / (4.0 - leak_threshold)) * leak_strength;

		edgesC_LRTB[side] = clamp(edgesC_LRTB[side] + edginess, 0.0, 1.0);

		// for diagonals; used by first and second pass
		weightTL[side] = diagWeight * (edgesC_LRTB[side].x * edgesL_LRTB.z + edgesC_LRTB[side].z * edgesT_LRTB.x);
		weightTR[side] = diagWeight * (edgesC_LRTB[side].z * edgesT_LRTB.y + edgesC_LRTB[side].y * edgesR_LRTB.z);
		weightBL[side] = diagWeight * (edgesC_LRTB[side].w * edgesB_LRTB.x + edgesC_LRTB[side].x * edgesL_LRTB.w);
		weightBR[side] = diagWeight * (edgesC_LRTB[side].y * edgesR_LRTB.w + edgesC_LRTB[side].w * edgesB_LRTB.y);

		// first pass
		AOTermType ssaoValue = (side == 0) ? (visQ0[1]) : (visQ1[0]);
		AOTermType ssaoValueL = (side == 0) ? (visQ0[0]) : (visQ0[1]);
		AOTermType ssaoValueT = (side == 0) ? (visQ0[2]) : (visQ1[3]);
		AOTermType ssaoValueR = (side == 0) ? (visQ1[0]) : (visQ1[1]);
		AOTermType ssaoValueB = (side == 0) ? (visQ2[2]) : (visQ3[3]);
		AOTermType ssaoValueTL = (side == 0) ? (visQ0[3]) : (visQ0[2]);
		AOTermType ssaoValueBR = (side == 0) ? (visQ3[3]) : (visQ3[2]);
		AOTermType ssaoValueTR = (side == 0) ? (visQ1[3]) : (visQ1[2]);
		AOTermType ssaoValueBL = (side == 0) ? (visQ2[3]) : (visQ2[2]);

		float sumWeight = blurAmount;
		AOTermType sum = ssaoValue * sumWeight;

		XeGTAO_AddSample(ssaoValueL, edgesC_LRTB[side].x, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueR, edgesC_LRTB[side].y, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueT, edgesC_LRTB[side].z, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueB, edgesC_LRTB[side].w, sum, sumWeight);

		XeGTAO_AddSample(ssaoValueTL, weightTL[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueTR, weightTR[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueBL, weightBL[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueBR, weightBR[side], sum, sumWeight);

		aoTerm[side] = sum / sumWeight;

		XeGTAO_Output(pixCoord, aoTerm[side], finalApply);
	}
}
)GLSL";

}
