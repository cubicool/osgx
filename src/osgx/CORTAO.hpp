#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/DispatchCompute>
#include <osg/Group>
#include <osg/Image>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// Circular Occluder Ray-Traced Ambient Occlusion (CORTAO, Daniel Isheden, MIT,
// https://github.com/theagentd/CORTAO) - an experimental alternative to osgx::SSAO with the same
// inputs (a G-buffer's view-space normal + eye-space position, plus the projection uniform) and the
// same output contract (`aoTexture`, single-channel `GL_R8`, 1.0 = unoccluded), so it plugs into
// `PBRLightingPass::Options::aoTexture` the same way. The reference shaders are ported to plain GLSL
// in src/CORTAOShaders.hpp (MIT, upstream v1.0.0).
//
// Up to three compute passes from the reference implementation:
//
// - Prepass (8x8 workgroups): packs linear depth (read as -gPosition.z, never hardware depth, so
//   no near/far is ever needed) plus a low-precision spheremap normal into `packedTexture`
//   (`GL_R32UI`, up to 8 mips), writing every mip in one dispatch.
// - Main pass (16x16 workgroups): traces CORTAO_RAY_COUNT (16) rays per pixel against circular
//   occluders built from `packedTexture` samples, via `occluderLookupTable`, writing
//   `rawAoTexture` (and `bentNormalTexture`, if requested).
// - Filter pass: `rawAoTexture` -> `aoTexture`, one of (see Filter, setFilter()):
//   - TILED (default, osgx's own): the main pass's per-pixel randomness repeats every 4x4 pixels
//     (its cortao_seed() calls see pixel coordinates masked to 0-3), and a dense depth/normal-aware
//     4x4 average then covers all 16 tile phases at every pixel, cancelling the pattern on flat
//     ground - smooth and frame-stable without temporal filtering.
//   - SPARSE: CORTAO's own 5- or 13-tap prefilter (8x8 workgroups of 2x2 pixels each). The
//     reference intends it to be followed by temporal filtering (a new randomSeed per frame), which
//     osgx doesn't do; on its own it leaves a regular lattice (its taps are 3 and 6 pixels apart).
//   - NONE: a copy of `rawAoTexture`.
//
// All dispatches live under `node` with fixed RenderBin numbers (-3, -2, -1) and glMemoryBarrier()
// after each. `node` must be traversed by a camera whose render stage runs after the camera that
// writes normal/position and before whatever samples the outputs - e.g. directly under the
// viewer's main camera, between a PRE_RENDER G-buffer camera and a POST_RENDER lighting pass.
struct CORTAO {
	enum class Filter {
		NONE,
		SPARSE,
		TILED
	};

	struct Options {
		// World-space (scene units) maximum occlusion radius, scale-dependent like SSAO::radius.
		float maxRadius = 0.5f;

		// Initial CORTAO_SAMPLE_COUNT; see setSampleCount().
		unsigned int sampleCount = 16;

		// The filter pass's initial mode; see setFilter().
		Filter filter = Filter::TILED;

		// Also writes `bentNormalTexture`: `GL_RGBA8_SNORM`, xyz = view-space bent normal (the
		// average unoccluded direction; the surface normal where every ray is occluded), w =
		// occlusion. Unfiltered.
		bool bentNormal = false;
	};

	osg::ref_ptr<osg::Group> node;
	osg::ref_ptr<osg::DispatchCompute> prepass;
	osg::ref_ptr<osg::DispatchCompute> mainPass;
	osg::ref_ptr<osg::DispatchCompute> filterPass;

	// The filter pass's two programs, swapped by setFilter() (NONE uses the TILED program's copy
	// variant).
	osg::ref_ptr<osg::Program> sparseFilterProgram;
	osg::ref_ptr<osg::Program> tiledFilterProgram;

