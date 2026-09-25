#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Program>
#include <osg/Shader>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osgx {

struct ShaderLib {
	std::string_view name;
	std::string_view glslName;
	std::string_view source;
};

// Registers a namespace's shader-library catalog (name/glslName/source triples) so
// resolveShaderLibs() can expand matching `#pragma <namespace> <lib1>[,<lib2>...]` directives
// found in GLSL source. Throws if namespaceName/libs is empty, or if a library with the same
// name is already registered under that namespace with different content.
void registerShaderLibs(std::string_view namespaceName, std::span<const ShaderLib> libs);

// Expands every `#pragma <namespace> <lib1>[,<lib2>...]` (or `#pragma <namespace> *` for all)
// directive in `src` whose namespace matches a registered catalog, inline-splicing the matching
// libraries' source. A library's own source is expanded the same way before it is spliced in, so
// a library may pull in other libraries; one that pulls itself in (directly or through others)
// throws. Nothing is deduplicated: a library pulled in twice is spliced twice. Pragmas for
// namespaces that were never registered are left untouched --
// this lets callers compose osgx's own snippet expansion with OSG's own state-driven shader
// variant pragmas (#pragma import_defines(...), import_modes(...), requires(...)) in the same
// source without conflict.
std::string resolveShaderLibs(std::string src);

// ================================================================================================
// Shader-object caching: osg::Shader INSTANCE dedup, the compile-time counterpart to
// resolveShaderLibs()'s text-splicing above. osg::Shader dedupes its own GL compile by instance
// identity, not source content: Program::apply() calls Shader::getPCS(state) once per attached
// Shader object, and a PerContextShader's compile is gated by `if(!_needsCompile) return`, so N
// Programs sharing the SAME osg::Shader* only ever trigger one real glCompileShader, while N
// Programs each holding their OWN Shader instance - even with byte-identical source - recompile
// independently every time. Confirmed empirically, not just by reading OSG's source.
// cachedShader() makes sharing the outcome by default for source that's likely to repeat.
// ================================================================================================

// Returns a cached osg::Shader for (type, src), compiling a new instance only the first time this
// exact (type, source text) pair is requested; every later call with the same pair returns the
// SAME instance, so Program::apply() finds it already compiled. Process-wide and never evicted --
// same tradeoff as the shaderLibCatalogs() registry above - which is fine for the intended use (a
// library's own default/no-op shader constants, or caller-supplied hook text that happens to
// repeat across call sites), but wrong for source built fresh with caller-specific data baked in
// as a literal every call: that source never actually repeats, so caching it only grows the map
// forever for no benefit.
osg::Shader* cachedShader(osg::Shader::Type type, std::string src);

// ================================================================================================
// Hook points: shader-object SUBSTITUTION, the counterpart to registerShaderLibs()/
// resolveShaderLibs()' text-splicing above. A Program-building call site (e.g.
// osgx::PBRScene::create()) declares which slots it supports via `defaults`;
// a caller overrides only the slots it cares about via `hooks`, leaving every other slot at that
// call site's own built-in. One shared enum/mechanism used everywhere osgx composes a Program
// this way, instead of each call site growing its own `osg::Shader* someHook=nullptr` parameter
// - see TODO.md's HookList entry for the history (osgSlug's Atlas.hpp is the model).
// ================================================================================================

// Hook slots a Program-building call site MAY expose for shader-object substitution. Only add a
// new enumerator once a real call site wires it up - see TODO.md's HookList entry for slots
// that are still discussed but not yet real parameters anywhere (e.g. ambient lighting); don't
// pre-populate this for hypothetical future hooks.
//
// Tonemap/Skinning substitute one leaf function's body while the rest of a Program stays fixed --
// genuinely reusable across whichever Program-building call sites happen to need that exact
// concept (Tonemap already is, across the forward PBR renderer and osgx::PBRLightingPass).
// DeferredLighting is different in kind: it substitutes the WHOLE shader that defines main() for
// osgx::PBRLightingPass::create()'s fullscreen lighting pass - an "I know what
// I'm doing, replace the entire pipeline" escape hatch, not a leaf-function swap, and NOT
// interchangeable with an equivalent hook on a differently-shaped Program (e.g. the forward path's
// own main() reads vertex-interpolated PBR inputs, not G-buffer textures) - hence the
// pass-specific name instead of a generic one. See docs/CORE.md and docs/GLTF.md.
//
// DirectLighting (migrated 2026-08-21, `osgx-gbuffer-dice.cpp`) - PBRLightingPass::create()
// used to attach its osgx_DirectLighting() shader (DIRECT_LIGHTING_HOOK_DEFAULT/_SHADOWED,
// PBR.hpp) UNCONDITIONALLY, outside applyHooks() entirely, on the assumption that a
// DeferredLighting override simply ignores it (true, and harmless, RIGHT UP UNTIL an override
// wants the same low-level BRDF primitives - D_GGX/G_Schlick/G_Smith/F_Schlick/DirectSpecular/
// DirectDiffuse/DirectLight - for its own use, since DIRECT_LIGHTING_HOOK_DEFAULT pulls in that
// exact same `#pragma osgx::pbr` set to implement itself. Two shader objects in one Program then
// define the same GLSL functions - a link error (`function "osgx_D_GGX" is already defined`),
// confirmed live building a worn-edge material blend that needed real osgx_DirectLight() calls
// from inside its own DeferredLighting override). Now a real slot: a DeferredLighting override
// that doesn't need osgx_DirectLighting() at all can supply a trivially empty
// `osg::Shader(FRAGMENT, "#version 460 core\n")` override for THIS slot too, freeing it to declare
// the underlying BRDF primitives itself with no collision.
enum class Hook {
	Tonemap,
	Skinning,
	DeferredLighting,
	DirectLighting,
};

// One hook-slot override: which slot, and the shader object substituting the call site's own
// built-in definition for it.
using HookList = std::vector<std::pair<Hook, osg::ref_ptr<osg::Shader>>>;

// True if `hooks` contains an override for `hook`.
bool hasHook(const HookList& hooks, Hook hook);

// Attaches exactly one shader per slot in `defaults` to `program` - the caller's override from
// `hooks` for that slot if present, otherwise `defaults`' own shader. `defaults` is the single
// source of truth for which slots this Program actually supports; every slot in it gets EXACTLY
// one definition attached, always - never zero (a call with no definition is a link error only
// caught at OSG's realize-time GLObjectsVisitor precompile, not at Program-build time) and never
// two (GLSL permits one body per function, so a `hooks` entry SUBSTITUTES the default, it is
// never attached alongside it). Throws if `hooks` names a slot absent from `defaults` - a hook
// this Program doesn't support, almost always a caller bug (typo, or a slot this call site
// doesn't actually have).
void applyHooks(osg::Program* program, const HookList& hooks, const HookList& defaults);

}
