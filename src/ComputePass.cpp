#include "osgx/ComputePass.hpp"
#include "osgx/RTT.hpp" // osgx::detail::bindTextureInputs

OSGX_DISABLE_WARNINGS

#include <osg/GLExtensions>
#include <osg/Shader>
#include <osg/State>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <array>

namespace osgx {

ComputePass::ComputePass(const std::string& name, osg::Program* program, GLbitfield barriers):
osg::DispatchCompute(1, 1, 1),
_barriers(barriers) {
	setName(name);
	setCullingActive(false);

	if(program) setProgram(program);
}

ComputePass::~ComputePass() {}

void ComputePass::drawImplementation(osg::RenderInfo& renderInfo) const {
	osg::DispatchCompute::drawImplementation(renderInfo);

	if(_barriers) renderInfo.getState()->get<osg::GLExtensions>()->glMemoryBarrier(_barriers);
}

void ComputePass::setProgram(osg::Program* program) {
	getOrCreateStateSet()->setAttributeAndModes(program);
}

osg::Program* ComputePass::getProgram() const {
	const auto* ss = getStateSet();

	return ss ? dynamic_cast<osg::Program*>(
		const_cast<osg::StateAttribute*>(ss->getAttribute(osg::StateAttribute::PROGRAM))
	) : nullptr;
}

void ComputePass::setComputeGroupsFor(int width, int height, int localSizeX, int localSizeY) {
	setComputeGroups(
		static_cast<GLint>((width + localSizeX - 1) / localSizeX),
		static_cast<GLint>((height + localSizeY - 1) / localSizeY),
		1
	);
}

void ComputePass::setRenderBin(int number) {
	getOrCreateStateSet()->setRenderBinDetails(number, "RenderBin");
}

void ComputePass::bindImage(
	GLuint unit,
	osg::Texture* texture,
	osg::BindImageTexture::Access access,
	GLenum format,
	int level
) {
	getOrCreateStateSet()->setAttribute(new osg::BindImageTexture(unit, texture, access, format, level));
}

void ComputePass::bindTexture(unsigned int unit, osg::Texture* texture, const char* uniformName) {
	const std::array<detail::TextureInput, 1> input = {{{unit, texture, uniformName}}};

	detail::bindTextureInputs(getOrCreateStateSet(), input);
}

void ComputePass::setDefineEnabled(const std::string& name, bool enabled) {
	auto* ss = getOrCreateStateSet();

	if(enabled) ss->setDefine(name, "1");

	else ss->removeDefine(name);
}

bool ComputePass::isDefineEnabled(const std::string& name) const {
	return getStateSet() && getStateSet()->getDefinePair(name);
}

osg::ref_ptr<osg::Program> ComputePass::createProgram(const std::string& name, const std::string& source) {
	auto program = osgx::make_nref<osg::Program>(name);

	program->addShader(new osg::Shader(osg::Shader::COMPUTE, source));

	return program;
}

osg::ref_ptr<osg::Texture2D> ComputePass::createTexture(
	const std::string& name,
	int width,
	int height,
	GLint internalFormat
) {
	auto texture = osgx::make_nref<osg::Texture2D>(name);

	texture->setTextureSize(width, height);
	texture->setInternalFormat(internalFormat);
	texture->setResizeNonPowerOfTwoHint(false);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	texture->setDataVariance(osg::Object::DYNAMIC);

	return texture;
}

}
