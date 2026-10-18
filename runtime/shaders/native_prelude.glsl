// native_prelude.glsl - the GL/Vulkan dialect layer of the shared Native shaders.
//
// Every shader that includes this file is compiled twice from the same text:
//   * Vulkan: glslc (tools/embed_spirv.py) defines VULKAN.
//   * OpenGL: tools/embed_glsl.py inlines the includes, replaces the first
//     "#version 450" line with the entry's "//! gl_version" and emits C strings.
// The shader bodies stay plain GLSL with their historical identifiers; only the
// macros below differ between the dialects.
//
// Vulkan resource layout (VK):
//   set 0, per-work resources (descriptor set rebuilt when the sampled resources change):
//     samplers / texel buffers / storage buffers / storage images, at the binding passed
//     to the PSX_* declaration macros below. These numbers equal the GL texture units
//     (Native ubershader: 0 words, 1 destination, 2 source_image, 3 destination_depth,
//     4 source_depth, 5 hd_image, 6 mip_image, 7 parameters, 8 recipe_words) so one
//     table serves both APIs. The GL image units 0/1 of the interlock images move to
//     9 (destination_image) and 10 (destination_depth_image) in VK.
//     Native core variant: destination (binding 1, input_attachment_index 0, RGBA8
//     colour) and destination_depth (binding 3, input_attachment_index 1, R32UI key)
//     are input attachments of the same subpass that writes them; plane 0's
//     provenance attachment is attachment 2 and is not read by the shader.
//   set 1, per-draw state, one std140 uniform block per PSX_UNIFORMS(Block, binding, LIST)
//     (dynamic offsets); members keep the GL loose-uniform names and are listed in the
//     LIST X-macro, so both dialects declare exactly the same fields.
//     Native: binding 0 NativeView (size, shared by vertex and fragment stage),
//             binding 1 NativeDraw (fragment stage).
//     Present: binding 0 PresentRect (u_uv_rect, vertex+fragment), binding 1 PresentDraw.
//     Interp: shares PresentRect (binding 0); binding 1 InterpDraw.
//     Scanout resolve: binding 0 ScanoutDraw. NativeSetup: binding 0 SetupDraw.
//     Fields are ordered vec4/ivec4 first, then arrays, ivec2, then 4-byte scalars so
//     std140 introduces no padding between them.
//   Varyings and fragment outputs carry explicit locations in VK only (PSX_VARYING);
//   GL keeps matching them by name, exactly as before.

#ifdef VULKAN

#define VERTEX_ID gl_VertexIndex
#define PSX_BINDING(g, v) set=0, binding=v
#define PSX_VARYING(n) layout(location=n)
#define PSX_SAMPLER2D(unit, name) layout(set=0, binding=unit) uniform sampler2D name;
#define PSX_USAMPLER2D(unit, name) layout(set=0, binding=unit) uniform usampler2D name;
#define PSX_USAMPLERBUFFER(unit, name) layout(set=0, binding=unit) uniform usamplerBuffer name;
#define PSX_UNIFORM_MEMBER(type, name) type name;
#define PSX_UNIFORMS(Block, slot, LIST) \
    layout(std140, set=1, binding=slot) uniform Block { LIST(PSX_UNIFORM_MEMBER) };

// Destination reads of the Native core variant.
#define NATIVE_DEST_DECLS \
    layout(input_attachment_index=0, set=0, binding=1) uniform subpassInput destination; \
    layout(input_attachment_index=1, set=0, binding=3) uniform usubpassInput destination_depth;
#define DEST_COLOR() subpassLoad(destination)
#define DEST_DEPTH() subpassLoad(destination_depth)

#else

#define VERTEX_ID gl_VertexID
#define PSX_BINDING(g, v) binding=g
#define PSX_VARYING(n)
#define PSX_SAMPLER2D(unit, name) uniform sampler2D name;
#define PSX_USAMPLER2D(unit, name) uniform usampler2D name;
#define PSX_USAMPLERBUFFER(unit, name) uniform usamplerBuffer name;
#define PSX_UNIFORM_LOOSE(type, name) uniform type name;
#define PSX_UNIFORMS(Block, slot, LIST) LIST(PSX_UNIFORM_LOOSE)

#define NATIVE_DEST_DECLS uniform usampler2D destination_depth; uniform sampler2D destination;
#define DEST_COLOR() texelFetch(destination, ivec2(gl_FragCoord.xy), 0)
#define DEST_DEPTH() texelFetch(destination_depth, ivec2(gl_FragCoord.xy), 0)

#endif
