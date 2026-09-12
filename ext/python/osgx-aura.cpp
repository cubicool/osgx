#include "osgx-python.hpp"
#include "osgx/Aura.hpp"

namespace osgx_python {

void bind_aura(py::module_& m) {
	py::class_<osgx::Aura>(
		m,
		"Aura",
		"A screen-space silhouette expansion pipeline: selectionCamera renders `selected` into "
		"originalMask + originalDepth, then two fullscreen passes separable-max-filter both into "
		"`expanded` (mask in .r, propagated eye-space depth in .g, Chebyshev distance in pixels in "
		".a; .b is reserved). Depth is propagated as a value through the same nearest-neighbor "
		"search as mask, not as a UV for a later indirect lookup - ties can only jump between "
		"depths already found near that pixel, bounded by local geometry. Depth-aware vs. "
		"'x-ray' occlusion is entirely a composite-shader decision built on this same output, not "
		"an Aura mode. Build via Aura.create() - the plain constructor leaves every camera/texture "
		"field empty."
	)
		.def(py::init<>(), "Constructs an empty Aura with no cameras/textures set; see Aura.create().")
		.def_readwrite(
			"selectionCamera", &osgx::Aura::selectionCamera,
			"Renders into originalMask + originalDepth; the first pass in the pipeline (PRE_RENDER "
			"order 1). Starts with no children - call selectionCamera.addChild(node) for whatever "
			"should cast the aura."
		)
		.def_readwrite(
			"dilateXCamera", &osgx::Aura::dilateXCamera,
			"First separable max-filter pass: reads originalMask + originalDepth, writes dilatedX "
			"(PRE_RENDER order 2)."
		)
		.def_readwrite(
			"dilateYCamera", &osgx::Aura::dilateYCamera,
			"Second separable max-filter pass: reads dilatedX, writes expanded (PRE_RENDER order 3)."
		)
		.def_readwrite(
			"originalMask", &osgx::Aura::originalMask,
			"The selection camera's raw mask output: nonzero where a child of selectionCamera was "
			"rendered, zero elsewhere."
		)
		.def_readwrite(
			"originalDepth", &osgx::Aura::originalDepth,
			"The selection camera's raw eye-space depth output, valid wherever originalMask is set."
		)
		.def_readwrite(
			"dilatedX", &osgx::Aura::dilatedX,
			"Intermediate result of the horizontal dilation pass, consumed by dilateYCamera."
		)
		.def_readwrite(
			"expanded", &osgx::Aura::expanded,
			"Final dilated result: mask in .r, propagated eye-space depth in .g, Chebyshev distance "
			"in pixels in .a (.b is reserved)."
		)
		.def_readwrite(
			"radius", &osgx::Aura::radius,
			"Dilation radius in output pixels; both dilation shaders clamp it to their fixed "
			"maximum of 64."
		)
		.def(
			"valid", &osgx::Aura::valid,
			"True if every camera and texture in the pipeline was successfully built."
		)
		.def_static(
			"create",
			&osgx::Aura::create,
			"width"_a,
			"height"_a,
			"radius"_a=8,
			"Builds a 3-pass screen-space silhouette expansion pipeline with an empty "
			"selectionCamera: a mask+depth capture pass, then two fullscreen passes separable-max-"
			"filter both into `expanded` (mask in .r, propagated eye-space depth in .g, Chebyshev "
			"distance in pixels in .a). Add the three cameras to the render graph in the listed "
			"order (PRE_RENDER order 1, 2, 3 - order 0 is left free for a preceding G-buffer pass), "
			"then call selectionCamera.addChild(node) for whatever should cast the aura. A node can "
			"be multi-parented normally (once under the visible scene, once under selectionCamera), "
			"and selectionCamera's children can be swapped at any time - e.g. to retarget one "
			"shared pipeline from a hover callback."
		)
	;
}

}
