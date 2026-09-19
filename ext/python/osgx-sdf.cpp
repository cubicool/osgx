#include "osgx-python.hpp"
#include "osgx/SDF.hpp"

namespace osgx_python {

// osgx::SDF - a pure GLSL shader-library catalog (no CPU-side functions, unlike Projection.hpp's
// CPU/GLSL twins - see SDF.hpp's own comment for why). Just the one registration call.
void bind_sdf(py::module_& m) {
	m.def(
		"registerSDFShaderLibs",
		&osgx::registerSDFShaderLibs,
		"Registers nine closed-form 2D signed-distance functions (osgx_SDF_Circle/Rect/Capsule/"
		"Arc/ArcBand/Rotate/Hexagon/Octagon/Star) under the '#pragma osgx::sdf SHAPES' "
		"shader-library key. Negative = inside. See SDF.hpp's own doc comment for full "
		"per-function signatures."
	);
}

}
