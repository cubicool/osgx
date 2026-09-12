#include "osgx-python.hpp"
#include "osgx/Callbacks.hpp"

namespace osgx_python {

namespace detail {
	struct CameraDrawCallbacksTag;
	struct NodeCallbacksGroupTag;
	struct DrawableDrawCallbacksTag;
}

}

// osgx::CallbacksGroup<Callback>'s add()/remove() are identity-based, but the underlying
// storage (osgx/Callbacks.hpp's _callbacks) is a real std::vector - size()/get()/set()/
// removeAt()/insert() (added alongside this binding) expose that positionally, which is what
// SequenceTraits actually needs. This is the SAME shape as Group.children (OpenSceneGraph.py's
// pyosg/osg/Group.hpp): a pyx::SequenceProxy handles Python list semantics AND, critically,
// per-element Python identity/lifetime via SlotCache - NOT py::dynamic_attr() (deliberately
// avoided project-wide, it's slow) and NOT py::keep_alive (can't reach elements added later
// through a method call, only fixed constructor argument positions).
template<>
struct pyx::SequenceTraits<osgx::CameraDrawCallbacksGroup, osgx_python::detail::CameraDrawCallbacksTag> {
	using element_type = osg::Camera::DrawCallback;
	using value_type = element_type*;

	static value_type from_python(py::handle h) { return h.cast<value_type>(); }
	static size_t size(const osgx::CameraDrawCallbacksGroup* g) { return g->size(); }
	static element_type* get(osgx::CameraDrawCallbacksGroup* g, size_t i) { return g->get(i); }
	static void set(osgx::CameraDrawCallbacksGroup* g, size_t i, value_type v) { g->set(i, v); }
	static void del(osgx::CameraDrawCallbacksGroup* g, size_t i) { g->removeAt(i); }
	static void append(osgx::CameraDrawCallbacksGroup* g, value_type v) { g->add(v); }
	static void insert(osgx::CameraDrawCallbacksGroup* g, size_t i, value_type v) { g->insert(i, v); }
};

template<>
struct pyx::SequenceTraits<osgx::NodeCallbacksGroup, osgx_python::detail::NodeCallbacksGroupTag> {
	using element_type = osg::NodeCallback;
	using value_type = element_type*;

	static value_type from_python(py::handle h) { return h.cast<value_type>(); }
	static size_t size(const osgx::NodeCallbacksGroup* g) { return g->size(); }
	static element_type* get(osgx::NodeCallbacksGroup* g, size_t i) { return g->get(i); }
	static void set(osgx::NodeCallbacksGroup* g, size_t i, value_type v) { g->set(i, v); }
	static void del(osgx::NodeCallbacksGroup* g, size_t i) { g->removeAt(i); }
	static void append(osgx::NodeCallbacksGroup* g, value_type v) { g->add(v); }
	static void insert(osgx::NodeCallbacksGroup* g, size_t i, value_type v) { g->insert(i, v); }
};

template<>
struct pyx::SequenceTraits<osgx::DrawableDrawCallbacksGroup, osgx_python::detail::DrawableDrawCallbacksTag> {
	using element_type = osg::Drawable::DrawCallback;
	using value_type = element_type*;

	static value_type from_python(py::handle h) { return h.cast<value_type>(); }
	static size_t size(const osgx::DrawableDrawCallbacksGroup* g) { return g->size(); }
	static element_type* get(osgx::DrawableDrawCallbacksGroup* g, size_t i) { return g->get(i); }
	static void set(osgx::DrawableDrawCallbacksGroup* g, size_t i, value_type v) { g->set(i, v); }
	static void del(osgx::DrawableDrawCallbacksGroup* g, size_t i) { g->removeAt(i); }
	static void append(osgx::DrawableDrawCallbacksGroup* g, value_type v) { g->add(v); }
	static void insert(osgx::DrawableDrawCallbacksGroup* g, size_t i, value_type v) { g->insert(i, v); }
};

namespace osgx_python {

namespace detail {
	using CameraDrawCallbacksProxy = pyx::SequenceProxy<osgx::CameraDrawCallbacksGroup, CameraDrawCallbacksTag>;
	using CameraDrawCallbacksStorage =
		pyx::ProxyStorageOSG<osgx::CameraDrawCallbacksGroup, CameraDrawCallbacksProxy>;

	using NodeCallbacksGroupProxy = pyx::SequenceProxy<osgx::NodeCallbacksGroup, NodeCallbacksGroupTag>;
	using NodeCallbacksGroupStorage = pyx::ProxyStorageOSG<osgx::NodeCallbacksGroup, NodeCallbacksGroupProxy>;

