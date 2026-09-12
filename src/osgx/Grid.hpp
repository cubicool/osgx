#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/Array>
#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateAttribute>

OSGX_ENABLE_WARNINGS

namespace osg {
	class ShaderStorageBufferBinding;
}

namespace osgx {

// Registers the `#pragma osgx::grid INPUTS` and `#pragma osgx::grid GRID` shader-library blocks.
// INPUTS declares GridSettings' shader-storage data for a vertex stage; GRID declares the same
// inputs and exposes `osgx_GridColor(vec2 gridPos)` for a fragment stage.
void registerGridShaderLibs();

class GridSettings: public osg::StateAttribute {
public:
	static constexpr Type GRID_SETTINGS_TYPE = CAPABILITY;
	static constexpr unsigned int GRID_SETTINGS_MEMBER = 2;
	// Binding 4 follows Material (0), glTF joints (2), and LightSet (3). The FloatArray backing
	// store has the exact std430 layout declared by the shader-library blocks in Grid.cpp.
	static constexpr unsigned int GRID_SETTINGS_BINDING = 4;

	enum EdgeMode {
		EDGE_ASIS = 0,
		EDGE_HIDE = 1,
		EDGE_NUDGE = 2
	};

	enum LineMode {
		LINE_SCREEN_PIXELS = 0,
		LINE_GRID_UNITS = 1
	};

	GridSettings();
	GridSettings(const GridSettings& settings, const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);

	OSGX_META_StateAttribute(osgx, GridSettings, GRID_SETTINGS_TYPE)

	unsigned int getMember() const override { return GRID_SETTINGS_MEMBER; }
	int compare(const osg::StateAttribute& sa) const override;
	void apply(osg::State& state) const override;

	void setCanvasSize(const osg::Vec2& value);
	osg::Vec2 getCanvasSize() const;
	void setGridInterval(float value);
	float getGridInterval() const;
	void setGridIntervalStrong(float value);
	float getGridIntervalStrong() const;
	void setLineWidthPx(float value);
	float getLineWidthPx() const;
	void setLineWidth(float value);
	float getLineWidth() const;
	void setEdgeMode(EdgeMode value);
	EdgeMode getEdgeMode() const;
	void setLineMode(LineMode value);
	LineMode getLineMode() const;
	void setColorBg(const osg::Vec4& value);
	osg::Vec4 getColorBg() const;
	void setColorLine(const osg::Vec4& value);
	osg::Vec4 getColorLine() const;
	void setColorLineStrong(const osg::Vec4& value);
	osg::Vec4 getColorLineStrong() const;

protected:
	virtual ~GridSettings();

private:
	void _initBuffer();
	float* _data() const;

	osg::ref_ptr<osg::FloatArray> _buffer;
	osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
};

// ================================================================================================
// Grid
//
// Procedurally-generated, antialiased, view-aware grid lines on a single quad. It supports crisp
// constant screen-pixel lines for flat overlays and grid/world-unit line widths for perspective
// ground planes. Both modes are derivative-driven off a "grid space" coordinate (UV * canvasSize),
// so no MVP decomposition is needed; the screen-space derivative already bakes in model, view, AND
// projection.
//
// Typical usage:
//
//   // Fullscreen background grid, always drawn first (the osgSlug HUD/UI use case):
//   auto camera = osgx::Grid::createOrthoCamera();
//   viewer.setSceneData(camera);
//   viewer.getCamera()->setClearMask(GL_DEPTH_BUFFER_BIT); // don't let the main cam stomp it
//
//   // A real 3D ground plane instead:
//   auto grid = osgx::make_ref<osgx::Grid>(
//       osg::Vec3(-10,0,-10), osg::Vec3(20,0,0), osg::Vec3(0,0,20)
//   );
//   auto geode = osgx::make_ref<osg::Geode>();
//   geode->addDrawable(grid);
//
//   // Live updates, either way:
//   grid->setGridInterval(1.0f);
//   grid->setEdgeMode(osgx::Grid::EDGE_NUDGE);
//
// The GLSL vertex/fragment shader source (grid-space projection; osgx_GridLine()/
// osgx_PristineGridLine(),
// Ben Golus' "Pristine Grid" technique: https://bgolus.medium.com/the-best-darn-grid-shader-yet-727f9278b9d8)
// lives entirely in Grid.cpp - nothing outside this repo's own build has ever referenced those
// strings by name.
// ================================================================================================

class Grid: public osg::Geometry {
public:
	OSGX_META_Object(osgx, Grid)

	// Boundary-line handling for lines that fall exactly on the canvas edge (pos == 0 or
	// pos == canvasSize) - see EDGE_ASIS/EDGE_HIDE/EDGE_NUDGE in the fragment shader (Grid.cpp)
	// for the full rationale.
	using EdgeMode = GridSettings::EdgeMode;
	using LineMode = GridSettings::LineMode;

