// vimrun! ./examples/osgx-notifyfilter

#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/Visitors.hpp"

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

	s->setName("ShapeDrawable");

	return s;
}

auto createSphereAt(const osg::Vec3& pos, osgx::vec_t radius, osgx::vec_t pSize=1.0) {
	auto m = new osg::MatrixTransform(osg::Matrix::translate(pos));
	auto g = new osg::Geode();

	g->addDrawable(createSphere(radius, pSize));
	g->setName("Geode");

	m->addChild(g);
	m->setName("MatrixTransform");

	return m;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osg::setNotifyLevel(osg::DEBUG_FP);
	osg::setNotifyHandler(new osgx::FilterNotifyHandler(
		"^Done destructing osg::View",
		R"(^DatabasePager::RequestQueue::~RequestQueue\(\) Destructing queue.)",
		R"(^\s+Waiting for OperationThread to cancel 0x.*)"
	));

	osgViewer::Viewer viewer;

	viewer.realize();

	auto root = osgx::make_ref<osg::Geode>();
	auto draw = createSphere(10.0);

	root->addDrawable(draw);

	viewer.setSceneData(root);

	return viewer.run();
}
