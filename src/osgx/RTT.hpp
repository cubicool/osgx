#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Texture>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <utility>
#include <vector>

namespace osgx {

// ================================================================================================
// RTT -- a single render-to-texture osg::Camera.
//
// Every hand-rolled RTT camera in this codebase (GBuffer, Picking, Grid, GGXPrefilter,
// CaptureCubeMap, LambertianBake, Shadow, Aura, gltf::PBRIBL) independently repeats the same four
// lines -- PRE_RENDER, FRAME_BUFFER_OBJECT, ABSOLUTE_RF, and a viewport matching the target
// texture's size -- and forgetting ABSOLUTE_RF is a SILENT failure: without it, the camera's own
// view matrix gets folded into the PARENT scene's bound computation (osg::Camera IS-A
// osg::Transform), producing a black/empty screenshot with no other symptom. RTT's constructor
// sets all four so this can no longer be forgotten at a call site.
//
// Deliberately IS-A osg::Camera (not a wrapping Group/struct): every osg::Camera setter
// (setViewMatrix, setProjectionMatrix, setClearColor, the inherited attach() overloads, ...)
// stays directly available with no forwarding, and `root->addChild(rtt)` just works. Multi-camera
// fan-out (cubemap faces, prefiltered mip chains) stays OUT of scope here on purpose -- RTT is the
// single-pass primitive those get built FROM (by something that owns N of them -- a future
// manager, or a bake function like GGXPrefilter's own that still hand-rolls its N cameras), not a
// thing that grows its own fan-out logic.
//
// No OSGX_META_Object / clone() support -- like osgx::CameraIntentHost's manipulator mixins
// (Manipulators.hpp), an RTT owns real GPU render-target state that clone() can't meaningfully
// duplicate; it is not expected to ever be cloned.
// ================================================================================================
class RTT: public osg::Camera {
public:
	// Declarative multi-attachment setup: a std::vector, not std::initializer_list, deliberately
	// matching osgx::Shader::HookList exactly (Shader.hpp) -- both bind to Python for free via
	// pybind11/stl.h's vector/pair casters (see ext/python/osgx-rtt.cpp), whereas
	// std::initializer_list has no pybind11 caster at all and would need a hand-written wrapper.
	// Costs nothing at C++ call sites: std::vector takes the identical `{{...}}` brace-init. `using
	// osg::Camera::attach` below keeps every other attach() overload (the level/face/mipmap variants
	// CaptureCubeMap/GGXPrefilter need) reachable too -- introducing this overload would otherwise
	// hide ALL base-class attach() overloads at this type, not just add to them.
	using AttachmentList = std::vector<std::pair<osg::Camera::BufferComponent, osg::Texture*>>;

	using osg::Camera::attach;

	// width/height: FBO resolution in pixels. Sets renderOrder=PRE_RENDER,
	// renderTargetImplementation=FRAME_BUFFER_OBJECT, referenceFrame=referenceFrame (ABSOLUTE_RF
	// by default -- see this class's own header comment), and viewport(0,0,width,height) -- every
	// other camera setting (clearMask/clearColor/view/projection/attach) is left to the caller,
	// same as a raw osg::Camera.
	//
	// referenceFrame=RELATIVE_RF is a real, deliberate second shape, not an escape hatch: a camera
	// that never sets its own view/projection and instead inherits whatever the cull traversal's
	// current matrices are at its position in the scene graph -- i.e. "render exactly what my
	// parent camera sees, into a texture instead of the backbuffer." osgx::Aura's selectionCamera
	// (Aura.cpp) is exactly this: it renders the selected node from the SAME viewpoint as the main
	// camera by simply never overriding view/projection, relying on ordinary RELATIVE_RF
	// composition during cull. Get this wrong the other way -- ABSOLUTE_RF on a camera that never
	// sets its own matrices -- and it renders from the origin looking down -Z, not from wherever
	// the caller expected.
	explicit RTT(
		int width,
		int height,
		osg::Transform::ReferenceFrame referenceFrame=osg::Transform::ABSOLUTE_RF
	);

	void attach(const AttachmentList& attachments);

	// The GGXPrefilter/SSAO-shaped fullscreen post-process pass: identity view/projection, a
	// full-viewport NDC quad already added as this camera's own child (osgx::FULLSCREEN_VERT +
	// fragmentShaderSrc), GL_DEPTH_TEST/GL_CULL_FACE forced off (OVERRIDE) on the camera's own
	// StateSet, and NO_CULLING/DO_NOT_COMPUTE_NEAR_FAR set on the camera itself. Every existing
	// hand-rolled version of this shape shares the same silent bug when depth test is left on: it
	// happens to work against the backbuffer (the main camera clears depth every frame) and then
	// discards every fragment once retargeted to an FBO with an unwritten implicit depth
	// attachment. Baking the fix in here makes it structural instead of a thing every call site
	// has to remember.
	static osg::ref_ptr<RTT> fullscreenQuad(int width, int height, const char* fragmentShaderSrc);

	// The fullscreenQuad() drawable, for a caller that wants to inspect/replace it (e.g. custom UV
	// layout). "Just works" for the common case: the quad is already this camera's own child.
	osg::Geometry* getQuad() const { return _quad.get(); }
	void setQuad(osg::Geometry* quad) { _quad = quad; }

protected:
	~RTT() override;

private:
	osg::ref_ptr<osg::Geometry> _quad;
};

}
