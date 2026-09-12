#include "osgx/Array.hpp"

// The one real instantiation matching each `extern template` declaration in Array.hpp - see
// the comment there for why this exists. Every other translation unit that includes Array.hpp
// borrows these instead of compiling its own copy.
template class osg::MixinVector<GLbyte>;
template class osg::MixinVector<GLubyte>;
template class osg::MixinVector<GLshort>;
template class osg::MixinVector<GLushort>;
template class osg::MixinVector<GLint>;
template class osg::MixinVector<GLuint>;
template class osg::MixinVector<GLfloat>;

template class osg::MixinVector<osg::Vec2>;
template class osg::MixinVector<osg::Vec3>;
template class osg::MixinVector<osg::Vec4>;

template class osg::MixinVector<osg::Vec2b>;
template class osg::MixinVector<osg::Vec3b>;
template class osg::MixinVector<osg::Vec4b>;

template class osg::MixinVector<osg::Vec2ub>;
template class osg::MixinVector<osg::Vec3ub>;
template class osg::MixinVector<osg::Vec4ub>;

template class osg::MixinVector<osg::Vec2s>;
template class osg::MixinVector<osg::Vec3s>;
template class osg::MixinVector<osg::Vec4s>;

template class osg::MixinVector<osg::Vec2us>;
template class osg::MixinVector<osg::Vec3us>;
template class osg::MixinVector<osg::Vec4us>;

template class osg::MixinVector<osg::Vec2i>;
template class osg::MixinVector<osg::Vec3i>;
template class osg::MixinVector<osg::Vec4i>;

template class osg::MixinVector<osg::Vec2ui>;
template class osg::MixinVector<osg::Vec3ui>;
template class osg::MixinVector<osg::Vec4ui>;

template class osg::MixinVector<osg::Matrixf>;
