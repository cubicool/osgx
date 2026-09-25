// vimrun! ./examples/osgx-culling

#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/Visitors.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/io_utils>
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>

#include <osgUtil/CullVisitor>

#include <osgDB/WriteFile>

#include <osgGA/GUIEventAdapter>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

class CullProbeCallback: public osg::DrawableCullCallback {
public:
	bool cull(osg::NodeVisitor* nv, osg::Drawable* drawable, osg::RenderInfo*) const override {
		auto* cv = nv ? nv->asCullVisitor() : nullptr;

		if(!cv || !drawable) return false;

		const bool culled = drawable->isCullingActive() && cv->isCulled(drawable->getBoundingBox());
		const auto frame = cv->getFrameStamp() ? cv->getFrameStamp()->getFrameNumber() : 0u;

		std::cout
			<< "frame " << frame
			<< " | " << drawable->getName()
			<< " | " << (culled ? "CULLED" : "VISIBLE")
			<< std::endl
		;

		return false;
	}
};

osg::ref_ptr<osg::Node> makeScene() {
	auto root = osgx::make_nref<osg::Group>("culling.root");
	root->setCullingActive(false);

	for(int y = -3; y <= 3; y++) {
		for(int x = -3; x <= 3; x++) {
			const std::string name = "sphere." + std::to_string(x) + "." + std::to_string(y);

			auto transform = osgx::make_nref<osg::MatrixTransform>(
				name + ".xf",
				osg::Matrix::translate(
					static_cast<float>(x) * 3.0f,
					0.0f,
					static_cast<float>(y) * 3.0f
				)
			);

			auto geode = osgx::make_nref<osg::Geode>(name + ".geode");
			auto drawable = osgx::make_nref<osg::ShapeDrawable>(
				name + ".drawable",
				new osg::Sphere(osg::Vec3(), 1.0f)
			);

			transform->setCullingActive(false);
			geode->setCullingActive(false);
			drawable->setCullCallback(new CullProbeCallback());

			geode->addDrawable(drawable);
			transform->addChild(geode);
			root->addChild(transform);
		}
	}

	return root;
}

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	// osgx::debug::FrameByFrameViewer viewer;
	osgViewer::Viewer viewer;

	auto root = makeScene();

	viewer.setSceneData(root);
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.addEventHandler(new osgx::VisitorEventHandler<osgx::DescribeSceneVisitor>(
		osgGA::GUIEventAdapter::KeySymbol::KEY_F9 // 'v'
	));

	auto r = viewer.run();

	// osgDB::writeNodeFile(*root, "osgx-culling.osgt");

	return r;
}
