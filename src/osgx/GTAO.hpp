#pragma once

#include "ComputePass.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Group>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <array>

namespace osgx {

// Ground Truth Ambient Occlusion (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
// Indirect Occlusion"), as implemented by Intel's XeGTAO (https://github.com/GameTechDev/XeGTAO,
// MIT), ported to GLSL in src/GTAOShaders.hpp. Horizon-based: per pixel, a few screen-space slices,
// each searched in both directions for the highest horizon, the visible arc between them integrated
// analytically with cosine weighting.
//
// Same inputs as osgx::SSAO/osgx::CORTAO (a G-buffer's view-space normal + eye-space position, plus
// the projection uniform) and the same output contract (`aoTexture`, single-channel `GL_R8`, 1.0 =
// unoccluded), so it plugs into `PBRLightingPass::Options::aoTexture` the same way.
//
// Compute passes, all osgx::ComputePass nodes under `node` ordered by RenderBin (-5 to -1):
//
// - Prefilter: view-space depth (read as -gPosition.z, so near/far are never needed) into the 5-mip
//   `depthTexture`.
// - Main: the horizon search (slice/step counts per Quality), writing a packed visibility term and
//   packed depth edges.
// - Denoise: getDenoisePasses() edge-aware 3x3 blurs; the last one writes `aoTexture` (and
//   `bentNormalTexture`). With 0 passes the last one still runs, with the blur disabled.
//
// XeGTAO's own heuristics are live uniforms with its defaults (radiusMultiplier etc., tuned by
// Intel against ray-traced ground truth). XeGTAO is designed to be followed by TAA (a new noise
// index per frame); without it, the denoiser alone smooths its fixed noise. The same traversal
// requirement as CORTAO applies: `node` must be traversed after the G-buffer is written and before
// anything samples the outputs.
struct GTAO {
	// Slices x steps per slice: LOW 1x2, MEDIUM 2x2, HIGH 3x3, ULTRA 9x3.
	enum class Quality {
		LOW,
		MEDIUM,
		HIGH,
		ULTRA
	};

	struct Options {
		// World-space (scene units) occlusion radius, scale-dependent like SSAO::radius.
		float radius = 0.5f;

		Quality quality = Quality::HIGH;

		// 0-3; see setDenoisePasses().
		unsigned int denoisePasses = 1;

		// Also writes `bentNormalTexture`: `GL_RGBA8_SNORM`, xyz = view-space bent normal, w =
		// visibility (same layout as CORTAO's).
		bool bentNormal = false;
	};

	osg::ref_ptr<osg::Group> node;
	osg::ref_ptr<ComputePass> prefilterPass;
	osg::ref_ptr<ComputePass> mainPass;
	// The intermediate denoise passes (ping-pong between `workingTextures`) and the last one.
	std::array<osg::ref_ptr<ComputePass>, 2> denoisePasses;
	osg::ref_ptr<ComputePass> finalDenoisePass;

	// The result to consume.
	osg::ref_ptr<osg::Texture2D> aoTexture;
	osg::ref_ptr<osg::Texture2D> bentNormalTexture;
	// View-space depth, 5 mips, GL_R32F; rows top-down (XeGTAO's layout).
	osg::ref_ptr<osg::Texture2D> depthTexture;
	// Packed depth edges (GL_R8) and the packed working term (GL_R8UI, or GL_R32UI with bent
	// normals), rows top-down.
	osg::ref_ptr<osg::Texture2D> edgesTexture;
	std::array<osg::ref_ptr<osg::Texture2D>, 2> workingTextures;

	// Live-tunable: call ->set(value) at any time. Names and defaults match XeGTAO's GTAOSettings.
	osg::ref_ptr<osg::Uniform> radius;
	// "Auto-tuned settings (heuristics)" in XeGTAO's UI.
	osg::ref_ptr<osg::Uniform> radiusMultiplier;
	osg::ref_ptr<osg::Uniform> falloffRange;
	osg::ref_ptr<osg::Uniform> sampleDistributionPower;
	osg::ref_ptr<osg::Uniform> thinOccluderCompensation;
	osg::ref_ptr<osg::Uniform> finalValuePower;
	osg::ref_ptr<osg::Uniform> depthMipSamplingOffset;
	// 0 by default; set frameNumber % 64 only with some temporal filter downstream.
	osg::ref_ptr<osg::Uniform> noiseIndex;

	bool valid() const;

	// A `#pragma import_defines` value on the main pass (one Program variant per quality level).
	void setQuality(Quality quality);
	Quality getQuality() const;

	// 0 (no blur), 1 (sharp), 2 (medium) or 3 (soft); clamped to 3.
	void setDenoisePasses(unsigned int passes);
	unsigned int getDenoisePasses() const;

	// Same contract as SSAO::create()/CORTAO::create(): `normalTexture` view-space,
	// `positionTexture` eye-space with background pixels left at 0, `projectionMatrix` the
	// caller-owned, draw-time-captured "projectionMatrix" uniform. Perspective projections only
	// (off-center is fine).
	static GTAO create(
		osg::Texture2D* normalTexture,
		osg::Texture2D* positionTexture,
		osg::Uniform* projectionMatrix,
		int width,
		int height,
		const Options& options
	);
	static GTAO create(
		osg::Texture2D* normalTexture,
		osg::Texture2D* positionTexture,
		osg::Uniform* projectionMatrix,
		int width,
		int height
	);

private:
	osg::ref_ptr<osg::Uniform> _denoiseBlurBeta;
	unsigned int _denoisePasses = 1;
};

}
