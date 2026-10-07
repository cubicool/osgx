// vimrun! ./examples/osgx-shaderclock

#include "osgx/Core.hpp"
#include "osgx/Debug.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Group>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;

	auto debugSupported = osgx::make_ref<osgx::debug::GraphicsOperation>();

	viewer.setRealizeOperation(debugSupported);
	viewer.realize();

	auto root = osgx::make_ref<osg::Group>();
	auto sphere = osgx::make_ref<osg::Geode>();

	sphere->addDrawable(new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(0.0, 0.0, 0.0), 1.0)));

	// A drawable that stalls the GPU for a fixed, targeted duration every frame it's drawn - the
	// motivating case is testing occlusion/culling strategies (Hi-Z, etc.) against a piece of
	// "geometry" with a known, repeatable GPU cost, instead of a real mesh whose cost depends on
	// fill rate, vertex count, driver state, and everything else that makes real-world GPU cost
	// hard to pin down for a controlled test.
	auto stall = osgx::make_ref<osgx::debug::ShaderClockDrawable>();

	stall->setName("stall");
	stall->setDuration(5.0);
	stall->setRecalibrationFrameInterval(20);

	// Wraps THIS drawable's own drawImplementation() in GL_TIMESTAMP queries, same as any other
	// drawable - ProfilerFinalCallback below prints the measured GPU time every 60 samples, which
	// should converge on ~5ms once ShaderClockDrawable's own DYNAMIC calibration (logged
	// separately, see its own console output) has run.
	stall->setDrawCallback(new osgx::debug::ProfilerCallback(stall->getName()));

	root->addChild(sphere);
	root->addChild(stall);

	viewer.setSceneData(root);

	osgx::debug::appendCameraDrawCallback(
		viewer.getCamera(),
		osgx::debug::CameraDrawCallbackSlot::FINAL_DRAW,
		new osgx::debug::ProfilerFinalCallback<>(60)
	);

	osgx::debug::pushGroup(0, __FUNCTION__);

	auto r = viewer.run();

	osgx::debug::popGroup();

	return r;
}
