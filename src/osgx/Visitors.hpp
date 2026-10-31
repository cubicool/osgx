#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Drawable>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Notify>
#include <osg/ShapeDrawable>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <cstdint>
#include <format>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

namespace osgx {

// ------------------------------------------------------------------------------------------------
// TODO: Can this be used with VisitorEventHandler?
template<typename Node=osg::Node>
class LambdaVisitor: public osg::NodeVisitor {
public:
	using Function = std::function<void(Node&)>;

	LambdaVisitor(Function function):
	osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
	_function(function) {}

	virtual void apply(Node& n) override {
		_function(n);

		traverse(n);
	}

	virtual void operator()(osg::Node* node) {
		node->accept(*this);
	}

protected:
	Function _function;
};

class IndexedVisitor: public osg::NodeVisitor {
public:
	IndexedVisitor():
	osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN) {}

	virtual void itraverse(osg::Node& n) {
		_i++;

		traverse(n);

		_i--;
	}

protected:
	inline size_t _index() const {
		return _i;
	}

private:
	size_t _i = 0;
};

// TODO: Call setUserValue("path", ...), if requested.
// class NameVisitor: public osg::NodeVisitor {
class NameVisitor: public IndexedVisitor {
public:
	// TODO: Add a "format" method, where the user can specify: "%c%i".
	// TODO: Considering adding a "%%" format, which uses a lambda for formatting.
	enum Options {
		CLASS = 1u,
		PATH = 1u << 1,
		FORCE = 1u << 2
	};

	NameVisitor(unsigned int options=Options::CLASS):
	// osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
	_options(options) {}

	// TODO: See below.
	virtual void apply(osg::Node& n) override {
		_setName(n);

		itraverse(n);
	}

protected:
	// TODO: Is this REALLY the best way to handle this? If a subclass wants to kick off the apply()
	// behavior FIRST (rather than before), I can't think of anything else...
	void _setName(osg::Node& n) {
		if(!n.getName().size()) {
			std::ostringstream name;

			const auto& cn = n.className();

			if(_options & Options::CLASS) name << "$" << cn << "_" << _cidmap[cn];

			n.setName(name.str());

			_cidmap[cn]++;
		}
	}

private:
	unsigned int _options;

	using ClassIDMap = std::map<std::string, size_t>;

	ClassIDMap _cidmap;
};

// Prints the scene tree to stdout, one line per node and drawable, indented by depth: the name (or
// "ClassName@0x<address>" when unnamed), then "(library::ClassName)". Does not modify the scene.
class DescribeSceneVisitor: public IndexedVisitor {
public:
	DescribeSceneVisitor(const std::string& indent="   "):
	_indent(indent) {}

	virtual void apply(osg::Node& n) override {
		std::cout << _getIndent() << _describe(n) << std::endl;

		itraverse(n);
	}

	virtual void apply(osg::Drawable& d) override {
		std::cout << _getIndent() << "+ " << _describe(d) << std::endl;

		itraverse(d);
	}

protected:
	std::string _getIndent() const {
		std::string indent;

		for(auto i = 0_z; i < _index(); i++) indent += _indent;

		return indent;
	}

	static std::string _describe(const osg::Object& o) {
		const auto label = o.getName().empty()
			? std::format("{}@{:#x}", o.className(), reinterpret_cast<std::uintptr_t>(&o))
			: o.getName()
		;

		return std::format("{} ({}::{})", label, o.libraryName(), o.className());
	}

private:
	std::string _indent;
};

namespace scene {
	// TODO: More arguments.
	inline auto sphereAt(const std::string& name, const osg::Vec3& pos, vec_t radius) {
		auto m = new osg::MatrixTransform(osg::Matrix::translate(pos));
		auto g = new osg::Geode();
		auto t = new osg::TessellationHints();

		t->setDetailRatio(0.25);

		auto s = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3(0.0, 0.0, 0.0), radius), t);

		s->setName(name + ".spr");

		g->setName(name + ".geo");
		g->addDrawable(s);
		// g->setCullCallback(new DrawableCullCallback());

		m->setName(name);
		m->addChild(g);
		// m->setCullCallback(new CullCallback());

		return m;
	}
}

// Runs the passed-in Visitor on the scene when a specified key is pressed.
//
// TODO: More control over how and with what the visitor is called.
// TODO: Figure out a better way than just hard-coding the osgViewer::Viewer assumption.
template<typename Visitor, typename Viewer=osgViewer::Viewer>
class VisitorEventHandler: public osgGA::GUIEventHandler {
public:
	// TODO: OSG needs to make this less ghettofied.
	using key_t = int;

	VisitorEventHandler(key_t key, Visitor* visitor=nullptr):
	_key(key),
	_visitor(visitor) {}

	virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override {
		// TODO: Is this really necessary?
		if(ea.getHandled()) return false;

		/* auto* view = dynamic_cast<osgViewer::View*>(&aa);

		if(!view) return false;

		auto* viewer = view->getViewerBase(); */

		// auto* viewer = dynamic_cast<osgViewer::Viewer*>(&aa);
		auto* viewer = dynamic_cast<Viewer*>(&aa);

		if(!viewer) return false;

		if(
			ea.getEventType() == osgGA::GUIEventAdapter::KEYUP &&
			ea.getKey() == _key
		) {
			auto* root = viewer->getSceneData();

			if(_visitor) root->accept(*_visitor);

			else {
				auto v = Visitor();

				root->accept(v);
			}
		}

		return false;
	}

protected:
	key_t _key;

	osg::ref_ptr<Visitor> _visitor;
};

class FilterNotifyHandler: public osg::NotifyHandler {
public:
	template<typename... Args>
	FilterNotifyHandler(Args&&... args) {
		(addFilter(std::forward<Args>(args)), ...);

		// Define patterns to filter/exclude!
		_filter.emplace_back("^draw()");
		_filter.emplace_back("^cull()");
		_filter.emplace_back("^end draw()");
		_filter.emplace_back("^end cull()");
		_filter.emplace_back(R"(^BufferObject::releaseGLObjects\(0)");
		_filter.emplace_back(R"(^BufferData::releaseGLObjects\(0)");
	}

	void notify(osg::NotifySeverity severity, const char* message) override {
		std::string msg(message);

		for(const auto& f : _filter) {
			if(std::regex_search(msg, f)) return;
		}

		std::cerr << msg;
	}

	void addFilter(const std::string& patternStr) {
		try {
			_filter.emplace_back(patternStr);
		}

		catch(const std::regex_error& e) {
			std::cerr
				<< "Invalid regex pattern: " << patternStr
				<< " (" << e.what() << ")" << std::endl
			;
		}
	}

private:
	std::vector<std::regex> _filter;
};

}