	static constexpr EdgeMode EDGE_ASIS = GridSettings::EDGE_ASIS;
	static constexpr EdgeMode EDGE_HIDE = GridSettings::EDGE_HIDE;
	static constexpr EdgeMode EDGE_NUDGE = GridSettings::EDGE_NUDGE;
	static constexpr LineMode LINE_SCREEN_PIXELS = GridSettings::LINE_SCREEN_PIXELS;
	static constexpr LineMode LINE_GRID_UNITS = GridSettings::LINE_GRID_UNITS;

	// Default: a fullscreen NDC quad (XY plane, z=0, -1..1) - the most common case, meant to
	// pair with orthoCamera()/createOrthoCamera() below.
	Grid() {
		_build(osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
	}

	// `corner`/`widthVec`/`heightVec` place the quad in whatever space it ends up added to the
	// scene graph in - NDC for a fullscreen overlay, or world-space for a real 3D ground plane.
	Grid(const osg::Vec3& corner, const osg::Vec3& widthVec, const osg::Vec3& heightVec) {
		_build(corner, widthVec, heightVec);
	}

	Grid(const Grid& g, const osg::CopyOp& co = osg::CopyOp::SHALLOW_COPY):
	osg::Geometry(g, co) {
		_bindSettings();
	}

	// --- Live settings updates --------------------------------------------------------------

	void setCanvasSize(const osg::Vec2& value) { _settings->setCanvasSize(value); }
	void setGridInterval(float value) { _settings->setGridInterval(value); }
	void setGridIntervalStrong(float value) { _settings->setGridIntervalStrong(value); }
	void setLineWidthPx(float value) { _settings->setLineWidthPx(value); }
	void setLineWidth(float value) { _settings->setLineWidth(value); }
	void setEdgeMode(EdgeMode value) { _settings->setEdgeMode(value); }
	void setLineMode(LineMode value) { _settings->setLineMode(value); }
	void setColorBg(const osg::Vec4& value) { _settings->setColorBg(value); }
	void setColorLine(const osg::Vec4& value) { _settings->setColorLine(value); }
	void setColorLineStrong(const osg::Vec4& value) { _settings->setColorLineStrong(value); }

	osg::Vec2 getCanvasSize() const { return _settings->getCanvasSize(); }
	float getGridInterval() const { return _settings->getGridInterval(); }
	float getGridIntervalStrong() const { return _settings->getGridIntervalStrong(); }
	float getLineWidthPx() const { return _settings->getLineWidthPx(); }
	float getLineWidth() const { return _settings->getLineWidth(); }
	EdgeMode getEdgeMode() const { return _settings->getEdgeMode(); }
	LineMode getLineMode() const { return _settings->getLineMode(); }
	osg::Vec4 getColorBg() const { return _settings->getColorBg(); }
	osg::Vec4 getColorLine() const { return _settings->getColorLine(); }
	osg::Vec4 getColorLineStrong() const { return _settings->getColorLineStrong(); }

	GridSettings* getSettings() { return _settings.get(); }
	const GridSettings* getSettings() const { return _settings.get(); }
	void setSettings(GridSettings* settings);

	// --- Fullscreen overlay wiring -----------------------------------------------------------

	// Wraps `this` in a Geode, and that Geode in an ABSOLUTE_RF, PRE_RENDER, ortho2D(-1,1,-1,1)
	// camera - the "always drawn first, in the background" fullscreen NDC setup this class
	// exists for. PRE_RENDER runs before the viewer's own NESTED_RENDER camera regardless of
	// scene graph position; the viewport is deliberately left unset so it inherits the window's
	// current viewport and tracks resizes for free.
	//
	// The caller still needs to drop the viewer's main camera to GL_DEPTH_BUFFER_BIT-only
	// clearing, or its default COLOR_BUFFER_BIT clear will stomp this camera's paint:
	//
	//   viewer.getCamera()->setClearMask(GL_DEPTH_BUFFER_BIT);
	osg::ref_ptr<osg::Camera> orthoCamera();

	// Skips the throwaway named instance: constructs a Grid and immediately wraps it.
	// Keep concrete overloads for the public API so language bindings do not need
	// their own factory implementations.
	static osg::ref_ptr<osg::Camera> createOrthoCamera() {
		return make_ref<Grid>()->orthoCamera();
	}

	static osg::ref_ptr<osg::Camera> createOrthoCamera(
		const osg::Vec3& corner,
		const osg::Vec3& widthVec,
		const osg::Vec3& heightVec
	) {
		return make_ref<Grid>(corner, widthVec, heightVec)->orthoCamera();
	}

	// Builds a UV sphere using this Grid's own shader and uniforms. The duplicated seam/pole
	// vertices make the equirectangular grid coordinates continuous everywhere except the
	// intentional longitude wrap, so the usual live style setters work unchanged.
	static osg::ref_ptr<Grid> createSphere(float radius=1.0f, unsigned int slices=48, unsigned int stacks=24);

private:
	void _build(const osg::Vec3& corner, const osg::Vec3& widthVec, const osg::Vec3& heightVec);
	void _buildSphere(float radius, unsigned int slices, unsigned int stacks);
	void _installState();

	void _bindSettings();

	osg::ref_ptr<GridSettings> _settings;
};

}
