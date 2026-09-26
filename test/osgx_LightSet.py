import math

import pytest

import osgx

from OpenSceneGraph import *


def test_light_set_is_a_real_state_attribute_with_its_own_member():
	# LightSet shares CAPABILITY with Material, but claims member 1 so both shader-facing bundles
	# can coexist on one StateSet without fighting over OSG's (Type, member) state-cache key.
	lights = osgx.LightSet()
	material = osgx.Material()

	assert isinstance(lights, osg.StateAttribute)
	assert lights.type == osg.StateAttribute.CAPABILITY
	assert lights.member == 1
	assert material.member == 0


def test_light_set_attaches_a_complete_default_light_set():
	ss = osg.StateSet()
	lights = osgx.LightSet()

	ss.attributes.append(lights)

	assert lights.valid()
	assert not any(lights.getEnabled(i) for i in range(osgx.MAX_LIGHTS))

	# The Python StateSet attribute proxy exposes member 0 only, while LightSet deliberately uses
	# member 1. append() still takes the real C++ setAttributeAndModes() path and confirms the
	# attribute can be installed alongside Material rather than replacing it.
	ss.attributes.append(osgx.Material())
	ss.attributes.append(lights)

	assert lights.valid()


def test_light_set_typed_setters_and_getters_round_trip():
	lights = osgx.LightSet()

	lights.setPoint(0, osg.Vec3(1.0, 2.0, 3.0), osg.Vec3(0.2, 0.4, 0.6), 7.0, 0.5)
	lights.setDirectional(1, osg.Vec3(0.0, 0.0, -1.0), osg.Vec3(0.8, 0.7, 0.6), 4.0)
	lights.setSpot(
		2,
		osg.Vec3(-1.0, 0.0, 2.0),
		osg.Vec3(0.0, 1.0, 0.0),
		osg.Vec3(1.0, 0.5, 0.25),
		9.0,
		0.2,
		0.4,
		0.75,
	)

	assert lights.getType(0) == osgx.LightType.Point
	assert lights.getEnabled(0)
	assert lights.getPosIntensity(0) == osg.Vec4(1.0, 2.0, 3.0, 7.0)
	assert lights.getColor(0) == osg.Vec3(0.2, 0.4, 0.6)
	assert lights.getSourceRadius(0) == pytest.approx(0.5)

	assert lights.getType(1) == osgx.LightType.Directional
	assert lights.getEnabled(1)
	assert lights.getDirection(1) == osg.Vec3(0.0, 0.0, -1.0)
	assert lights.getSourceRadius(1) == 0.0

	assert lights.getType(2) == osgx.LightType.Spot
	assert lights.getEnabled(2)
	assert lights.getPosIntensity(2) == osg.Vec4(-1.0, 0.0, 2.0, 9.0)
	assert lights.getDirection(2) == osg.Vec3(0.0, 1.0, 0.0)
	assert lights.getSpotAngles(2).x == pytest.approx(math.cos(0.2))
	assert lights.getSpotAngles(2).y == pytest.approx(math.cos(0.4))
	assert lights.getSourceRadius(2) == pytest.approx(0.75)

	lights.setEnabled(1, False)

	assert not lights.getEnabled(1)


def test_light_set_rejects_out_of_range_index():
	lights = osgx.LightSet()

	with pytest.raises(IndexError):
		lights.setPosition(osgx.MAX_LIGHTS, osg.Vec3(), 1.0)
