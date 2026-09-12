// vimrun! ./examples/osgx-hover
//
// Demonstrates onEnter / onLeave hover state with scene graph visual feedback.
//
// ASYNC (default): PickReadbackAsync (Texture2D + PBO glGetTexImage) updates _lastID
// atomically on the draw thread. PickHoverCallback polls lastID() on the update thread
// and fires onEnter/onLeave - safe for scene modifications at any OSG threading level.
//
// --sync: PickReadbackSync (osg::Image NodeCallback) for comparison. onEnter/onLeave
// still go through PickHoverCallback for consistency.
//
// Both modes use 1x1 FBO + continuous sub-frustum hover (PickCameraSync).
// Hovering a sphere scales it up (onEnter) and restores it on leave (onLeave).
//
// Scene: five spheres, pick IDs 1-5. ID 0 = background.

#include "osgx/Core.hpp"
#include "osgx/Picking.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/Uniform>

#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <unordered_map>

// ------------------------------------------------------------------------------------------------
// Scene
// ------------------------------------------------------------------------------------------------

struct ObjectInfo {
	osg::Vec3   pos;
	osg::Vec4   color;
	const char* name;
};

static const ObjectInfo OBJECTS[] = {
	{{ -8.0f, 0.0f, 0.0f }, { 1.0f, 0.2f, 0.2f, 1.0f }, "red"    },
	{{ -4.0f, 0.0f, 0.0f }, { 0.2f, 1.0f, 0.2f, 1.0f }, "green"  },
	{{  0.0f, 0.0f, 0.0f }, { 0.2f, 0.2f, 1.0f, 1.0f }, "blue"   },
	{{  4.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.2f, 1.0f }, "yellow" },
	{{  8.0f, 0.0f, 0.0f }, { 1.0f, 0.2f, 1.0f, 1.0f }, "magenta"},
};

struct PickEntry {
	osg::MatrixTransform* mt;
	osg::Matrix           base;
};

osg::ref_ptr<osg::Group> createScene(std::unordered_map<uint32_t, PickEntry>& objects) {
	auto root = osgx::make_nref<osg::Group>("scene");

	for(size_t i = 0; i < std::size(OBJECTS); i++) {
		const auto& o    = OBJECTS[i];
		auto        base = osg::Matrix::translate(o.pos);
		auto        mt   = osgx::make_ref<osg::MatrixTransform>(base);
		auto        geo  = osgx::make_ref<osg::Geode>();
		auto        sd   = osgx::make_ref<osg::ShapeDrawable>(new osg::Sphere(osg::Vec3(), 1.5f));

		mt->setDataVariance(osg::Object::DYNAMIC);
		sd->setColor(o.color);

		auto* uid = new osg::Uniform(osg::Uniform::UNSIGNED_INT, "pickID");

		uid->set(static_cast<unsigned int>(i + 1));

		geo->getOrCreateStateSet()->addUniform(uid);
		geo->addDrawable(sd);
		mt->addChild(geo);
		root->addChild(mt);

		objects[static_cast<uint32_t>(i + 1)] = { mt, base };
	}

	return root;
}

// ------------------------------------------------------------------------------------------------
// main
// ------------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	bool async = args.read("--async");

	osgViewer::Viewer viewer(args);

	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.realize();

	auto* vp = viewer.getCamera()->getViewport();
	int W = static_cast<int>(vp->width());
	int H = static_cast<int>(vp->height());

	OSG_NOTICE
		<< "Hover readback: "
		<< (async ? "ASYNC (Texture2D + PBO)" : "SYNC (osg::Image NodeCallback)")
		<< std::endl
		<< "onEnter/onLeave always fired from update thread via PickHoverCallback"
		<< std::endl;

	std::unordered_map<uint32_t, PickEntry> objects;

	auto scene   = createScene(objects);
	auto onEnter = [&objects](uint32_t id) {
		auto it = objects.find(id);
		if(it == objects.end()) return;
		OSG_NOTICE << "Enter -> ID " << id << " (" << OBJECTS[id - 1].name << ")" << std::endl;
		it->second.mt->setMatrix(osg::Matrix::scale(1.35, 1.35, 1.35) * it->second.base);
	};
	auto onLeave = [&objects](uint32_t id) {
		auto it = objects.find(id);
		if(it == objects.end()) return;
		OSG_NOTICE << "Leave -> ID " << id << " (" << OBJECTS[id - 1].name << ")" << std::endl;
		it->second.mt->setMatrix(it->second.base);
	};

	osg::ref_ptr<osg::Camera> pickCam;

	// ------------------------------------------------------------------------------------------------
	// ASYNC (--async): PickReadbackAsync updates _lastID on the draw thread.
	// PickHoverCallback fires onEnter/onLeave on the update thread - safe for scene mods.
	// Chain on pickCam: PickCameraSync -> PickHoverCallback
	// ------------------------------------------------------------------------------------------------
	if(async) {
		auto pickTex = osgx::make_ref<osg::Texture2D>();

		pickCam = osgx::makePickCamera(1, 1, pickTex);
		pickCam->addChild(scene);

		auto rb = osgx::make_ref<osgx::PickReadbackAsync>(
			pickTex, 1, 1, osgx::PickReadbackAsync::Mode::CONTINUOUS
		);

		pickCam->setPostDrawCallback(rb);

		rb->onEnter = onEnter;
		rb->onLeave = onLeave;

		auto* sync  = new osgx::PickCameraSync(viewer.getCamera(), true, W, H, rb);
		auto* hover = new osgx::PickHoverCallback(rb);

		sync->setNestedCallback(hover);
		pickCam->setUpdateCallback(sync);

		viewer.addEventHandler(new osgx::PickHandler(rb, true));
	}

	// ------------------------------------------------------------------------------------------------
	// SYNC (default): PickReadbackSync samples image->data() on the update thread.
	// PickHoverCallback fires onEnter/onLeave on the same update thread pass.
	// Chain on pickCam: PickCameraSync -> PickHoverCallback -> PickReadbackSync
	// ------------------------------------------------------------------------------------------------
	else {
		auto pickImage = osgx::make_ref<osg::Image>();

		pickImage->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);

		// makePickCamera() zeroes the image itself once attached - see its comment for why.
		pickCam = osgx::makePickCamera(1, 1, pickImage);
		pickCam->addChild(scene);

		auto rb = osgx::make_ref<osgx::PickReadbackSync>(
			1, osgx::spiralPick, pickImage, W, H,
			osgx::PickReadbackSync::Mode::CONTINUOUS
		);

		rb->onEnter = onEnter;
		rb->onLeave = onLeave;

		auto* sync  = new osgx::PickCameraSync(viewer.getCamera(), true, W, H, rb);
		auto* hover = new osgx::PickHoverCallback(rb);

		hover->setNestedCallback(rb);
		sync->setNestedCallback(hover);
		pickCam->setUpdateCallback(sync);

		viewer.addEventHandler(new osgx::PickHandler(rb, true));
	}

	auto root = osgx::make_nref<osg::Group>("root");
	root->addChild(pickCam);
	root->addChild(scene);

	viewer.setSceneData(root);

	return viewer.run();
}
