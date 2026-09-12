#pragma once

#include "IBL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Image>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// Requested quality for a GPU cosine-convolution of an equirectangular HDR. The resulting cube
// stores diffuse irradiance divided by pi, matching the existing CPU Lambertian cube convention.
struct LambertianBakeOptions {
	int cubeSize = 256;
	int sampleCount = 2048;

	// Same firefly-suppression tradeoff as GGXPrefilterOptions::fireflyClamp (see that comment
	// for the full explanation, including why the default has to sit near the scene's *normal*
	// luminance range rather than merely "below the sun") - caps a single equirect sample's
	// luminance before it enters the cosine-weighted hemisphere average, so a bright sun disc
	// doesn't show up as visible Hammersley-pattern speckle in the diffuse irradiance cube.
	float fireflyClamp = 8.0f;
};

// A frame-driven GPU bake. Add root to a viewer scene, advance frames, then use diffuseTexture
// once completion reports done. `rebake()` re-arms the same passes for a new
// HDR image without recreating their cameras or output texture.
struct LambertianBakeScene {
	osg::ref_ptr<osg::Group> root;
	osg::ref_ptr<osg::Texture2D> sourceTexture;
	osg::ref_ptr<osg::TextureCubeMap> diffuseTexture;
	osg::ref_ptr<BakeCompletion> completion;

	bool ready() const;

	static LambertianBakeScene create(
		osg::Image* equirectangularHDR,
		const LambertianBakeOptions& options={}
	);

	// Re-arms this bake scene's existing passes for a new HDR image without recreating their
	// cameras or output texture.
	bool rebake(osg::Image* equirectangularHDR);
};

// A CPU-readback companion for LambertianBakeScene, mirroring GGXPrefilterReadback (see
// GGXPrefilter.hpp) - exists purely for the offline serialize-to-KTX2 use case. Attach to a
// viewer's OUTER camera (not any of the six face cameras inside LambertianBakeScene::root), the
// same way osggltf-iblbake-gpu attaches GGXPrefilterReadback to viewer.getCamera(). Interactive/
// dynamic-probe consumers of LambertianBakeScene::create() never construct one of these, so they
// never pay for the glFinish-gated readback below - LambertianBakeScene::completion alone is
// enough for them. Triggers off that exact completion signal rather than a frame-count heuristic,
// since one is already available here (GGX has no equivalent, hence its own trigger style).
class LambertianCubeReadback: public osg::Camera::DrawCallback {
public:
	LambertianCubeReadback(osg::TextureCubeMap* srcTex, BakeCompletion* completion, bool sync=true);

	void operator()(osg::RenderInfo& ri) const override;

	bool isDone() const { return _done; }
	osg::TextureCubeMap* getResult() const { return _result; }

	// Returns the finished, CPU-readable cubemap once isDone() reports true (nullptr otherwise),
	// with filter/wrap state set for normal sampling - mirrors GGXPrefilterReadback::finish()'s
	// contract.
	osg::ref_ptr<osg::TextureCubeMap> finish() const;

private:
	osg::ref_ptr<osg::TextureCubeMap> _srcTex;
	osg::ref_ptr<BakeCompletion> _completion;
	bool _sync;
	mutable osg::ref_ptr<osg::TextureCubeMap> _result;
	mutable bool _done = false;
};

}
