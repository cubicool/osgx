#pragma once

// libosgx-internal: registration of osgx's built-in shader-lib catalogs, called only by the
// osgx::Library constructor. Not installed; included only by libosgx's own translation units.

namespace osgx {

void registerEnvironmentShaderLibs();
void registerGBufferShaderLibs();
void registerGridShaderLibs();
void registerIBLShaderLibs();
void registerLightShaderLibs();
void registerPBRShaderLibs();
void registerPickShaderLibs();
void registerProjectionShaderLibs();
void registerSDFShaderLibs();
void registerShadowShaderLibs();
void registerSkinningShaderLibs();

}