	using DrawableDrawCallbacksProxy =
		pyx::SequenceProxy<osgx::DrawableDrawCallbacksGroup, DrawableDrawCallbacksTag>;
	using DrawableDrawCallbacksStorage =
		pyx::ProxyStorageOSG<osgx::DrawableDrawCallbacksGroup, DrawableDrawCallbacksProxy>;
}

// osgx::CallbacksGroup<Callback> and its three instantiations (osgx/Callbacks.hpp) - the usual
// way to run several callbacks of the same kind off one slot (updateCallback, postDrawCallback,
// etc.) without threading a Callback::setNestedCallback() chain by hand. Bound directly at the
// top-level `osgx` namespace (not a submodule), matching FlyToCallback/ShakeCallback in
// osgx-core.cpp - these are general infra, not thematically their own module.
void bind_callbacks(py::module_& m) {
	auto cameraGroup = py::class_<
		osgx::CameraDrawCallbacksGroup,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgx::CameraDrawCallbacksGroup>
	>(
		m,
		"CameraDrawCallbacksGroup",
		"Runs several osg.Camera.DrawCallback objects off one draw-callback slot, in list order, "
		"side by side - NOT chained via setNestedCallback(). Populate through .callbacks (a real "
		"Python list: indexing, len(), append(), insert(), del) or the add()/remove() aliases."
	);

	pyx::bind_proxy_property<
		detail::CameraDrawCallbacksProxy, osgx::CameraDrawCallbacksGroup, detail::CameraDrawCallbacksStorage
	>(
		cameraGroup, "_Callbacks", "callbacks",
		"The group's member callbacks as a real Python list, in run order."
	);

	cameraGroup
		.def(py::init<>(), "Creates an empty group.")
		// Populated through the SAME proxy .callbacks exposes (via extend()), not group->add()
		// directly - extend()/append() are what actually cache each element's Python identity
		// (SlotCache), so a callback passed inline in the list literal with no other Python
		// reference stays alive exactly as long as it's a member, same guarantee append()
		// gives when called after construction.
		.def(py::init([](py::sequence cbs) {
			auto* group = new osgx::CameraDrawCallbacksGroup();

			detail::CameraDrawCallbacksStorage::get(*group)
				->template proxy<detail::CameraDrawCallbacksProxy>()
				.extend(cbs);

			return group;
		}), "callbacks"_a, "Creates a group pre-populated with `callbacks`, run in list order.")
		// Thin aliases matching osgx::CallbacksGroup's own add()/remove() vocabulary - both
		// delegate to the SAME retaining proxy .callbacks exposes, not the raw C++ methods.
		.def("add", [](osgx::CameraDrawCallbacksGroup& g, py::object cb) {
			detail::CameraDrawCallbacksStorage::get(g)->template proxy<detail::CameraDrawCallbacksProxy>().append(cb);
		}, "callback"_a, "Appends `callback` to the group (alias for .callbacks.append()).")
		.def("remove", [](osgx::CameraDrawCallbacksGroup& g, py::object cb) {
			detail::CameraDrawCallbacksStorage::get(g)->template proxy<detail::CameraDrawCallbacksProxy>().remove(cb);
		}, "callback"_a, "Removes `callback` from the group (alias for .callbacks.remove()).")
	;

	auto nodeGroup = py::class_<
		osgx::NodeCallbacksGroup,
		osg::NodeCallback,
		osg::ref_ptr<osgx::NodeCallbacksGroup>
	>(
		m,
		"NodeCallbacksGroup",
		"Runs several osg.NodeCallback objects off one update/event/cull callback slot, in list "
		"order, side by side - NOT chained via setNestedCallback(). Populate through .callbacks "
		"(a real Python list: indexing, len(), append(), insert(), del) or the add()/remove() aliases."
	);

	pyx::bind_proxy_property<
		detail::NodeCallbacksGroupProxy, osgx::NodeCallbacksGroup, detail::NodeCallbacksGroupStorage
	>(
		nodeGroup, "_Callbacks", "callbacks",
		"The group's member callbacks as a real Python list, in run order."
	);

	nodeGroup
		.def(py::init<>(), "Creates an empty group.")
		.def(py::init([](py::sequence cbs) {
			auto* group = new osgx::NodeCallbacksGroup();

			detail::NodeCallbacksGroupStorage::get(*group)
				->template proxy<detail::NodeCallbacksGroupProxy>()
				.extend(cbs);

			return group;
		}), "callbacks"_a, "Creates a group pre-populated with `callbacks`, run in list order.")
		.def("add", [](osgx::NodeCallbacksGroup& g, py::object cb) {
			detail::NodeCallbacksGroupStorage::get(g)->template proxy<detail::NodeCallbacksGroupProxy>().append(cb);
		}, "callback"_a, "Appends `callback` to the group (alias for .callbacks.append()).")
		.def("remove", [](osgx::NodeCallbacksGroup& g, py::object cb) {
			detail::NodeCallbacksGroupStorage::get(g)->template proxy<detail::NodeCallbacksGroupProxy>().remove(cb);
		}, "callback"_a, "Removes `callback` from the group (alias for .callbacks.remove()).")
	;

	auto drawableGroup = py::class_<
		osgx::DrawableDrawCallbacksGroup,
		osg::Drawable::DrawCallback,
		osg::ref_ptr<osgx::DrawableDrawCallbacksGroup>
	>(
		m,
		"DrawableDrawCallbacksGroup",
		"Runs several osg.Drawable.DrawCallback objects off one draw-callback slot, in list "
		"order, side by side - NOT chained via setNestedCallback(). Populate through .callbacks "
		"(a real Python list: indexing, len(), append(), insert(), del) or the add()/remove() aliases."
	);

	pyx::bind_proxy_property<
		detail::DrawableDrawCallbacksProxy, osgx::DrawableDrawCallbacksGroup, detail::DrawableDrawCallbacksStorage
	>(
		drawableGroup, "_Callbacks", "callbacks",
		"The group's member callbacks as a real Python list, in run order."
	);

	drawableGroup
		.def(py::init<>(), "Creates an empty group.")
		.def(py::init([](py::sequence cbs) {
			auto* group = new osgx::DrawableDrawCallbacksGroup();

			detail::DrawableDrawCallbacksStorage::get(*group)
				->template proxy<detail::DrawableDrawCallbacksProxy>()
				.extend(cbs);

			return group;
		}), "callbacks"_a, "Creates a group pre-populated with `callbacks`, run in list order.")
		.def("add", [](osgx::DrawableDrawCallbacksGroup& g, py::object cb) {
			detail::DrawableDrawCallbacksStorage::get(g)->template proxy<detail::DrawableDrawCallbacksProxy>().append(cb);
		}, "callback"_a, "Appends `callback` to the group (alias for .callbacks.append()).")
		.def("remove", [](osgx::DrawableDrawCallbacksGroup& g, py::object cb) {
			detail::DrawableDrawCallbacksStorage::get(g)->template proxy<detail::DrawableDrawCallbacksProxy>().remove(cb);
		}, "callback"_a, "Removes `callback` from the group (alias for .callbacks.remove()).")
	;

	// No keep_alive on the texture argument: WriteTextureCallback holds a real
	// osg::ref_ptr<osg::Texture>, so the C++ object outlives any Python wrapper on its own, and
	// py::cast() returns the same PyObject* for the same pointer anyway. Nothing here needs a
	// SlotCache/proxy either - it's one fixed constructor argument, not an add()-retained
	// container.
	py::class_<
		osgx::WriteTextureCallback,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgx::WriteTextureCallback>
	>(
		m,
		"WriteTextureCallback",
		"A Camera.DrawCallback that asynchronously writes its texture to disk when requested via "
		"write(); does nothing on frames where write() hasn't been called since the last run."
	)
		.def(
			py::init<osg::Texture*>(), "texture"_a,
			"Wraps `texture`; call .write(filename) to request one readback-and-save the next "
			"time this callback runs."
		)
		.def(
			"write", &osgx::WriteTextureCallback::write, "filename"_a,
			"Request that this callback's texture be written to `filename` the NEXT time the "
			"callback runs. Does not write immediately and does not block - the readback needs a "
			"live GL context, so it happens inside the draw traversal rather than here.\n\n"
			"Install it on the camera that RENDERS the texture, normally as that camera's "
			"postDrawCallback, so the readback sees what the camera just drew (use "
			"CameraDrawCallbacksGroup if the slot is already occupied).\n\n"
			"Chiefly a diagnostic. It settles questions about a render target that are genuinely "
			"hard to judge on screen - 'is this actually flat, or just low-contrast?' becomes "
			"unambiguous once you count distinct colors in the dump, where a uniform fill stands "
			"out instantly. Caveat: the readback can be GPU-async-stale; if a result seems to lag "
			"a frame, prefer live shader hot-swap over trusting the pixels."
		)
	;
}

}
