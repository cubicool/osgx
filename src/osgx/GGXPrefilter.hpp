#pragma once

#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

namespace osg { class Image; }

namespace osgx {

// TODO: If these can't be NEGATIVE, they need to be OTHER TYPES than `int`!
struct GGXPrefilterOptions {
	int prefilterSize = 128;
	int sampleCount = 1024;
	int maxFrames = 8;
	int readbackFrame = 2;

	// Caps the luminance of any single equirect sample before it's accumulated into the
	// weighted average. Real photographed HDRIs can have a sun disc with peak radiance in the
	// tens of thousands - once that's correctly finite (not clamped to +Infinity by a too-narrow
	// storage format), it's still ~1000x brighter than the surrounding sky, so a handful of the
	// (deterministic, low-discrepancy) Hammersley samples that happen to land near it dominate
	// the average and show up as visible firefly noise/sparkle instead of blending in --
	// especially visible at low roughness, where few samples ever collapse to the same direction.
	//
	// Measured on ingwe_beach_sunny_2k.hdr (a real photographed sky with a visible sun): median
	// luminance 0.11, p99 0.68, p99.9 4.5, p99.99 28, sun peak ~84480. The clamp has to land near
	// that *normal* range to matter, not merely "below the sun" - any standard display tonemap
	// (e.g. Reinhard x/(x+1)) already saturates to near-white well before 50 (tonemap(50) = 0.98,
	// tonemap(84480) = 0.99999 - both read as "pure white" on screen), so a clamp value that's
	// merely "huge but smaller than the sun" is visually indistinguishable from no clamp at all.
	// Default (8) sits just above the top of this HDRI's *normal* sky/cloud range so the sun's
	// outlier samples actually blend in, at the cost of flattening the very brightest highlights
	// too - tune per-scene if that tradeoff isn't right for a given HDRI.
	float fireflyClamp = 8.0f;

	// bool configureGLContext = true;

	// If true, GGXPrefilterReadback calls glFinish() immediately before reading the
	// baked cubemap back from the GPU (deterministic, but stalls the
	// pipeline). If false, it trusts that `readbackFrame` frames having
	// already elapsed is enough for the GPU to have caught up, and skips
	// the stall. TODO: no caller flips this yet; it exists so a future
	// best-effort/async bake mode can do so without touching GGXPrefilterReadback.
	bool syncReadback = true;
};

// Sets the OSG_GL_* / OSG_THREADING environment variables so that a graphics
// context created afterward (e.g. by an osgViewer::Viewer) is compatible with
// the GLSL 4.60 prefilter shaders. Must be called before that context exists.
// void configureIBLGLContext();

// Post-draw callback that, once attached to a rendering camera, waits until
// `triggerFrame` frames have been rendered and then reads the prefiltered
// cubemap back from the GPU into `getResult()`.
class GGXPrefilterReadback: public osg::Camera::DrawCallback {
public:
	GGXPrefilterReadback(osg::TextureCubeMap* srcTex, int triggerFrame, bool sync);

	void operator()(osg::RenderInfo& ri) const override;

	bool isDone() const { return done; }
	osg::TextureCubeMap* getResult() const { return result; }
	void reset();

	// Returns the finished, filtered/wrapped cubemap once isDone() is true (nullptr otherwise).
	// Only valid to call once isDone() reports true.
	osg::ref_ptr<osg::TextureCubeMap> finish() const;

private:
	osg::ref_ptr<osg::TextureCubeMap> srcTex;
	int triggerFrame = 0;
	bool sync = true;
	mutable int frameCount = 0;
	mutable osg::ref_ptr<osg::TextureCubeMap> result;
	mutable bool done = false;
};

struct GGXPrefilterScene {
	osg::ref_ptr<osg::Group> root;
	osg::ref_ptr<osg::Texture2D> sourceTexture;
	osg::ref_ptr<osg::TextureCubeMap> prefilterTexture;
	osg::ref_ptr<GGXPrefilterReadback> readback;

	// Builds the offscreen scene graph (PRE_RENDER cameras, one per cubemap
	// face/mip) that GGX-prefilters `equirectImage`. Does not render anything:
	// the caller owns the graphics context/viewer, is responsible for setting
	// `root` as scene data, attaching `readback` as a post-draw callback on the
	// camera that will actually render frames, and running frames until
	// `readback->isDone()`.
	static GGXPrefilterScene create(
		osg::Image* equirectImage,
		const GGXPrefilterOptions& options = {}
	);

	// Reuses this bake scene for a new equirectangular source image. This avoids rebuilding all
	// PRE_RENDER cameras/FBOs/programs for live rebakes.
	bool rebake(osg::Image* equirectImage);
};

}
