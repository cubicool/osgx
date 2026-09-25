//vimrun! ./examples/osgx-eventhandler

#include "osgx/Callbacks.hpp"
#include "osgx/Core.hpp"
#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osgGA/GUIEventAdapter>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <cassert>

using namespace osgx::literals;

int main(int argc, char** argv) {
	auto lib = osgx::initialize();

	osgViewer::Viewer viewer;

	// Single key, 2-arg lambda (unchanged).
	viewer.addEventHandler(new osgx::LambdaKeyHandler('g', [](auto& ea, auto& aa) {
		OSG_WARN << "FIRST" << std::endl;

		return false;
	}));

	// Single key, 3-arg lambda - receives the matched key.
	viewer.addEventHandler(new osgx::LambdaKeyHandler('g', [](auto& ea, auto& aa, auto key) {
		OSG_WARN << "SECOND (key=" << key << ")" << std::endl;

		return true;
	}));

	// Multiple keys, 3-arg lambda - one handler for 'a', 'b', and arrow-up.
	viewer.addEventHandler(new osgx::LambdaKeyHandler(
		{'a', 'b', osgGA::GUIEventAdapter::KEY_Up},
		[](auto& ea, auto& aa, auto key) {
			OSG_WARN << "MULTI (key=" << key << ")" << std::endl;

			return true;
		}
	));

	return viewer.run();
}
