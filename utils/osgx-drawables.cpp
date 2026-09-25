// vimrun! ./utils/osgx-drawables

#include "osgx/Core.hpp"
#include "osgx/Debug.hpp"
#include "osgx/Library.hpp"
#include "osgx/Visitors.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geometry>
#include <osg/Group>
#include <osgDB/ReadFile>

OSGX_ENABLE_WARNINGS

auto createSceneA() {
	auto root = osgx::make_nref<osg::Group>("SceneA");
	auto g0 = osgx::make_nref<osg::Group>("group0");
	auto g1 = osgx::make_nref<osg::Group>("group1");

	g0->addChild(osgx::make_nref<osg::Geode>("geode0"));
	g0->addChild(osgx::make_nref<osg::Geode>("geode1"));
	g0->addChild(osgx::make_nref<osg::Geode>("geode2"));

	g1->addChild(osgx::make_nref<osg::Geode>("geode0"));
	g1->addChild(osgx::make_nref<osg::Geode>("geode1"));

	root->addChild(g0);
	root->addChild(g1);

	return root;
}

auto createSceneB() {
	auto root = new osg::Group();
	auto g0 = new osg::Group();
	auto g1 = new osg::Group();

	g0->addChild(new osg::Geode());
	g0->addChild(new osg::Geode());
	g0->addChild(new osg::Geode());

	g1->addChild(new osg::Geode());
	g1->addChild(new osg::Geode());

	auto geode = new osg::Geode();
	auto geom = new osg::Geometry();

	geode->addDrawable(geom);

	root->addChild(g0);
	root->addChild(g1);
	root->addChild(geode);

	// dynamic_cast<osg::Group*>(root->getChild(0))->addChild(new osg::Node());

	return root;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	auto root = osgx::make_ref<osg::Node>(nullptr);

	if(argc >= 2) root = osgDB::readNodeFile(argv[1]);

	else {
		auto group = osgx::make_nref<osg::Group>("root");

		group->addChild(createSceneA());
		group->addChild(createSceneB());

		root = group;
	}

	auto dsv = osgx::DescribeSceneVisitor();
	auto dv = osgx::debug::ProfilerVisitor();

	root->accept(dsv);
	root->accept(dv);

	// TODO: Make this optional (--view, --no-view, etc).
	osgViewer::Viewer viewer;

	viewer.setSceneData(root);

	auto r = viewer.run();

	// root->accept(dsv);

	return r;
}

#if 0
int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	auto plod = new osg::PagedLOD();

	plod->setFileName(0, "cow.osg");
	plod->setFileName(1, "cessna.osg");
	plod->setRange(0, 0.0, 5.0);
	plod->setRange(1, 5.0, FLT_MAX);

	osgViewer::Viewer v;

	v.setSceneData(plod);

	return v.run();
}
#endif

#if 0
using lod_min_t = osg::LOD::MinMaxPair::first_type;
using lod_max_t = osg::LOD::MinMaxPair::second_type;

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	auto lod = new osg::LOD();

	lod->addChild(osgDB::readNodeFile("cow.osg"), 0.0, 150.0);
	lod->addChild(osgDB::readNodeFile("cessna.osg"), 150.0, std::numeric_limits<float>::max());

	osgViewer::Viewer v;

	v.setCameraManipulator(new osgGA::TrackballManipulator());
	v.getCameraManipulator()->setHomePosition(
		osg::Vec3(0.0, 20.0, 0.0),
		osg::Vec3(0.0, 0.0, 0.0),
		osg::Vec3(0.0, 0.0, 1.0)
	);

    v.home();
	v.setSceneData(lod);

	return v.run();
}
#endif
