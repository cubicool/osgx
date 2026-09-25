// vimrun! ./examples/osgx-viewer

#include "osgx/Core.hpp"
#include "osgx/Debug.hpp"
#include "osgx/Library.hpp"
#include "osgx/Visitors.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>

#include <osgDB/ReadFile>

#include <osgGA/TrackballManipulator>

#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <vector>
#include <iostream>
#include <sstream>

auto createSphere(osgx::vec_t radius, osgx::vec_t pSize=1.0) {
	auto s = osgx::make_nref<osg::ShapeDrawable>(
		"SPHERE", new osg::Sphere(osg::Vec3(0.0, 0.0, 0.0), radius)
	);

	// s->getOrCreateStateSet()->setAttribute(new osg::Point(pSize));

	return s;
}

auto createSphereAt(const osg::Vec3& pos, osgx::vec_t radius, osgx::vec_t pSize=1.0) {
	auto m = osgx::make_ref<osg::MatrixTransform>(osg::Matrix::translate(pos));
	auto g = osgx::make_ref<osg::Geode>();

	g->addDrawable(createSphere(radius, pSize));

	m->addChild(g);

	return m;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgx::debug::FrameByFrameViewer viewer;

	auto debugSupported = osgx::make_ref<osgx::debug::GraphicsOperation>();

	viewer.setRealizeOperation(debugSupported);
	viewer.realize();

	auto root = osgx::make_ref<osg::Node>(nullptr);

	if(argc >= 2) {
		root = osgDB::readRefNodeFile(argv[1]);

		if(!root) return 0;
	}

	else {
		auto geode = osgx::make_ref<osg::Geode>();
		auto draw = createSphere(10.0);

		geode->addDrawable(draw);

		root = geode;
	}

	auto dsv = osgx::DescribeSceneVisitor();
	auto dv = osgx::debug::ProfilerVisitor();

	root->accept(dsv);
	root->accept(dv);

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	// viewer.addEventHandler(new osgViewer::StatsHandler());

	osgx::debug::appendCameraDrawCallback(
		viewer.getCamera(),
		osgx::debug::CameraDrawCallbackSlot::FINAL_DRAW,
		new osgx::debug::ProfilerFinalCallback()
	);

	osgx::debug::pushGroup(0, __FUNCTION__);

	auto r = viewer.run();

	osgx::debug::popGroup();

	// osgDB::writeNodeFile(*root, "tmp.osgt");

	return r;
}
