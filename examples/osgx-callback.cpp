// vimrun! ./examples/osgx-callback

#include "osgx/Core.hpp"
#include "osgx/Debug.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

auto createSphere(osgx::vec_t radius, osgx::vec_t pSize=1.0) {
	auto s = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(0.0, 0.0, 0.0), radius));

	/* s->getOrCreateStateSet()->setAttributeAndModes(
	// s->getOrCreateStateSet()->setAttribute(
		new osg::Point(pSize),
		// osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
		osg::StateAttribute::ON
	); */

	s->setName("SPHERE");

	return s;
}

auto createSphereAt(const osg::Vec3& pos, osgx::vec_t radius, osgx::vec_t pSize=1.0) {
	auto m = new osg::MatrixTransform(osg::Matrix::translate(pos));
	auto g = new osg::Geode();

	g->addDrawable(createSphere(radius, pSize));

	m->addChild(g);

	return m;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;

	auto debugSupported = osgx::make_ref<osgx::debug::GraphicsOperation>();

	viewer.setRealizeOperation(debugSupported);
	viewer.realize();

	auto root = osgx::make_ref<osg::Geode>();
	auto draw = createSphere(10.0);

	draw->setDrawCallback(new osgx::debug::ProfilerCallback());

	root->addDrawable(draw);

	viewer.setSceneData(root);

	osgx::debug::pushGroup(0, __FUNCTION__);

	auto r = viewer.run();

	osgx::debug::popGroup();

	// osgDB::writeNodeFile(*root, "tmp.osgt");

	return r;
}
