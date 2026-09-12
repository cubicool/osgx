#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Node>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// A small screen-space silhouette expansion pipeline, in exactly 3 passes: `selectionCamera`
// renders `selected` into originalMask (mask) + originalDepth (eye-space Z of that same surface),
// then dilateXCamera and dilateYCamera perform a separable max filter propagating BOTH channels as
// values - not a UV to re-sample later. `expanded` stores mask in .r, propagated eye-space depth
// in .g, and Chebyshev distance in pixels in .a (.b is reserved/unused). A caller's final effect
// remains separate: sample both originalMask/expanded and calculate
// `expanded.r * (1.0 - originalMask.r)` there.
//
// Depth-aware vs. "x-ray" occlusion is entirely a composite-shader decision, not an Aura mode:
// discard a ring pixel when the real scene is nearer than `expanded.g` there for a grounded look,
// or ignore depth entirely for an always-on-top "target lock" look - both read the exact same
// Aura output, so a caller can even flip between them live off one uniform.
//
// Depth (like mask) is propagated as a VALUE through the same nearest-neighbor search, not as a
// coordinate for a later indirect lookup. Equal-distance ties retain scan order, so which candidate
// wins can still change pixel to pixel, but now a tie can only jump between two depths the search
// ALREADY found near that pixel - bounded by local geometry. Propagating a UV instead (the design
// prior to 2026-09-03) let a tie's indirect re-lookup land anywhere on the selected surface,
// including a completely unrelated point on a curved/concave object; that showed up as a visible
// crack once used to drive occlusion, which is far more sensitive to jumps than a stripe/color
// pattern ever was.
struct Aura {
	osg::ref_ptr<osg::Camera> selectionCamera;
	osg::ref_ptr<osg::Camera> dilateXCamera;
	osg::ref_ptr<osg::Camera> dilateYCamera;
	osg::ref_ptr<osg::Texture2D> originalMask;
	osg::ref_ptr<osg::Texture2D> originalDepth;
	osg::ref_ptr<osg::Texture2D> dilatedX;
	osg::ref_ptr<osg::Texture2D> expanded;
	// Radius in output pixels. Both dilation shaders clamp it to their fixed maximum of 64.
	osg::ref_ptr<osg::Uniform> radius;

	bool valid() const;

	// Builds the pipeline with an empty selectionCamera - add the returned cameras to the render
	// graph in the listed order (PRE_RENDER order 1, 2, 3, leaving order 0 for a preceding G-buffer
	// geometry pass), then call `selectionCamera->addChild(node)` for whatever should cast the aura.
	// A node can be multi-parented normally: once under the application's visible scene, and once
	// under selectionCamera. The selection camera's protected flat-mask Program wins over a material
	// Program installed below it, so this works for arbitrary already-rendered geometry.
	//
	// Since selectionCamera is a plain osg::Group, its children can be swapped at any time - e.g.
	// addChild()/removeChild() from an onEnter/onLeave hover callback to retarget one shared pipeline
	// at whichever object is currently selected, instead of building one Aura per candidate object.
	// With no child attached, originalMask stays all-zero and the pipeline naturally renders nothing.
	static Aura create(int width, int height, int radius=8);
};

}
