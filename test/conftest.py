import os
import sys

sys.path.append("BUILD-g++-13.3.0-NOASAN")
sys.path.append("BUILD-clang++-18.1.3-NOASAN")

os.putenv("OSG_THREADING", "SingleThreaded")

import osgx
import pytest

from OpenSceneGraph import *

@pytest.fixture(scope="session", autouse=True)
def osgx_library():
	"""One osgx.Library for the whole test session, released after the last test."""

	yield osgx.initialize()

def refcmp(obj: osg.Referenced, cpp: int, py: int) -> bool:
	"""
	Compare an object's C++ and Python reference counts.

	The expected Python reference count is adjusted by +2 to account for:
		1) CPython's temporary reference during attribute access, and
		2) the reference held by passing `obj` into this function.

	This helper allows tests to express *logical* ownership expectations
	rather than raw CPython refcount mechanics. Mirrors OpenSceneGraph.py's
	test/conftest.py -- kept as a plain function (not imported cross-repo) since
	the two test suites are independently runnable, each from its own repo root.
	"""

	return obj.referenceCount == osg.RefCounts(cpp, py + 2)
