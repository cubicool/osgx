import osgx

from OpenSceneGraph import *
from OpenSceneGraph.GL import *

def make_tex(w=4, h=4):
	t = osg.Texture2D()
	t.size = (w, h)
	t.internalFormat = GL_RGBA

	return t

# osgx.RTT.attach() (ext/python/osgx-rtt.cpp) is a py::args binding backed by
# pyx::unpack_list_or_args<T>() (pybind11x.hpp). At the pybind11 boundary there are really only
# two shapes it has to tell apart -- ONE argument that's itself a sequence of pairs, or SEVERAL
# arguments each individually castable to a pair -- and every call style below reduces to one of
# those two, including the ones that only differ at the Python syntax level (a literal list vs. a
# variable holding one, or `*pairs` splatting vs. writing the pairs out by hand). Each test proves
# the texture was actually forwarded through to a real osg::Camera::attach() call (not silently
# dropped) by checking the camera's internal BufferAttachmentMap picks up exactly one new
# C++-side ref_ptr per texture -- attach() only ever borrows its arguments, so the Python-side
# refcount is untouched either way.
def _assert_newly_attached(tex, cpp_before):
	assert tex.referenceCount.cpp == cpp_before + 1

def test_attach_accepts_a_list_of_pairs():
	cam = osgx.RTT(4, 4)
	a, b = make_tex(), make_tex()
	a_cpp, b_cpp = a.referenceCount.cpp, b.referenceCount.cpp

	cam.attach([(osg.Camera.COLOR_BUFFER0, a), (osg.Camera.COLOR_BUFFER1, b)])

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)

def test_attach_accepts_a_dict():
	# The preferred form -- see osgx-rtt.cpp's attach() docstring: a dict rules out attaching two
	# textures to the same component by construction, the same reasoning osgSlug.Text.setHooks()
	# uses for its own {Hook: shader} argument.
	cam = osgx.RTT(4, 4)
	a, b = make_tex(), make_tex()
	a_cpp, b_cpp = a.referenceCount.cpp, b.referenceCount.cpp

	cam.attach({osg.Camera.COLOR_BUFFER0: a, osg.Camera.COLOR_BUFFER1: b})

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)

def test_attach_accepts_a_single_tuple_of_pairs():
	cam = osgx.RTT(4, 4)
	a, b = make_tex(), make_tex()
	a_cpp, b_cpp = a.referenceCount.cpp, b.referenceCount.cpp

	cam.attach(((osg.Camera.COLOR_BUFFER0, a), (osg.Camera.COLOR_BUFFER1, b)))

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)

def test_attach_accepts_pairs_as_separate_positional_arguments():
	cam = osgx.RTT(4, 4)
	a, b, c = make_tex(), make_tex(), make_tex()
	a_cpp, b_cpp, c_cpp = a.referenceCount.cpp, b.referenceCount.cpp, c.referenceCount.cpp

	cam.attach(
		(osg.Camera.COLOR_BUFFER0, a),
		(osg.Camera.COLOR_BUFFER1, b),
		(osg.Camera.COLOR_BUFFER2, c),
	)

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)
	_assert_newly_attached(c, c_cpp)

def test_attach_accepts_an_existing_sequence_variable_directly():
	# Python makes no distinction between a literal list/tuple and a variable holding one --
	# `args[0]` is the exact same object either way, so this hits the identical single-argument
	# path as the list/tuple-literal tests above.
	cam = osgx.RTT(4, 4)
	a, b = make_tex(), make_tex()
	a_cpp, b_cpp = a.referenceCount.cpp, b.referenceCount.cpp
	pairs = [(osg.Camera.COLOR_BUFFER0, a), (osg.Camera.COLOR_BUFFER1, b)]

	cam.attach(pairs)

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)

def test_attach_accepts_an_existing_sequence_unpacked_with_star():
	# `*pairs` unpacking happens in CPython's own call machinery before pybind11 ever sees the
	# arguments -- indistinguishable, at the binding, from the separate-positional-arguments
	# test above.
	cam = osgx.RTT(4, 4)
	a, b, c = make_tex(), make_tex(), make_tex()
	a_cpp, b_cpp, c_cpp = a.referenceCount.cpp, b.referenceCount.cpp, c.referenceCount.cpp
	pairs = [
		(osg.Camera.COLOR_BUFFER0, a),
		(osg.Camera.COLOR_BUFFER1, b),
		(osg.Camera.COLOR_BUFFER2, c),
	]

	cam.attach(*pairs)

	_assert_newly_attached(a, a_cpp)
	_assert_newly_attached(b, b_cpp)
	_assert_newly_attached(c, c_cpp)

def test_attach_accepts_a_single_bare_pair():
	# A single pair given bare (not wrapped in a list/tuple of pairs) is the case
	# unpack_list_or_args() falls through to the per-argument path for: casting the one 2-tuple
	# to std::vector<pair<...>> fails first (its OWN elements -- the BufferComponent and the
	# Texture -- don't individually cast to a pair), so it retries the whole tuple as one pair.
	cam = osgx.RTT(4, 4)
	a = make_tex()
	a_cpp = a.referenceCount.cpp

	cam.attach((osg.Camera.COLOR_BUFFER0, a))

	_assert_newly_attached(a, a_cpp)
