#include "osgx-python.hpp"
#include "osgx/Skinning.hpp"

namespace osgx_python {

// osgx/Skinning.hpp - the tangent/joint vertex attribute locations, bindMeshAttributes(), and the
// osgx_ApplySkin() hook sources. The GLSL declarations are the "osgx::skinning" catalog.
void bind_skinning(py::module_& m) {
	m.attr("TANGENT_ATTRIBUTE") = osgx::TANGENT_ATTRIBUTE;
	m.attr("JOINT_INDICES_ATTRIBUTE") = osgx::JOINT_INDICES_ATTRIBUTE;
	m.attr("JOINT_WEIGHTS_ATTRIBUTE") = osgx::JOINT_WEIGHTS_ATTRIBUTE;
	m.attr("TANGENT_ATTRIBUTE_NAME") = py::str(osgx::TANGENT_ATTRIBUTE_NAME);
	m.attr("JOINT_INDICES_ATTRIBUTE_NAME") = py::str(osgx::JOINT_INDICES_ATTRIBUTE_NAME);
	m.attr("JOINT_WEIGHTS_ATTRIBUTE_NAME") = py::str(osgx::JOINT_WEIGHTS_ATTRIBUTE_NAME);
	m.attr("SKINNING_HOOK_IDENTITY") = py::str(osgx::SKINNING_HOOK_IDENTITY);
	m.attr("SKINNING_HOOK_LINEAR_BLEND") = py::str(osgx::SKINNING_HOOK_LINEAR_BLEND);

	m.def(
		"bindMeshAttributes",
		&osgx::bindMeshAttributes,
		"program"_a,
		"Binds the tangent and joint index/weight attribute names (TANGENT_ATTRIBUTE_NAME, "
		"JOINT_INDICES_ATTRIBUTE_NAME, JOINT_WEIGHTS_ATTRIBUTE_NAME) to their locations "
		"(TANGENT_ATTRIBUTE, JOINT_INDICES_ATTRIBUTE, JOINT_WEIGHTS_ATTRIBUTE) on `program`."
	);

	py::class_<osgx::Skin, osg::ref_ptr<osgx::Skin>>(
		m,
		"Skin",
		"A skeleton's joint palette source: joint transforms in palette order and one inverse "
		"bind matrix per joint. Palette entry i is inverseBind[i] * jointWorld[i] * "
		"inverse(meshWorld), read from the joints' current transforms."
	)
		.def(
			py::init<
				const std::vector<osg::MatrixTransform*>&,
				const std::vector<osg::Matrixf>&
			>(),
			"joints"_a,
			"inverseBindMatrices"_a=std::vector<osg::Matrixf>(),
			"Joints are held weakly (the scene graph owns them). Missing inverse bind matrices "
			"are identity."
		)
		.def_property_readonly(
			"numJoints",
			&osgx::Skin::getNumJoints,
			"The number of joints (palette entries)."
		)
		.def(
			"attach",
			[](osgx::Skin& self, osg::Node* mesh) {
				self.attach(mesh);
			},
			"mesh"_a,
			"Gives `mesh` its own joint palette storage buffer, bound at the osgx::joints slot, "
			"and an update callback that recomputes it every update traversal. Geometry under "
			"`mesh` needs the joint index/weight attributes and a SKINNING_HOOK_LINEAR_BLEND "
			"Hook.Skinning shader."
		)
	;
}

}