	// The result to consume: the filter pass's output.
	osg::ref_ptr<osg::Texture2D> aoTexture;
	// The main pass's per-pixel output: occlusion quantized to 1/16 (16 rays), each pixel with its
	// own random sample rotation, constant across frames for a constant `randomSeed`.
	osg::ref_ptr<osg::Texture2D> rawAoTexture;
	osg::ref_ptr<osg::Texture2D> bentNormalTexture;
	osg::ref_ptr<osg::Texture2D> packedTexture;
	osg::ref_ptr<osg::Texture3D> occluderLookupTable;

	// Live-tunable: call ->set(value) at any time, no pass rebuild needed. Names and meanings
	// match CORTAOConfigurationParameters in the reference README; the derived per-pass values
	// are computed in the shaders from these plus the projection uniform.
	osg::ref_ptr<osg::Uniform> maxRadius;
	osg::ref_ptr<osg::Uniform> maxScreenspaceRadiusHeightRatio;
	osg::ref_ptr<osg::Uniform> mipBias;
	osg::ref_ptr<osg::Uniform> maxSlopeCompensation;
	osg::ref_ptr<osg::Uniform> backfaceThreshold;
	osg::ref_ptr<osg::Uniform> backfaceBoost;
	osg::ref_ptr<osg::Uniform> filterDepthSensitivity;
	osg::ref_ptr<osg::Uniform> filterNormalSensitivity;
	// Constant by default; re-randomize per frame only with some temporal filter downstream.
	osg::ref_ptr<osg::Uniform> randomSeed;

	bool valid() const;

	// The compile-time switches below are `#pragma import_defines` defines on their pass's
	// StateSet: OSG compiles and caches one Program variant per combination, so each first use of
	// a new value costs a shader compile.
	//
	// CORTAO_SAMPLE_COUNT, the primary quality/cost knob (4-32 recommended).
	void setSampleCount(unsigned int count);
	unsigned int getSampleCount() const;

	// The filter pass's mode (program swap plus the main pass's tiled-noise define). The depth/normal
	// sensitivity uniforms apply to SPARSE and TILED.
	void setFilter(Filter filter);
	Filter getFilter() const;

	// SPARSE only: CORTAO_LARGE_FILTER (13 taps instead of 5; default on).
	void setLargeFilter(bool large);
	bool getLargeFilter() const;

	// SPARSE only: CORTAO_FILTER_SYMMETRY (centered kernel weights: better contact shadows, less
	// filtering near edges; default on).
	void setFilterSymmetry(bool symmetry);
	bool getFilterSymmetry() const;

	// `normalTexture` must be view-space and `positionTexture` eye-space with background pixels
	// left at 0 (PBRGBuffer's contract, same as SSAO::create()). `projectionMatrix` is the same
	// caller-owned, draw-time-captured uniform SSAO::create() takes (an osgx::
	// DepthProjectionCallback's getProjection(), named "projectionMatrix"). Perspective
	// projections only.
	static CORTAO create(
		osg::Texture2D* normalTexture,
		osg::Texture2D* positionTexture,
		osg::Uniform* projectionMatrix,
		int width,
		int height,
		const Options& options
	);
	static CORTAO create(
		osg::Texture2D* normalTexture,
		osg::Texture2D* positionTexture,
		osg::Uniform* projectionMatrix,
		int width,
		int height
	);

	// The occluder lookup table as a 3D `GL_R{8,16,32}UI` image (rayCount 8/16/32), a C++ port of
	// the reference `utils/generators/OccluderLookupTableGenerator.py`: for each spheremap-encoded
	// occluder direction (x, y) and occluder radius (z), the bitmask of rays the occluder blocks.
	// Deterministic; built at startup instead of loading the reference `tables/occluder/*.bin`.
	static osg::ref_ptr<osg::Image> createOccluderLookupTable(
		unsigned int rayCount=16,
		unsigned int directionResolution=24,
		unsigned int radiusResolution=16
	);
};

}
