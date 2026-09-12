#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Drawable>
#include <osg/Image>
#include <osg/RenderInfo>
#include <osg/Texture>
#include <osgDB/WriteFile>
#include <osgGA/GUIEventHandler>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <initializer_list>
#include <ranges>
#include <vector>

namespace osgx {

class LambdaKeyHandler: public osgGA::GUIEventHandler {
public:
	// TODO: OSG needs to make this less ghettofied, and EXPOSE THE TYPE (int) somehow!
	using Key = int;
	using Keys = std::vector<Key>;
	using Function = std::function<bool(
		const osgGA::GUIEventAdapter&,
		osgGA::GUIActionAdapter&,
		Key
	)>;

	template<typename Fn>
	LambdaKeyHandler(Key key, Fn fn): _keys{key}, _fn(_wrap(std::move(fn))) {}

	template<typename Fn>
	LambdaKeyHandler(std::initializer_list<Key> keys, Fn fn):
	_keys(keys), _fn(_wrap(std::move(fn))) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override {
		// if(ea.getHandled()) return false;

		if(ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN) {
			auto key = ea.getKey();

			if(std::ranges::find(_keys, key) != _keys.end()) {
				// OSG_NOTICE << "In LambdaKeyHandler(key=" << key << ")" << std::endl;

				return _fn(ea, aa, key);
			}
		}

		return false;
	}

private:
	Keys _keys;
	Function _fn;

	template<typename Fn>
	static Function _wrap(Fn fn) {
		static_assert(
			std::is_invocable_v<Fn, const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&> ||
			std::is_invocable_v<Fn, const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&, Key>,
			"LambdaKeyHandler: function must accept (ea, aa) or (ea, aa, key)"
		);

		if constexpr(std::is_invocable_v<Fn,
			const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&, Key>
		) {
			return fn;
		}

		else {
			return [fn = std::move(fn)](
				const osgGA::GUIEventAdapter& ea,
				osgGA::GUIActionAdapter& aa,
				Key
			) -> bool {
				return fn(ea, aa);
			};
		}
	}
};

class WriteTextureCallback: public osg::Camera::DrawCallback {
public:
	WriteTextureCallback(osg::Texture* t): _tex(t) {}

	virtual void operator()(osg::RenderInfo& ri) const {
		if(_write.exchange(false, std::memory_order_acq_rel)) {
			auto* state = ri.getState();

			_tex->apply(*state);

			auto img = make_ref<osg::Image>();

			img->readImageFromCurrentTexture(ri.getContextID(), false);

			osgDB::writeImageFile(*img, _name);

			OSG_NOTICE << "Writing: " << _name << std::endl;
		}
	}

	void write(const std::string& name) {
		_name = name;

		_write.store(true, std::memory_order_release);
	}

protected:
	osg::ref_ptr<osg::Texture> _tex;

	// NOTE: Using atomic flag with exchange() to safely trigger once from event thread.
	// I'm not going to lie, this bit was totally written by a LLM/AI. :) You MIGHT be able to
	// get away with JUST the `bool`, but it's bad form.
	mutable std::atomic<bool> _write{false};

	std::string _name;
};

template<typename Callback>
requires std::derived_from<Callback, osg::Referenced>
class CallbacksGroup: public Callback {
public:
	using Callbacks = std::vector<osg::ref_ptr<Callback>>;

	CallbacksGroup() = default;

	CallbacksGroup(std::initializer_list<Callback*> cbs) {
		for(auto* cb : cbs) add(cb);
	}

	void add(Callback* cb) {
		_callbacks.emplace_back(cb);
	}

	void remove(Callback* cb) {
		std::erase_if(_callbacks, [&](auto& p){ return p == cb; });
	}

	// Index-based accessors - kept alongside add()/remove() (not a replacement) so callers
	// with an identity-based mental model still have it, while anything wanting positional
	// list semantics (e.g. a Python SequenceProxy binding) has a real primitive to build on
	// instead of a linear identity-scan.
	std::size_t size() const {
		return _callbacks.size();
	}

	Callback* get(std::size_t i) const {
		return _callbacks[i].get();
	}

	void set(std::size_t i, Callback* cb) {
		_callbacks[i] = cb;
	}

	void removeAt(std::size_t i) {
		_callbacks.erase(_callbacks.begin() + static_cast<std::ptrdiff_t>(i));
	}

	void insert(std::size_t i, Callback* cb) {
		_callbacks.insert(_callbacks.begin() + static_cast<std::ptrdiff_t>(i), cb);
	}

protected:
	Callbacks _callbacks;
};

class CameraDrawCallbacksGroup: public CallbacksGroup<osg::Camera::DrawCallback> {
public:
	using CallbacksGroup<osg::Camera::DrawCallback>::CallbacksGroup;

	void operator()(osg::RenderInfo& ri) const override {
		for(const auto& cb : _callbacks) (*cb)(ri);
	}
};

class NodeCallbacksGroup: public CallbacksGroup<osg::NodeCallback> {
public:
	using CallbacksGroup<osg::NodeCallback>::CallbacksGroup;

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		for(const auto& cb : _callbacks) (*cb)(node, nv);
	}
};

class DrawableDrawCallbacksGroup: public CallbacksGroup<osg::Drawable::DrawCallback> {
public:
	using CallbacksGroup<osg::Drawable::DrawCallback>::CallbacksGroup;

	void drawImplementation(osg::RenderInfo& ri, const osg::Drawable* d) const override {
		for(const auto& cb : _callbacks) cb->drawImplementation(ri, d);
	}
};

template<typename Callback, typename Fn>
requires std::derived_from<Callback, osg::Referenced>
class LambdaCallbackBase: public Callback {
public:
	explicit LambdaCallbackBase(Fn fn): _fn(std::move(fn)) {}

protected:
	Fn _fn;
};

class CameraDrawLambdaCallback: public LambdaCallbackBase<
	osg::Camera::DrawCallback,
	std::function<void(osg::RenderInfo&)>
> {
public:
	using Base = LambdaCallbackBase<
		osg::Camera::DrawCallback,
		std::function<void(osg::RenderInfo&)>
	>;

	using Base::Base;

	void operator()(osg::RenderInfo& ri) const override {
		_fn(ri);
	}
};

class NodeLambdaCallback: public LambdaCallbackBase<
	osg::NodeCallback,
	std::function<void(osg::Node*, osg::NodeVisitor*)>
> {
public:
	using Base = LambdaCallbackBase<
		osg::NodeCallback,
		std::function<void(osg::Node*, osg::NodeVisitor*)>
	>;

	using Base::Base;

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		_fn(node, nv);
	}
};


}
