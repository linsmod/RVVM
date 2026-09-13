/*
 * GENERATED FILE - produced by tools/gen_gl_abi.py - DO NOT EDIT BY HAND.
 *
 * Source of truth: NDK sysroot headers GLES2/gl2.h + GLES3/gl3.h + EGL/egl.h
 * (parsed; gl3.h is merged after gl2.h so the GLES2 ids never move).
 * Regenerate with:  python tools/gen_gl_abi.py
 *
 * ABI notes:
 *  - fn_id macros are the single source of truth shared by the guest stubs,
 *    src/virtpass/vp_cmdpost.c and both host GL dispatches.
 *  - gl_call.args has 12 slots. glTexSubImage3D (GLES3) needs 11 and
 *    glCompressedTexSubImage2D (GLES2) 9; the extra slot keeps
 *    GL_CALL_RETBUF_SLOT above every real parameter list. Guest and host are
 *    rebuilt together, so widening it is an internal ABI change only.
 *  - Floats travel bit-packed through the int64_t slots; pointers travel as
 *    guest virtual addresses. Guest memory is NOT mapped into the host, so
 *    the host dispatch translates every data pointer argument with
 *    rvvm_user_guest_ptr() and, for calls that hand back a host-owned string
 *    (glGetString/glGetStringi/eglQueryString), copies it through the guest
 *    scratch buffer offered in args[GL_CALL_RETBUF_SLOT].
 *  - Opaque host values (EGLDisplay/Config/Surface/Context, GLsync) are only
 *    passed back by the guest and never translated.
 *  - The overloaded pointer arguments (glVertexAttribPointer/IPointer,
 *    glDrawElements/Instanced, glDrawRangeElements) travel as their bare
 *    value; the host reads it as a byte offset when a buffer is bound to the
 *    matching target and as a guest address otherwise (vpgl_ptr()).
 */

#ifndef VIRTPASS_GL
#define VIRTPASS_GL

#include <stdint.h>

/* ============================================================
 * GL types (khronos widths, riscv64 LP64 guest)
 * GLES2 core, plus the GLES3 additions (64-bit integers, GLsync)
 * ============================================================ */
typedef void             GLvoid;
typedef unsigned int     GLenum;
typedef unsigned char    GLboolean;
typedef unsigned int     GLbitfield;
typedef signed char      GLbyte;
typedef short            GLshort;
typedef int              GLint;
typedef unsigned char    GLubyte;
typedef unsigned short   GLushort;
typedef unsigned int     GLuint;
typedef int              GLsizei;
typedef float            GLfloat;
typedef float            GLclampf;
typedef char             GLchar;
typedef long             GLintptr;    /* khronos_intptr_t */
typedef long             GLsizeiptr;  /* khronos_ssize_t  */
typedef long             GLint64;     /* khronos_int64_t  */
typedef unsigned long    GLuint64;    /* khronos_uint64_t */
/* Fence/sync object. Never dereferenced: the guest only hands it back, so the
 * opaque pointer form is all the guest side needs. */
typedef void*            GLsync;

/* ============================================================
 * GLES2 + GLES3 symbolic constants
 *
 * 2131 tokens, parsed out of the NDK headers - nothing in here is
 * hand-maintained, so a guest can reach for every symbolic constant
 * the real headers offer instead of only the ones somebody typed in.
 *
 * gl2.h is merged before gl3.h, exactly like the prototypes: a shared
 * token keeps its GLES2 spelling rather than being re-pinned by the ES3
 * copy. gl2ext.h/gl3ext.h contribute their tokens too - constants only,
 * never prototypes, since extensions have no fn_id here.
 * ============================================================ */
#define GL_3DC_XY_AMD                        0x87FA
#define GL_3DC_X_AMD                         0x87F9
#define GL_ACCUM_ADJACENT_PAIRS_NV           0x90AD
#define GL_ACTIVE_ATTRIBUTES                 0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH       0x8B8A
#define GL_ACTIVE_PROGRAM_EXT                0x8259
#define GL_ACTIVE_TEXTURE                    0x84E0
#define GL_ACTIVE_UNIFORMS                   0x8B86
#define GL_ACTIVE_UNIFORM_BLOCKS             0x8A36
#define GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define GL_ACTIVE_UNIFORM_MAX_LENGTH         0x8B87
#define GL_ADJACENT_PAIRS_NV                 0x90AE
#define GL_AFFINE_2D_NV                      0x9092
#define GL_AFFINE_3D_NV                      0x9094
#define GL_ALIASED_LINE_WIDTH_RANGE          0x846E
#define GL_ALIASED_POINT_SIZE_RANGE          0x846D
#define GL_ALL_COMPLETED_NV                  0x84F2
#define GL_ALL_SHADER_BITS_EXT               0xFFFFFFFF
#define GL_ALPHA                             0x1906
#define GL_ALPHA16F_EXT                      0x881C
#define GL_ALPHA32F_EXT                      0x8816
#define GL_ALPHA8_EXT                        0x803C
#define GL_ALPHA8_OES                        0x803C
#define GL_ALPHA_BITS                        0xD55
#define GL_ALPHA_TEST_FUNC_QCOM              0xBC1
#define GL_ALPHA_TEST_QCOM                   0xBC0
#define GL_ALPHA_TEST_REF_QCOM               0xBC2
#define GL_ALREADY_SIGNALED                  0x911A
#define GL_ALREADY_SIGNALED_APPLE            0x911A
#define GL_ALWAYS                            0x207
#define GL_AMD_compressed_3DC_texture        1
#define GL_AMD_compressed_ATC_texture        1
#define GL_AMD_performance_monitor           1
#define GL_AMD_program_binary_Z400           1
#define GL_ANDROID_extension_pack_es31a      1
#define GL_ANGLE_depth_texture               1
#define GL_ANGLE_framebuffer_blit            1
#define GL_ANGLE_framebuffer_multisample     1
#define GL_ANGLE_instanced_arrays            1
#define GL_ANGLE_pack_reverse_row_order      1
#define GL_ANGLE_program_binary              1
#define GL_ANGLE_texture_compression_dxt3    1
#define GL_ANGLE_texture_compression_dxt5    1
#define GL_ANGLE_texture_usage               1
#define GL_ANGLE_translated_shader_source    1
#define GL_ANY_SAMPLES_PASSED                0x8C2F
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE   0x8D6A
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE_EXT 0x8D6A
#define GL_ANY_SAMPLES_PASSED_EXT            0x8C2F
#define GL_APPLE_clip_distance               1
#define GL_APPLE_color_buffer_packed_float   1
#define GL_APPLE_copy_texture_levels         1
#define GL_APPLE_framebuffer_multisample     1
#define GL_APPLE_rgb_422                     1
#define GL_APPLE_sync                        1
#define GL_APPLE_texture_format_BGRA8888     1
#define GL_APPLE_texture_max_level           1
#define GL_APPLE_texture_packed_float        1
#define GL_ARC_TO_NV                         0xFE
#define GL_ARM_mali_program_binary           1
#define GL_ARM_mali_shader_binary            1
#define GL_ARM_rgba8                         1
#define GL_ARM_shader_framebuffer_fetch      1
#define GL_ARM_shader_framebuffer_fetch_depth_stencil 1
#define GL_ARRAY_BUFFER                      0x8892
#define GL_ARRAY_BUFFER_BINDING              0x8894
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD       0x8C93
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD   0x87EE
#define GL_ATC_RGB_AMD                       0x8C92
#define GL_ATTACHED_SHADERS                  0x8B85
#define GL_BACK                              0x405
#define GL_BEVEL_NV                          0x90A6
#define GL_BGRA8_EXT                         0x93A1
#define GL_BGRA_EXT                          0x80E1
#define GL_BGRA_IMG                          0x80E1
#define GL_BINNING_CONTROL_HINT_QCOM         0x8FB0
#define GL_BLACKHOLE_RENDER_INTEL            0x83FC
#define GL_BLEND                             0xBE2
#define GL_BLEND_ADVANCED_COHERENT_KHR       0x9285
#define GL_BLEND_ADVANCED_COHERENT_NV        0x9285
#define GL_BLEND_COLOR                       0x8005
#define GL_BLEND_DST_ALPHA                   0x80CA
#define GL_BLEND_DST_RGB                     0x80C8
#define GL_BLEND_EQUATION                    0x8009
#define GL_BLEND_EQUATION_ALPHA              0x883D
#define GL_BLEND_EQUATION_RGB                0x8009
#define GL_BLEND_OVERLAP_NV                  0x9281
#define GL_BLEND_PREMULTIPLIED_SRC_NV        0x9280
#define GL_BLEND_SRC_ALPHA                   0x80CB
#define GL_BLEND_SRC_RGB                     0x80C9
#define GL_BLUE                              0x1905
#define GL_BLUE_BITS                         0xD54
#define GL_BLUE_NV                           0x1905
#define GL_BOLD_BIT_NV                       1
#define GL_BOOL                              0x8B56
#define GL_BOOL_VEC2                         0x8B57
#define GL_BOOL_VEC3                         0x8B58
#define GL_BOOL_VEC4                         0x8B59
#define GL_BOUNDING_BOX_NV                   0x908D
#define GL_BOUNDING_BOX_OF_BOUNDING_BOXES_NV 0x909C
#define GL_BUFFER_ACCESS_FLAGS               0x911F
#define GL_BUFFER_ACCESS_OES                 0x88BB
#define GL_BUFFER_IMMUTABLE_STORAGE_EXT      0x821F
#define GL_BUFFER_KHR                        0x82E0
#define GL_BUFFER_MAPPED                     0x88BC
#define GL_BUFFER_MAPPED_OES                 0x88BC
#define GL_BUFFER_MAP_LENGTH                 0x9120
#define GL_BUFFER_MAP_OFFSET                 0x9121
#define GL_BUFFER_MAP_POINTER                0x88BD
#define GL_BUFFER_MAP_POINTER_OES            0x88BD
#define GL_BUFFER_OBJECT_EXT                 0x9151
#define GL_BUFFER_SIZE                       0x8764
#define GL_BUFFER_STORAGE_FLAGS_EXT          0x8220
#define GL_BUFFER_USAGE                      0x8765
#define GL_BYTE                              0x1400
#define GL_CCW                               0x901
#define GL_CIRCULAR_CCW_ARC_TO_NV            0xF8
#define GL_CIRCULAR_CW_ARC_TO_NV             0xFA
#define GL_CIRCULAR_TANGENT_ARC_TO_NV        0xFC
#define GL_CLAMP_TO_BORDER_EXT               0x812D
#define GL_CLAMP_TO_BORDER_NV                0x812D
#define GL_CLAMP_TO_BORDER_OES               0x812D
#define GL_CLAMP_TO_EDGE                     0x812F
#define GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT 0x4000
#define GL_CLIENT_STORAGE_BIT_EXT            0x200
#define GL_CLIP_DEPTH_MODE_EXT               0x935D
#define GL_CLIP_DISTANCE0_APPLE              0x3000
#define GL_CLIP_DISTANCE0_EXT                0x3000
#define GL_CLIP_DISTANCE1_APPLE              0x3001
#define GL_CLIP_DISTANCE1_EXT                0x3001
#define GL_CLIP_DISTANCE2_APPLE              0x3002
#define GL_CLIP_DISTANCE2_EXT                0x3002
#define GL_CLIP_DISTANCE3_APPLE              0x3003
#define GL_CLIP_DISTANCE3_EXT                0x3003
#define GL_CLIP_DISTANCE4_APPLE              0x3004
#define GL_CLIP_DISTANCE4_EXT                0x3004
#define GL_CLIP_DISTANCE5_APPLE              0x3005
#define GL_CLIP_DISTANCE5_EXT                0x3005
#define GL_CLIP_DISTANCE6_APPLE              0x3006
#define GL_CLIP_DISTANCE6_EXT                0x3006
#define GL_CLIP_DISTANCE7_APPLE              0x3007
#define GL_CLIP_DISTANCE7_EXT                0x3007
#define GL_CLIP_ORIGIN_EXT                   0x935C
#define GL_CLOSE_PATH_NV                     0
#define GL_COLOR                             0x1800
#define GL_COLORBURN_KHR                     0x929A
#define GL_COLORBURN_NV                      0x929A
#define GL_COLORDODGE_KHR                    0x9299
#define GL_COLORDODGE_NV                     0x9299
#define GL_COLOR_ATTACHMENT0                 0x8CE0
#define GL_COLOR_ATTACHMENT0_EXT             0x8CE0
#define GL_COLOR_ATTACHMENT0_NV              0x8CE0
#define GL_COLOR_ATTACHMENT1                 0x8CE1
#define GL_COLOR_ATTACHMENT10                0x8CEA
#define GL_COLOR_ATTACHMENT10_EXT            0x8CEA
#define GL_COLOR_ATTACHMENT10_NV             0x8CEA
#define GL_COLOR_ATTACHMENT11                0x8CEB
#define GL_COLOR_ATTACHMENT11_EXT            0x8CEB
#define GL_COLOR_ATTACHMENT11_NV             0x8CEB
#define GL_COLOR_ATTACHMENT12                0x8CEC
#define GL_COLOR_ATTACHMENT12_EXT            0x8CEC
#define GL_COLOR_ATTACHMENT12_NV             0x8CEC
#define GL_COLOR_ATTACHMENT13                0x8CED
#define GL_COLOR_ATTACHMENT13_EXT            0x8CED
#define GL_COLOR_ATTACHMENT13_NV             0x8CED
#define GL_COLOR_ATTACHMENT14                0x8CEE
#define GL_COLOR_ATTACHMENT14_EXT            0x8CEE
#define GL_COLOR_ATTACHMENT14_NV             0x8CEE
#define GL_COLOR_ATTACHMENT15                0x8CEF
#define GL_COLOR_ATTACHMENT15_EXT            0x8CEF
#define GL_COLOR_ATTACHMENT15_NV             0x8CEF
#define GL_COLOR_ATTACHMENT16                0x8CF0
#define GL_COLOR_ATTACHMENT17                0x8CF1
#define GL_COLOR_ATTACHMENT18                0x8CF2
#define GL_COLOR_ATTACHMENT19                0x8CF3
#define GL_COLOR_ATTACHMENT1_EXT             0x8CE1
#define GL_COLOR_ATTACHMENT1_NV              0x8CE1
#define GL_COLOR_ATTACHMENT2                 0x8CE2
#define GL_COLOR_ATTACHMENT20                0x8CF4
#define GL_COLOR_ATTACHMENT21                0x8CF5
#define GL_COLOR_ATTACHMENT22                0x8CF6
#define GL_COLOR_ATTACHMENT23                0x8CF7
#define GL_COLOR_ATTACHMENT24                0x8CF8
#define GL_COLOR_ATTACHMENT25                0x8CF9
#define GL_COLOR_ATTACHMENT26                0x8CFA
#define GL_COLOR_ATTACHMENT27                0x8CFB
#define GL_COLOR_ATTACHMENT28                0x8CFC
#define GL_COLOR_ATTACHMENT29                0x8CFD
#define GL_COLOR_ATTACHMENT2_EXT             0x8CE2
#define GL_COLOR_ATTACHMENT2_NV              0x8CE2
#define GL_COLOR_ATTACHMENT3                 0x8CE3
#define GL_COLOR_ATTACHMENT30                0x8CFE
#define GL_COLOR_ATTACHMENT31                0x8CFF
#define GL_COLOR_ATTACHMENT3_EXT             0x8CE3
#define GL_COLOR_ATTACHMENT3_NV              0x8CE3
#define GL_COLOR_ATTACHMENT4                 0x8CE4
#define GL_COLOR_ATTACHMENT4_EXT             0x8CE4
#define GL_COLOR_ATTACHMENT4_NV              0x8CE4
#define GL_COLOR_ATTACHMENT5                 0x8CE5
#define GL_COLOR_ATTACHMENT5_EXT             0x8CE5
#define GL_COLOR_ATTACHMENT5_NV              0x8CE5
#define GL_COLOR_ATTACHMENT6                 0x8CE6
#define GL_COLOR_ATTACHMENT6_EXT             0x8CE6
#define GL_COLOR_ATTACHMENT6_NV              0x8CE6
#define GL_COLOR_ATTACHMENT7                 0x8CE7
#define GL_COLOR_ATTACHMENT7_EXT             0x8CE7
#define GL_COLOR_ATTACHMENT7_NV              0x8CE7
#define GL_COLOR_ATTACHMENT8                 0x8CE8
#define GL_COLOR_ATTACHMENT8_EXT             0x8CE8
#define GL_COLOR_ATTACHMENT8_NV              0x8CE8
#define GL_COLOR_ATTACHMENT9                 0x8CE9
#define GL_COLOR_ATTACHMENT9_EXT             0x8CE9
#define GL_COLOR_ATTACHMENT9_NV              0x8CE9
#define GL_COLOR_ATTACHMENT_EXT              0x90F0
#define GL_COLOR_BUFFER_BIT                  0x4000
#define GL_COLOR_BUFFER_BIT0_QCOM            1
#define GL_COLOR_BUFFER_BIT1_QCOM            2
#define GL_COLOR_BUFFER_BIT2_QCOM            4
#define GL_COLOR_BUFFER_BIT3_QCOM            8
#define GL_COLOR_BUFFER_BIT4_QCOM            0x10
#define GL_COLOR_BUFFER_BIT5_QCOM            0x20
#define GL_COLOR_BUFFER_BIT6_QCOM            0x40
#define GL_COLOR_BUFFER_BIT7_QCOM            0x80
#define GL_COLOR_CLEAR_VALUE                 0xC22
#define GL_COLOR_EXT                         0x1800
#define GL_COLOR_SAMPLES_NV                  0x8E20
#define GL_COLOR_WRITEMASK                   0xC23
#define GL_COMPARE_REF_TO_TEXTURE            0x884E
#define GL_COMPARE_REF_TO_TEXTURE_EXT        0x884E
#define GL_COMPILE_STATUS                    0x8B81
#define GL_COMPLETION_STATUS_KHR             0x91B1
#define GL_COMPRESSED_R11_EAC                0x9270
#define GL_COMPRESSED_RED_GREEN_RGTC2_EXT    0x8DBD
#define GL_COMPRESSED_RED_RGTC1_EXT          0x8DBB
#define GL_COMPRESSED_RG11_EAC               0x9272
#define GL_COMPRESSED_RGB8_ETC2              0x9274
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9276
#define GL_COMPRESSED_RGBA8_ETC2_EAC         0x9278
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR    0x93BB
#define GL_COMPRESSED_RGBA_ASTC_10x5_KHR     0x93B8
#define GL_COMPRESSED_RGBA_ASTC_10x6_KHR     0x93B9
#define GL_COMPRESSED_RGBA_ASTC_10x8_KHR     0x93BA
#define GL_COMPRESSED_RGBA_ASTC_12x10_KHR    0x93BC
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR    0x93BD
#define GL_COMPRESSED_RGBA_ASTC_3x3x3_OES    0x93C0
#define GL_COMPRESSED_RGBA_ASTC_4x3x3_OES    0x93C1
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR      0x93B0
#define GL_COMPRESSED_RGBA_ASTC_4x4x3_OES    0x93C2
#define GL_COMPRESSED_RGBA_ASTC_4x4x4_OES    0x93C3
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR      0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x4x4_OES    0x93C4
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR      0x93B2
#define GL_COMPRESSED_RGBA_ASTC_5x5x4_OES    0x93C5
#define GL_COMPRESSED_RGBA_ASTC_5x5x5_OES    0x93C6
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR      0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x5x5_OES    0x93C7
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR      0x93B4
#define GL_COMPRESSED_RGBA_ASTC_6x6x5_OES    0x93C8
#define GL_COMPRESSED_RGBA_ASTC_6x6x6_OES    0x93C9
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR      0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR      0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR      0x93B7
#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT    0x8E8C
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG  0x8C03
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG  0x9137
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG  0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG  0x9138
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT     0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_ANGLE   0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT     0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_ANGLE   0x83F3
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT     0x83F3
#define GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT 0x8E8E
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT 0x8E8F
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG   0x8C01
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG   0x8C00
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT      0x83F0
#define GL_COMPRESSED_SIGNED_R11_EAC         0x9271
#define GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT 0x8DBE
#define GL_COMPRESSED_SIGNED_RED_RGTC1_EXT   0x8DBC
#define GL_COMPRESSED_SIGNED_RG11_EAC        0x9273
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR 0x93DB
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR 0x93D8
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR 0x93D9
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR 0x93DA
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR 0x93DC
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR 0x93DD
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_3x3x3_OES 0x93E0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x3x3_OES 0x93E1
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR 0x93D0
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4x3_OES 0x93E2
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4x4_OES 0x93E3
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR 0x93D1
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4x4_OES 0x93E4
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR 0x93D2
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5x4_OES 0x93E5
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5x5_OES 0x93E6
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR 0x93D3
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5x5_OES 0x93E7
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR 0x93D4
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6x5_OES 0x93E8
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6x6_OES 0x93E9
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR 0x93D5
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR 0x93D6
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR 0x93D7
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC  0x9279
#define GL_COMPRESSED_SRGB8_ETC2             0x9275
#define GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9277
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT 0x8E8D
#define GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT 0x8A56
#define GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV2_IMG 0x93F0
#define GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT 0x8A57
#define GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV2_IMG 0x93F1
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_NV 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_NV 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_NV 0x8C4F
#define GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT  0x8A54
#define GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT  0x8A55
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT     0x8C4C
#define GL_COMPRESSED_SRGB_S3TC_DXT1_NV      0x8C4C
#define GL_COMPRESSED_TEXTURE_FORMATS        0x86A3
#define GL_CONDITION_SATISFIED               0x911C
#define GL_CONDITION_SATISFIED_APPLE         0x911C
#define GL_CONFORMANT_NV                     0x9374
#define GL_CONIC_CURVE_TO_NV                 0x1A
#define GL_CONJOINT_NV                       0x9284
#define GL_CONSERVATIVE_RASTERIZATION_INTEL  0x83FE
#define GL_CONSERVATIVE_RASTERIZATION_NV     0x9346
#define GL_CONSERVATIVE_RASTER_MODE_NV       0x954D
#define GL_CONSERVATIVE_RASTER_MODE_POST_SNAP_NV 0x954E
#define GL_CONSERVATIVE_RASTER_MODE_PRE_SNAP_NV 0x9550
#define GL_CONSERVATIVE_RASTER_MODE_PRE_SNAP_TRIANGLES_NV 0x954F
#define GL_CONSTANT_ALPHA                    0x8003
#define GL_CONSTANT_COLOR                    0x8001
#define GL_CONTEXT_FLAG_DEBUG_BIT_KHR        2
#define GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR     8
#define GL_CONTEXT_FLAG_PROTECTED_CONTENT_BIT_EXT 0x10
#define GL_CONTEXT_LOST_KHR                  0x507
#define GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_KHR 0x82FC
#define GL_CONTEXT_RELEASE_BEHAVIOR_KHR      0x82FB
#define GL_CONTEXT_ROBUST_ACCESS_EXT         0x90F3
#define GL_CONTEXT_ROBUST_ACCESS_KHR         0x90F3
#define GL_CONTRAST_NV                       0x92A1
#define GL_CONVEX_HULL_NV                    0x908B
#define GL_COPY_READ_BUFFER                  0x8F36
#define GL_COPY_READ_BUFFER_BINDING          0x8F36
#define GL_COPY_READ_BUFFER_NV               0x8F36
#define GL_COPY_WRITE_BUFFER                 0x8F37
#define GL_COPY_WRITE_BUFFER_BINDING         0x8F37
#define GL_COPY_WRITE_BUFFER_NV              0x8F37
#define GL_COUNTER_RANGE_AMD                 0x8BC1
#define GL_COUNTER_TYPE_AMD                  0x8BC0
#define GL_COUNT_DOWN_NV                     0x9089
#define GL_COUNT_UP_NV                       0x9088
#define GL_COVERAGE_ALL_FRAGMENTS_NV         0x8ED5
#define GL_COVERAGE_ATTACHMENT_NV            0x8ED2
#define GL_COVERAGE_AUTOMATIC_NV             0x8ED7
#define GL_COVERAGE_BUFFERS_NV               0x8ED3
#define GL_COVERAGE_BUFFER_BIT_NV            0x8000
#define GL_COVERAGE_COMPONENT4_NV            0x8ED1
#define GL_COVERAGE_COMPONENT_NV             0x8ED0
#define GL_COVERAGE_EDGE_FRAGMENTS_NV        0x8ED6
#define GL_COVERAGE_MODULATION_NV            0x9332
#define GL_COVERAGE_MODULATION_TABLE_NV      0x9331
#define GL_COVERAGE_MODULATION_TABLE_SIZE_NV 0x9333
#define GL_COVERAGE_SAMPLES_NV               0x8ED4
#define GL_CPU_OPTIMIZED_QCOM                0x8FB1
#define GL_CUBIC_CURVE_TO_NV                 12
#define GL_CUBIC_IMG                         0x9139
#define GL_CUBIC_MIPMAP_LINEAR_IMG           0x913B
#define GL_CUBIC_MIPMAP_NEAREST_IMG          0x913A
#define GL_CULL_FACE                         0xB44
#define GL_CULL_FACE_MODE                    0xB45
#define GL_CURRENT_PROGRAM                   0x8B8D
#define GL_CURRENT_QUERY                     0x8865
#define GL_CURRENT_QUERY_EXT                 0x8865
#define GL_CURRENT_VERTEX_ATTRIB             0x8626
#define GL_CW                                0x900
#define GL_D3D12_FENCE_VALUE_EXT             0x9595
#define GL_DARKEN_KHR                        0x9297
#define GL_DARKEN_NV                         0x9297
#define GL_DEBUG_CALLBACK_FUNCTION_KHR       0x8244
#define GL_DEBUG_CALLBACK_USER_PARAM_KHR     0x8245
#define GL_DEBUG_GROUP_STACK_DEPTH_KHR       0x826D
#define GL_DEBUG_LOGGED_MESSAGES_KHR         0x9145
#define GL_DEBUG_NEXT_LOGGED_MESSAGE_LENGTH_KHR 0x8243
#define GL_DEBUG_OUTPUT_KHR                  0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR      0x8242
#define GL_DEBUG_SEVERITY_HIGH_KHR           0x9146
#define GL_DEBUG_SEVERITY_LOW_KHR            0x9148
#define GL_DEBUG_SEVERITY_MEDIUM_KHR         0x9147
#define GL_DEBUG_SEVERITY_NOTIFICATION_KHR   0x826B
#define GL_DEBUG_SOURCE_API_KHR              0x8246
#define GL_DEBUG_SOURCE_APPLICATION_KHR      0x824A
#define GL_DEBUG_SOURCE_OTHER_KHR            0x824B
#define GL_DEBUG_SOURCE_SHADER_COMPILER_KHR  0x8248
#define GL_DEBUG_SOURCE_THIRD_PARTY_KHR      0x8249
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM_KHR    0x8247
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR 0x824D
#define GL_DEBUG_TYPE_ERROR_KHR              0x824C
#define GL_DEBUG_TYPE_MARKER_KHR             0x8268
#define GL_DEBUG_TYPE_OTHER_KHR              0x8251
#define GL_DEBUG_TYPE_PERFORMANCE_KHR        0x8250
#define GL_DEBUG_TYPE_POP_GROUP_KHR          0x826A
#define GL_DEBUG_TYPE_PORTABILITY_KHR        0x824F
#define GL_DEBUG_TYPE_PUSH_GROUP_KHR         0x8269
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR 0x824E
#define GL_DECODE_EXT                        0x8A49
#define GL_DECR                              0x1E03
#define GL_DECR_WRAP                         0x8508
#define GL_DEDICATED_MEMORY_OBJECT_EXT       0x9581
#define GL_DELETE_STATUS                     0x8B80
#define GL_DEPTH                             0x1801
#define GL_DEPTH24_STENCIL8                  0x88F0
#define GL_DEPTH24_STENCIL8_OES              0x88F0
#define GL_DEPTH32F_STENCIL8                 0x8CAD
#define GL_DEPTH_ATTACHMENT                  0x8D00
#define GL_DEPTH_BITS                        0xD56
#define GL_DEPTH_BUFFER_BIT                  0x100
#define GL_DEPTH_BUFFER_BIT0_QCOM            0x100
#define GL_DEPTH_BUFFER_BIT1_QCOM            0x200
#define GL_DEPTH_BUFFER_BIT2_QCOM            0x400
#define GL_DEPTH_BUFFER_BIT3_QCOM            0x800
#define GL_DEPTH_BUFFER_BIT4_QCOM            0x1000
#define GL_DEPTH_BUFFER_BIT5_QCOM            0x2000
#define GL_DEPTH_BUFFER_BIT6_QCOM            0x4000
#define GL_DEPTH_BUFFER_BIT7_QCOM            0x8000
#define GL_DEPTH_CLEAR_VALUE                 0xB73
#define GL_DEPTH_COMPONENT                   0x1902
#define GL_DEPTH_COMPONENT16                 0x81A5
#define GL_DEPTH_COMPONENT16_NONLINEAR_NV    0x8E2C
#define GL_DEPTH_COMPONENT16_OES             0x81A5
#define GL_DEPTH_COMPONENT24                 0x81A6
#define GL_DEPTH_COMPONENT24_OES             0x81A6
#define GL_DEPTH_COMPONENT32F                0x8CAC
#define GL_DEPTH_COMPONENT32_OES             0x81A7
#define GL_DEPTH_EXT                         0x1801
#define GL_DEPTH_FUNC                        0xB74
#define GL_DEPTH_RANGE                       0xB70
#define GL_DEPTH_SAMPLES_NV                  0x932D
#define GL_DEPTH_STENCIL                     0x84F9
#define GL_DEPTH_STENCIL_ATTACHMENT          0x821A
#define GL_DEPTH_STENCIL_OES                 0x84F9
#define GL_DEPTH_TEST                        0xB71
#define GL_DEPTH_WRITEMASK                   0xB72
#define GL_DEVICE_LUID_EXT                   0x9599
#define GL_DEVICE_NODE_MASK_EXT              0x959A
#define GL_DEVICE_UUID_EXT                   0x9597
#define GL_DIFFERENCE_KHR                    0x929E
#define GL_DIFFERENCE_NV                     0x929E
#define GL_DISJOINT_NV                       0x9283
#define GL_DITHER                            0xBD0
#define GL_DMP_PROGRAM_BINARY_DMP            0x9253
#define GL_DMP_program_binary                1
#define GL_DMP_shader_binary                 1
#define GL_DONT_CARE                         0x1100
#define GL_DOWNSAMPLE_SCALES_IMG             0x913E
#define GL_DRAW_BUFFER0                      0x8825
#define GL_DRAW_BUFFER0_EXT                  0x8825
#define GL_DRAW_BUFFER0_NV                   0x8825
#define GL_DRAW_BUFFER1                      0x8826
#define GL_DRAW_BUFFER10                     0x882F
#define GL_DRAW_BUFFER10_EXT                 0x882F
#define GL_DRAW_BUFFER10_NV                  0x882F
#define GL_DRAW_BUFFER11                     0x8830
#define GL_DRAW_BUFFER11_EXT                 0x8830
#define GL_DRAW_BUFFER11_NV                  0x8830
#define GL_DRAW_BUFFER12                     0x8831
#define GL_DRAW_BUFFER12_EXT                 0x8831
#define GL_DRAW_BUFFER12_NV                  0x8831
#define GL_DRAW_BUFFER13                     0x8832
#define GL_DRAW_BUFFER13_EXT                 0x8832
#define GL_DRAW_BUFFER13_NV                  0x8832
#define GL_DRAW_BUFFER14                     0x8833
#define GL_DRAW_BUFFER14_EXT                 0x8833
#define GL_DRAW_BUFFER14_NV                  0x8833
#define GL_DRAW_BUFFER15                     0x8834
#define GL_DRAW_BUFFER15_EXT                 0x8834
#define GL_DRAW_BUFFER15_NV                  0x8834
#define GL_DRAW_BUFFER1_EXT                  0x8826
#define GL_DRAW_BUFFER1_NV                   0x8826
#define GL_DRAW_BUFFER2                      0x8827
#define GL_DRAW_BUFFER2_EXT                  0x8827
#define GL_DRAW_BUFFER2_NV                   0x8827
#define GL_DRAW_BUFFER3                      0x8828
#define GL_DRAW_BUFFER3_EXT                  0x8828
#define GL_DRAW_BUFFER3_NV                   0x8828
#define GL_DRAW_BUFFER4                      0x8829
#define GL_DRAW_BUFFER4_EXT                  0x8829
#define GL_DRAW_BUFFER4_NV                   0x8829
#define GL_DRAW_BUFFER5                      0x882A
#define GL_DRAW_BUFFER5_EXT                  0x882A
#define GL_DRAW_BUFFER5_NV                   0x882A
#define GL_DRAW_BUFFER6                      0x882B
#define GL_DRAW_BUFFER6_EXT                  0x882B
#define GL_DRAW_BUFFER6_NV                   0x882B
#define GL_DRAW_BUFFER7                      0x882C
#define GL_DRAW_BUFFER7_EXT                  0x882C
#define GL_DRAW_BUFFER7_NV                   0x882C
#define GL_DRAW_BUFFER8                      0x882D
#define GL_DRAW_BUFFER8_EXT                  0x882D
#define GL_DRAW_BUFFER8_NV                   0x882D
#define GL_DRAW_BUFFER9                      0x882E
#define GL_DRAW_BUFFER9_EXT                  0x882E
#define GL_DRAW_BUFFER9_NV                   0x882E
#define GL_DRAW_BUFFER_EXT                   0xC01
#define GL_DRAW_FRAMEBUFFER                  0x8CA9
#define GL_DRAW_FRAMEBUFFER_ANGLE            0x8CA9
#define GL_DRAW_FRAMEBUFFER_APPLE            0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING          0x8CA6
#define GL_DRAW_FRAMEBUFFER_BINDING_ANGLE    0x8CA6
#define GL_DRAW_FRAMEBUFFER_BINDING_APPLE    0x8CA6
#define GL_DRAW_FRAMEBUFFER_BINDING_NV       0x8CA6
#define GL_DRAW_FRAMEBUFFER_NV               0x8CA9
#define GL_DRIVER_UUID_EXT                   0x9598
#define GL_DST_ALPHA                         0x304
#define GL_DST_ATOP_NV                       0x928F
#define GL_DST_COLOR                         0x306
#define GL_DST_IN_NV                         0x928B
#define GL_DST_NV                            0x9287
#define GL_DST_OUT_NV                        0x928D
#define GL_DST_OVER_NV                       0x9289
#define GL_DUP_FIRST_CUBIC_CURVE_TO_NV       0xF2
#define GL_DUP_LAST_CUBIC_CURVE_TO_NV        0xF4
#define GL_DYNAMIC_COPY                      0x88EA
#define GL_DYNAMIC_DRAW                      0x88E8
#define GL_DYNAMIC_READ                      0x88E9
#define GL_DYNAMIC_STORAGE_BIT_EXT           0x100
#define GL_EFFECTIVE_RASTER_SAMPLES_EXT      0x932C
#define GL_ELEMENT_ARRAY_BUFFER              0x8893
#define GL_ELEMENT_ARRAY_BUFFER_BINDING      0x8895
#define GL_EQUAL                             0x202
#define GL_ES_VERSION_2_0                    1
#define GL_ES_VERSION_3_0                    1
#define GL_ETC1_RGB8_OES                     0x8D64
#define GL_ETC1_SRGB8_NV                     0x88EE
#define GL_EXCLUSION_KHR                     0x92A0
#define GL_EXCLUSION_NV                      0x92A0
#define GL_EXCLUSIVE_EXT                     0x8F11
#define GL_EXTENSIONS                        0x1F03
#define GL_EXT_EGL_image_array               1
#define GL_EXT_EGL_image_storage             1
#define GL_EXT_YUV_target                    1
#define GL_EXT_base_instance                 1
#define GL_EXT_blend_func_extended           1
#define GL_EXT_blend_minmax                  1
#define GL_EXT_buffer_storage                1
#define GL_EXT_clear_texture                 1
#define GL_EXT_clip_control                  1
#define GL_EXT_clip_cull_distance            1
#define GL_EXT_color_buffer_float            1
#define GL_EXT_color_buffer_half_float       1
#define GL_EXT_conservative_depth            1
#define GL_EXT_copy_image                    1
#define GL_EXT_debug_label                   1
#define GL_EXT_debug_marker                  1
#define GL_EXT_discard_framebuffer           1
#define GL_EXT_disjoint_timer_query          1
#define GL_EXT_draw_buffers                  1
#define GL_EXT_draw_buffers_indexed          1
#define GL_EXT_draw_elements_base_vertex     1
#define GL_EXT_draw_instanced                1
#define GL_EXT_draw_transform_feedback       1
#define GL_EXT_external_buffer               1
#define GL_EXT_float_blend                   1
#define GL_EXT_geometry_point_size           1
#define GL_EXT_geometry_shader               1
#define GL_EXT_gpu_shader5                   1
#define GL_EXT_instanced_arrays              1
#define GL_EXT_map_buffer_range              1
#define GL_EXT_memory_object                 1
#define GL_EXT_memory_object_fd              1
#define GL_EXT_memory_object_win32           1
#define GL_EXT_multi_draw_arrays             1
#define GL_EXT_multi_draw_indirect           1
#define GL_EXT_multisampled_compatibility    1
#define GL_EXT_multisampled_render_to_texture 1
#define GL_EXT_multiview_draw_buffers        1
#define GL_EXT_occlusion_query_boolean       1
#define GL_EXT_polygon_offset_clamp          1
#define GL_EXT_post_depth_coverage           1
#define GL_EXT_primitive_bounding_box        1
#define GL_EXT_protected_textures            1
#define GL_EXT_pvrtc_sRGB                    1
#define GL_EXT_raster_multisample            1
#define GL_EXT_read_format_bgra              1
#define GL_EXT_render_snorm                  1
#define GL_EXT_robustness                    1
#define GL_EXT_sRGB                          1
#define GL_EXT_sRGB_write_control            1
#define GL_EXT_semaphore                     1
#define GL_EXT_semaphore_fd                  1
#define GL_EXT_semaphore_win32               1
#define GL_EXT_separate_shader_objects       1
#define GL_EXT_shader_framebuffer_fetch      1
#define GL_EXT_shader_framebuffer_fetch_non_coherent 1
#define GL_EXT_shader_group_vote             1
#define GL_EXT_shader_implicit_conversions   1
#define GL_EXT_shader_integer_mix            1
#define GL_EXT_shader_io_blocks              1
#define GL_EXT_shader_non_constant_global_initializers 1
#define GL_EXT_shader_pixel_local_storage    1
#define GL_EXT_shader_pixel_local_storage2   1
#define GL_EXT_shader_texture_lod            1
#define GL_EXT_shadow_samplers               1
#define GL_EXT_sparse_texture                1
#define GL_EXT_sparse_texture2               1
#define GL_EXT_tessellation_point_size       1
#define GL_EXT_tessellation_shader           1
#define GL_EXT_texture_border_clamp          1
#define GL_EXT_texture_buffer                1
#define GL_EXT_texture_compression_astc_decode_mode 1
#define GL_EXT_texture_compression_bptc      1
#define GL_EXT_texture_compression_dxt1      1
#define GL_EXT_texture_compression_rgtc      1
#define GL_EXT_texture_compression_s3tc      1
#define GL_EXT_texture_compression_s3tc_srgb 1
#define GL_EXT_texture_cube_map_array        1
#define GL_EXT_texture_filter_anisotropic    1
#define GL_EXT_texture_filter_minmax         1
#define GL_EXT_texture_format_BGRA8888       1
#define GL_EXT_texture_format_sRGB_override  1
#define GL_EXT_texture_mirror_clamp_to_edge  1
#define GL_EXT_texture_norm16                1
#define GL_EXT_texture_rg                    1
#define GL_EXT_texture_sRGB_R8               1
#define GL_EXT_texture_sRGB_RG8              1
#define GL_EXT_texture_sRGB_decode           1
#define GL_EXT_texture_storage               1
#define GL_EXT_texture_type_2_10_10_10_REV   1
#define GL_EXT_texture_view                  1
#define GL_EXT_unpack_subimage               1
#define GL_EXT_win32_keyed_mutex             1
#define GL_EXT_window_rectangles             1
#define GL_FACTOR_MAX_AMD                    0x901D
#define GL_FACTOR_MIN_AMD                    0x901C
#define GL_FALSE                             0
#define GL_FASTEST                           0x1101
#define GL_FENCE_CONDITION_NV                0x84F4
#define GL_FENCE_STATUS_NV                   0x84F3
#define GL_FETCH_PER_SAMPLE_ARM              0x8F65
#define GL_FILE_NAME_NV                      0x9074
#define GL_FILL_NV                           0x1B02
#define GL_FILL_RECTANGLE_NV                 0x933C
#define GL_FIRST_TO_REST_NV                  0x90AF
#define GL_FIRST_VERTEX_CONVENTION_EXT       0x8E4D
#define GL_FIRST_VERTEX_CONVENTION_OES       0x8E4D
#define GL_FIXED                             0x140C
#define GL_FJ_shader_binary_GCCSO            1
#define GL_FLOAT                             0x1406
#define GL_FLOAT16_NV                        0x8FF8
#define GL_FLOAT16_VEC2_NV                   0x8FF9
#define GL_FLOAT16_VEC3_NV                   0x8FFA
#define GL_FLOAT16_VEC4_NV                   0x8FFB
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV    0x8DAD
#define GL_FLOAT_MAT2                        0x8B5A
#define GL_FLOAT_MAT2x3                      0x8B65
#define GL_FLOAT_MAT2x3_NV                   0x8B65
#define GL_FLOAT_MAT2x4                      0x8B66
#define GL_FLOAT_MAT2x4_NV                   0x8B66
#define GL_FLOAT_MAT3                        0x8B5B
#define GL_FLOAT_MAT3x2                      0x8B67
#define GL_FLOAT_MAT3x2_NV                   0x8B67
#define GL_FLOAT_MAT3x4                      0x8B68
#define GL_FLOAT_MAT3x4_NV                   0x8B68
#define GL_FLOAT_MAT4                        0x8B5C
#define GL_FLOAT_MAT4x2                      0x8B69
#define GL_FLOAT_MAT4x2_NV                   0x8B69
#define GL_FLOAT_MAT4x3                      0x8B6A
#define GL_FLOAT_MAT4x3_NV                   0x8B6A
#define GL_FLOAT_VEC2                        0x8B50
#define GL_FLOAT_VEC3                        0x8B51
#define GL_FLOAT_VEC4                        0x8B52
#define GL_FONT_ASCENDER_BIT_NV              0x200000
#define GL_FONT_DESCENDER_BIT_NV             0x400000
#define GL_FONT_GLYPHS_AVAILABLE_NV          0x9368
#define GL_FONT_HAS_KERNING_BIT_NV           0x10000000
#define GL_FONT_HEIGHT_BIT_NV                0x800000
#define GL_FONT_MAX_ADVANCE_HEIGHT_BIT_NV    0x2000000
#define GL_FONT_MAX_ADVANCE_WIDTH_BIT_NV     0x1000000
#define GL_FONT_NUM_GLYPH_INDICES_BIT_NV     0x20000000
#define GL_FONT_TARGET_UNAVAILABLE_NV        0x9369
#define GL_FONT_UNAVAILABLE_NV               0x936A
#define GL_FONT_UNDERLINE_POSITION_BIT_NV    0x4000000
#define GL_FONT_UNDERLINE_THICKNESS_BIT_NV   0x8000000
#define GL_FONT_UNINTELLIGIBLE_NV            0x936B
#define GL_FONT_UNITS_PER_EM_BIT_NV          0x100000
#define GL_FONT_X_MAX_BOUNDS_BIT_NV          0x40000
#define GL_FONT_X_MIN_BOUNDS_BIT_NV          0x10000
#define GL_FONT_Y_MAX_BOUNDS_BIT_NV          0x80000
#define GL_FONT_Y_MIN_BOUNDS_BIT_NV          0x20000
#define GL_FOVEATION_ENABLE_BIT_QCOM         1
#define GL_FOVEATION_SCALED_BIN_METHOD_BIT_QCOM 2
#define GL_FRACTIONAL_EVEN_EXT               0x8E7C
#define GL_FRACTIONAL_EVEN_OES               0x8E7C
#define GL_FRACTIONAL_ODD_EXT                0x8E7B
#define GL_FRACTIONAL_ODD_OES                0x8E7B
#define GL_FRAGMENT_COVERAGE_COLOR_NV        0x92DE
#define GL_FRAGMENT_COVERAGE_TO_COLOR_NV     0x92DD
#define GL_FRAGMENT_INPUT_NV                 0x936D
#define GL_FRAGMENT_INTERPOLATION_OFFSET_BITS_OES 0x8E5D
#define GL_FRAGMENT_SHADER                   0x8B30
#define GL_FRAGMENT_SHADER_BIT_EXT           2
#define GL_FRAGMENT_SHADER_DERIVATIVE_HINT   0x8B8B
#define GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES 0x8B8B
#define GL_FRAGMENT_SHADER_DISCARDS_SAMPLES_EXT 0x8A52
#define GL_FRAGMENT_SHADER_FRAMEBUFFER_FETCH_MRT_ARM 0x8F66
#define GL_FRAMEBUFFER                       0x8D40
#define GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE 0x8215
#define GL_FRAMEBUFFER_ATTACHMENT_ANGLE      0x93A3
#define GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE  0x8214
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT 0x8210
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE 0x8211
#define GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT 0x8211
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE 0x8216
#define GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE 0x8213
#define GL_FRAMEBUFFER_ATTACHMENT_LAYERED_EXT 0x8DA7
#define GL_FRAMEBUFFER_ATTACHMENT_LAYERED_OES 0x8DA7
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE   0x8212
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_3D_ZOFFSET_OES 0x8CD4
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR 0x9632
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER 0x8CD4
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR 0x9630
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_SAMPLES_EXT 0x8D6C
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_SCALE_IMG 0x913F
#define GL_FRAMEBUFFER_BINDING               0x8CA6
#define GL_FRAMEBUFFER_COMPLETE              0x8CD5
#define GL_FRAMEBUFFER_DEFAULT               0x8218
#define GL_FRAMEBUFFER_DEFAULT_LAYERS_EXT    0x9312
#define GL_FRAMEBUFFER_DEFAULT_LAYERS_OES    0x9312
#define GL_FRAMEBUFFER_FETCH_NONCOHERENT_QCOM 0x96A2
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS 0x8CD9
#define GL_FRAMEBUFFER_INCOMPLETE_FOVEATION_QCOM 0x8BFF
#define GL_FRAMEBUFFER_INCOMPLETE_INSUFFICIENT_SHADER_COMBINED_LOCAL_STORAGE_EXT 0x9652
#define GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS_EXT 0x8DA8
#define GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS_OES 0x8DA8
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE 0x8D56
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_AND_DOWNSAMPLE_IMG 0x913C
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_ANGLE 0x8D56
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_APPLE 0x8D56
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_EXT 0x8D56
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_IMG 0x9134
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE_NV 0x8D56
#define GL_FRAMEBUFFER_INCOMPLETE_VIEW_TARGETS_OVR 0x9633
#define GL_FRAMEBUFFER_PROGRAMMABLE_SAMPLE_LOCATIONS_NV 0x9342
#define GL_FRAMEBUFFER_SAMPLE_LOCATION_PIXEL_GRID_NV 0x9343
#define GL_FRAMEBUFFER_SRGB                  0x8DB9
#define GL_FRAMEBUFFER_SRGB_EXT              0x8DB9
#define GL_FRAMEBUFFER_UNDEFINED             0x8219
#define GL_FRAMEBUFFER_UNDEFINED_OES         0x8219
#define GL_FRAMEBUFFER_UNSUPPORTED           0x8CDD
#define GL_FRONT                             0x404
#define GL_FRONT_AND_BACK                    0x408
#define GL_FRONT_FACE                        0xB46
#define GL_FUNC_ADD                          0x8006
#define GL_FUNC_REVERSE_SUBTRACT             0x800B
#define GL_FUNC_SUBTRACT                     0x800A
#define GL_GCCSO_SHADER_BINARY_FJ            0x9260
#define GL_GENERATE_MIPMAP_HINT              0x8192
#define GL_GEOMETRY_LINKED_INPUT_TYPE_EXT    0x8917
#define GL_GEOMETRY_LINKED_INPUT_TYPE_OES    0x8917
#define GL_GEOMETRY_LINKED_OUTPUT_TYPE_EXT   0x8918
#define GL_GEOMETRY_LINKED_OUTPUT_TYPE_OES   0x8918
#define GL_GEOMETRY_LINKED_VERTICES_OUT_EXT  0x8916
#define GL_GEOMETRY_LINKED_VERTICES_OUT_OES  0x8916
#define GL_GEOMETRY_SHADER_BIT_EXT           4
#define GL_GEOMETRY_SHADER_BIT_OES           4
#define GL_GEOMETRY_SHADER_EXT               0x8DD9
#define GL_GEOMETRY_SHADER_INVOCATIONS_EXT   0x887F
#define GL_GEOMETRY_SHADER_INVOCATIONS_OES   0x887F
#define GL_GEOMETRY_SHADER_OES               0x8DD9
#define GL_GEQUAL                            0x206
#define GL_GLES_PROTOTYPES                   1
#define GL_GLYPH_HAS_KERNING_BIT_NV          0x100
#define GL_GLYPH_HEIGHT_BIT_NV               2
#define GL_GLYPH_HORIZONTAL_BEARING_ADVANCE_BIT_NV 0x10
#define GL_GLYPH_HORIZONTAL_BEARING_X_BIT_NV 4
#define GL_GLYPH_HORIZONTAL_BEARING_Y_BIT_NV 8
#define GL_GLYPH_VERTICAL_BEARING_ADVANCE_BIT_NV 0x80
#define GL_GLYPH_VERTICAL_BEARING_X_BIT_NV   0x20
#define GL_GLYPH_VERTICAL_BEARING_Y_BIT_NV   0x40
#define GL_GLYPH_WIDTH_BIT_NV                1
#define GL_GPU_DISJOINT_EXT                  0x8FBB
#define GL_GPU_OPTIMIZED_QCOM                0x8FB2
#define GL_GREATER                           0x204
#define GL_GREEN                             0x1904
#define GL_GREEN_BITS                        0xD53
#define GL_GREEN_NV                          0x1904
#define GL_GUILTY_CONTEXT_RESET_EXT          0x8253
#define GL_GUILTY_CONTEXT_RESET_KHR          0x8253
#define GL_HALF_FLOAT                        0x140B
#define GL_HALF_FLOAT_OES                    0x8D61
#define GL_HANDLE_TYPE_D3D11_IMAGE_EXT       0x958B
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT   0x958C
#define GL_HANDLE_TYPE_D3D12_FENCE_EXT       0x9594
#define GL_HANDLE_TYPE_D3D12_RESOURCE_EXT    0x958A
#define GL_HANDLE_TYPE_D3D12_TILEPOOL_EXT    0x9589
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT         0x9586
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT      0x9587
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT  0x9588
#define GL_HARDLIGHT_KHR                     0x929B
#define GL_HARDLIGHT_NV                      0x929B
#define GL_HARDMIX_NV                        0x92A9
#define GL_HIGH_FLOAT                        0x8DF2
#define GL_HIGH_INT                          0x8DF5
#define GL_HORIZONTAL_LINE_TO_NV             6
#define GL_HSL_COLOR_KHR                     0x92AF
#define GL_HSL_COLOR_NV                      0x92AF
#define GL_HSL_HUE_KHR                       0x92AD
#define GL_HSL_HUE_NV                        0x92AD
#define GL_HSL_LUMINOSITY_KHR                0x92B0
#define GL_HSL_LUMINOSITY_NV                 0x92B0
#define GL_HSL_SATURATION_KHR                0x92AE
#define GL_HSL_SATURATION_NV                 0x92AE
#define GL_IMAGE_BUFFER_EXT                  0x9051
#define GL_IMAGE_BUFFER_OES                  0x9051
#define GL_IMAGE_CUBE_MAP_ARRAY_EXT          0x9054
#define GL_IMAGE_CUBE_MAP_ARRAY_OES          0x9054
#define GL_IMG_bindless_texture              1
#define GL_IMG_framebuffer_downsample        1
#define GL_IMG_multisampled_render_to_texture 1
#define GL_IMG_program_binary                1
#define GL_IMG_read_format                   1
#define GL_IMG_shader_binary                 1
#define GL_IMG_texture_compression_pvrtc     1
#define GL_IMG_texture_compression_pvrtc2    1
#define GL_IMG_texture_filter_cubic          1
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT  0x8B9B
#define GL_IMPLEMENTATION_COLOR_READ_TYPE    0x8B9A
#define GL_INCLUSIVE_EXT                     0x8F10
#define GL_INCR                              0x1E02
#define GL_INCR_WRAP                         0x8507
#define GL_INFO_LOG_LENGTH                   0x8B84
#define GL_INNOCENT_CONTEXT_RESET_EXT        0x8254
#define GL_INNOCENT_CONTEXT_RESET_KHR        0x8254
#define GL_INT                               0x1404
#define GL_INT16_NV                          0x8FE4
#define GL_INT16_VEC2_NV                     0x8FE5
#define GL_INT16_VEC3_NV                     0x8FE6
#define GL_INT16_VEC4_NV                     0x8FE7
#define GL_INT64_NV                          0x140E
#define GL_INT64_VEC2_NV                     0x8FE9
#define GL_INT64_VEC3_NV                     0x8FEA
#define GL_INT64_VEC4_NV                     0x8FEB
#define GL_INT8_NV                           0x8FE0
#define GL_INT8_VEC2_NV                      0x8FE1
#define GL_INT8_VEC3_NV                      0x8FE2
#define GL_INT8_VEC4_NV                      0x8FE3
#define GL_INTEL_blackhole_render            1
#define GL_INTEL_conservative_rasterization  1
#define GL_INTEL_framebuffer_CMAA            1
#define GL_INTEL_performance_query           1
#define GL_INTERLEAVED_ATTRIBS               0x8C8C
#define GL_INT_10_10_10_2_OES                0x8DF7
#define GL_INT_2_10_10_10_REV                0x8D9F
#define GL_INT_IMAGE_BUFFER_EXT              0x905C
#define GL_INT_IMAGE_BUFFER_OES              0x905C
#define GL_INT_IMAGE_CUBE_MAP_ARRAY_EXT      0x905F
#define GL_INT_IMAGE_CUBE_MAP_ARRAY_OES      0x905F
#define GL_INT_SAMPLER_2D                    0x8DCA
#define GL_INT_SAMPLER_2D_ARRAY              0x8DCF
#define GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY_OES 0x910C
#define GL_INT_SAMPLER_3D                    0x8DCB
#define GL_INT_SAMPLER_BUFFER_EXT            0x8DD0
#define GL_INT_SAMPLER_BUFFER_OES            0x8DD0
#define GL_INT_SAMPLER_CUBE                  0x8DCC
#define GL_INT_SAMPLER_CUBE_MAP_ARRAY_EXT    0x900E
#define GL_INT_SAMPLER_CUBE_MAP_ARRAY_OES    0x900E
#define GL_INT_VEC2                          0x8B53
#define GL_INT_VEC3                          0x8B54
#define GL_INT_VEC4                          0x8B55
#define GL_INVALID_ENUM                      0x500
#define GL_INVALID_FRAMEBUFFER_OPERATION     0x506
#define GL_INVALID_INDEX                     0xFFFFFFFF
#define GL_INVALID_OPERATION                 0x502
#define GL_INVALID_VALUE                     0x501
#define GL_INVERT                            0x150A
#define GL_INVERT_OVG_NV                     0x92B4
#define GL_INVERT_RGB_NV                     0x92A3
#define GL_ISOLINES_EXT                      0x8E7A
#define GL_ISOLINES_OES                      0x8E7A
#define GL_IS_PER_PATCH_EXT                  0x92E7
#define GL_IS_PER_PATCH_OES                  0x92E7
#define GL_ITALIC_BIT_NV                     2
#define GL_KEEP                              0x1E00
#define GL_KHR_blend_equation_advanced       1
#define GL_KHR_blend_equation_advanced_coherent 1
#define GL_KHR_context_flush_control         1
#define GL_KHR_debug                         1
#define GL_KHR_no_error                      1
#define GL_KHR_parallel_shader_compile       1
#define GL_KHR_robust_buffer_access_behavior 1
#define GL_KHR_robustness                    1
#define GL_KHR_texture_compression_astc_hdr  1
#define GL_KHR_texture_compression_astc_ldr  1
#define GL_KHR_texture_compression_astc_sliced_3d 1
#define GL_LARGE_CCW_ARC_TO_NV               0x16
#define GL_LARGE_CW_ARC_TO_NV                0x18
#define GL_LAST_VERTEX_CONVENTION_EXT        0x8E4E
#define GL_LAST_VERTEX_CONVENTION_OES        0x8E4E
#define GL_LAYER_PROVOKING_VERTEX_EXT        0x825E
#define GL_LAYER_PROVOKING_VERTEX_OES        0x825E
#define GL_LAYOUT_COLOR_ATTACHMENT_EXT       0x958E
#define GL_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_EXT 0x9531
#define GL_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_EXT 0x9530
#define GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT 0x958F
#define GL_LAYOUT_DEPTH_STENCIL_READ_ONLY_EXT 0x9590
#define GL_LAYOUT_GENERAL_EXT                0x958D
#define GL_LAYOUT_SHADER_READ_ONLY_EXT       0x9591
#define GL_LAYOUT_TRANSFER_DST_EXT           0x9593
#define GL_LAYOUT_TRANSFER_SRC_EXT           0x9592
#define GL_LEQUAL                            0x203
#define GL_LESS                              0x201
#define GL_LIGHTEN_KHR                       0x9298
#define GL_LIGHTEN_NV                        0x9298
#define GL_LINEAR                            0x2601
#define GL_LINEARBURN_NV                     0x92A5
#define GL_LINEARDODGE_NV                    0x92A4
#define GL_LINEARLIGHT_NV                    0x92A7
#define GL_LINEAR_MIPMAP_LINEAR              0x2703
#define GL_LINEAR_MIPMAP_NEAREST             0x2701
#define GL_LINEAR_TILING_EXT                 0x9585
#define GL_LINES                             1
#define GL_LINES_ADJACENCY_EXT               10
#define GL_LINES_ADJACENCY_OES               10
#define GL_LINE_LOOP                         2
#define GL_LINE_NV                           0x1B01
#define GL_LINE_STRIP                        3
#define GL_LINE_STRIP_ADJACENCY_EXT          11
#define GL_LINE_STRIP_ADJACENCY_OES          11
#define GL_LINE_TO_NV                        4
#define GL_LINE_WIDTH                        0xB21
#define GL_LINK_STATUS                       0x8B82
#define GL_LOCATION_INDEX_EXT                0x930F
#define GL_LOSE_CONTEXT_ON_RESET_EXT         0x8252
#define GL_LOSE_CONTEXT_ON_RESET_KHR         0x8252
#define GL_LOWER_LEFT_EXT                    0x8CA1
#define GL_LOW_FLOAT                         0x8DF0
#define GL_LOW_INT                           0x8DF3
#define GL_LUID_SIZE_EXT                     8
#define GL_LUMINANCE                         0x1909
#define GL_LUMINANCE16F_EXT                  0x881E
#define GL_LUMINANCE32F_EXT                  0x8818
#define GL_LUMINANCE4_ALPHA4_OES             0x8043
#define GL_LUMINANCE8_ALPHA8_EXT             0x8045
#define GL_LUMINANCE8_ALPHA8_OES             0x8045
#define GL_LUMINANCE8_EXT                    0x8040
#define GL_LUMINANCE8_OES                    0x8040
#define GL_LUMINANCE_ALPHA                   0x190A
#define GL_LUMINANCE_ALPHA16F_EXT            0x881F
#define GL_LUMINANCE_ALPHA32F_EXT            0x8819
#define GL_MAJOR_VERSION                     0x821B
#define GL_MALI_PROGRAM_BINARY_ARM           0x8F61
#define GL_MALI_SHADER_BINARY_ARM            0x8F60
#define GL_MAP_COHERENT_BIT_EXT              0x80
#define GL_MAP_FLUSH_EXPLICIT_BIT            0x10
#define GL_MAP_FLUSH_EXPLICIT_BIT_EXT        0x10
#define GL_MAP_INVALIDATE_BUFFER_BIT         8
#define GL_MAP_INVALIDATE_BUFFER_BIT_EXT     8
#define GL_MAP_INVALIDATE_RANGE_BIT          4
#define GL_MAP_INVALIDATE_RANGE_BIT_EXT      4
#define GL_MAP_PERSISTENT_BIT_EXT            0x40
#define GL_MAP_READ_BIT                      1
#define GL_MAP_READ_BIT_EXT                  1
#define GL_MAP_UNSYNCHRONIZED_BIT            0x20
#define GL_MAP_UNSYNCHRONIZED_BIT_EXT        0x20
#define GL_MAP_WRITE_BIT                     2
#define GL_MAP_WRITE_BIT_EXT                 2
#define GL_MAX                               0x8008
#define GL_MAX_3D_TEXTURE_SIZE               0x8073
#define GL_MAX_3D_TEXTURE_SIZE_OES           0x8073
#define GL_MAX_ARRAY_TEXTURE_LAYERS          0x88FF
#define GL_MAX_CLIP_DISTANCES_APPLE          0xD32
#define GL_MAX_CLIP_DISTANCES_EXT            0xD32
#define GL_MAX_COLOR_ATTACHMENTS             0x8CDF
#define GL_MAX_COLOR_ATTACHMENTS_EXT         0x8CDF
#define GL_MAX_COLOR_ATTACHMENTS_NV          0x8CDF
#define GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES_EXT 0x82FA
#define GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS 0x8A33
#define GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS_EXT 0x8A32
#define GL_MAX_COMBINED_GEOMETRY_UNIFORM_COMPONENTS_OES 0x8A32
#define GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS_EXT 0x8E1E
#define GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS_OES 0x8E1E
#define GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS_EXT 0x8E1F
#define GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS_OES 0x8E1F
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS  0x8B4D
#define GL_MAX_COMBINED_UNIFORM_BLOCKS       0x8A2E
#define GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS 0x8A31
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE         0x851C
#define GL_MAX_CULL_DISTANCES_EXT            0x82F9
#define GL_MAX_DEBUG_GROUP_STACK_DEPTH_KHR   0x826C
#define GL_MAX_DEBUG_LOGGED_MESSAGES_KHR     0x9144
#define GL_MAX_DEBUG_MESSAGE_LENGTH_KHR      0x9143
#define GL_MAX_DRAW_BUFFERS                  0x8824
#define GL_MAX_DRAW_BUFFERS_EXT              0x8824
#define GL_MAX_DRAW_BUFFERS_NV               0x8824
#define GL_MAX_DUAL_SOURCE_DRAW_BUFFERS_EXT  0x88FC
#define GL_MAX_ELEMENTS_INDICES              0x80E9
#define GL_MAX_ELEMENTS_VERTICES             0x80E8
#define GL_MAX_ELEMENT_INDEX                 0x8D6B
#define GL_MAX_EXT                           0x8008
#define GL_MAX_FRAGMENT_INPUT_COMPONENTS     0x9125
#define GL_MAX_FRAGMENT_INTERPOLATION_OFFSET_OES 0x8E5C
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS       0x8A2D
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS   0x8B49
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS      0x8DFD
#define GL_MAX_FRAMEBUFFER_LAYERS_EXT        0x9317
#define GL_MAX_FRAMEBUFFER_LAYERS_OES        0x9317
#define GL_MAX_GEOMETRY_ATOMIC_COUNTERS_EXT  0x92D5
#define GL_MAX_GEOMETRY_ATOMIC_COUNTERS_OES  0x92D5
#define GL_MAX_GEOMETRY_ATOMIC_COUNTER_BUFFERS_EXT 0x92CF
#define GL_MAX_GEOMETRY_ATOMIC_COUNTER_BUFFERS_OES 0x92CF
#define GL_MAX_GEOMETRY_IMAGE_UNIFORMS_EXT   0x90CD
#define GL_MAX_GEOMETRY_IMAGE_UNIFORMS_OES   0x90CD
#define GL_MAX_GEOMETRY_INPUT_COMPONENTS_EXT 0x9123
#define GL_MAX_GEOMETRY_INPUT_COMPONENTS_OES 0x9123
#define GL_MAX_GEOMETRY_OUTPUT_COMPONENTS_EXT 0x9124
#define GL_MAX_GEOMETRY_OUTPUT_COMPONENTS_OES 0x9124
#define GL_MAX_GEOMETRY_OUTPUT_VERTICES_EXT  0x8DE0
#define GL_MAX_GEOMETRY_OUTPUT_VERTICES_OES  0x8DE0
#define GL_MAX_GEOMETRY_SHADER_INVOCATIONS_EXT 0x8E5A
#define GL_MAX_GEOMETRY_SHADER_INVOCATIONS_OES 0x8E5A
#define GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS_EXT 0x90D7
#define GL_MAX_GEOMETRY_SHADER_STORAGE_BLOCKS_OES 0x90D7
#define GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS_EXT 0x8C29
#define GL_MAX_GEOMETRY_TEXTURE_IMAGE_UNITS_OES 0x8C29
#define GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS_EXT 0x8DE1
#define GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS_OES 0x8DE1
#define GL_MAX_GEOMETRY_UNIFORM_BLOCKS_EXT   0x8A2C
#define GL_MAX_GEOMETRY_UNIFORM_BLOCKS_OES   0x8A2C
#define GL_MAX_GEOMETRY_UNIFORM_COMPONENTS_EXT 0x8DDF
#define GL_MAX_GEOMETRY_UNIFORM_COMPONENTS_OES 0x8DDF
#define GL_MAX_LABEL_LENGTH_KHR              0x82E8
#define GL_MAX_MULTIVIEW_BUFFERS_EXT         0x90F2
#define GL_MAX_PATCH_VERTICES_EXT            0x8E7D
#define GL_MAX_PATCH_VERTICES_OES            0x8E7D
#define GL_MAX_PROGRAM_TEXEL_OFFSET          0x8905
#define GL_MAX_RASTER_SAMPLES_EXT            0x9329
#define GL_MAX_RENDERBUFFER_SIZE             0x84E8
#define GL_MAX_SAMPLES                       0x8D57
#define GL_MAX_SAMPLES_ANGLE                 0x8D57
#define GL_MAX_SAMPLES_APPLE                 0x8D57
#define GL_MAX_SAMPLES_EXT                   0x8D57
#define GL_MAX_SAMPLES_IMG                   0x9135
#define GL_MAX_SAMPLES_NV                    0x8D57
#define GL_MAX_SERVER_WAIT_TIMEOUT           0x9111
#define GL_MAX_SERVER_WAIT_TIMEOUT_APPLE     0x9111
#define GL_MAX_SHADER_COMBINED_LOCAL_STORAGE_FAST_SIZE_EXT 0x9650
#define GL_MAX_SHADER_COMBINED_LOCAL_STORAGE_SIZE_EXT 0x9651
#define GL_MAX_SHADER_COMPILER_THREADS_KHR   0x91B0
#define GL_MAX_SHADER_PIXEL_LOCAL_STORAGE_FAST_SIZE_EXT 0x8F63
#define GL_MAX_SHADER_PIXEL_LOCAL_STORAGE_SIZE_EXT 0x8F67
#define GL_MAX_SPARSE_3D_TEXTURE_SIZE_EXT    0x9199
#define GL_MAX_SPARSE_ARRAY_TEXTURE_LAYERS_EXT 0x919A
#define GL_MAX_SPARSE_TEXTURE_SIZE_EXT       0x9198
#define GL_MAX_SUBPIXEL_PRECISION_BIAS_BITS_NV 0x9349
#define GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS_EXT 0x92D3
#define GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS_OES 0x92D3
#define GL_MAX_TESS_CONTROL_ATOMIC_COUNTER_BUFFERS_EXT 0x92CD
#define GL_MAX_TESS_CONTROL_ATOMIC_COUNTER_BUFFERS_OES 0x92CD
#define GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS_EXT 0x90CB
#define GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS_OES 0x90CB
#define GL_MAX_TESS_CONTROL_INPUT_COMPONENTS_EXT 0x886C
#define GL_MAX_TESS_CONTROL_INPUT_COMPONENTS_OES 0x886C
#define GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS_EXT 0x8E83
#define GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS_OES 0x8E83
#define GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS_EXT 0x90D8
#define GL_MAX_TESS_CONTROL_SHADER_STORAGE_BLOCKS_OES 0x90D8
#define GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS_EXT 0x8E81
#define GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS_OES 0x8E81
#define GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS_EXT 0x8E85
#define GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS_OES 0x8E85
#define GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS_EXT 0x8E89
#define GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS_OES 0x8E89
#define GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS_EXT 0x8E7F
#define GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS_OES 0x8E7F
#define GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS_EXT 0x92D4
#define GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS_OES 0x92D4
#define GL_MAX_TESS_EVALUATION_ATOMIC_COUNTER_BUFFERS_EXT 0x92CE
#define GL_MAX_TESS_EVALUATION_ATOMIC_COUNTER_BUFFERS_OES 0x92CE
#define GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS_EXT 0x90CC
#define GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS_OES 0x90CC
#define GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS_EXT 0x886D
#define GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS_OES 0x886D
#define GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS_EXT 0x8E86
#define GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS_OES 0x8E86
#define GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS_EXT 0x90D9
#define GL_MAX_TESS_EVALUATION_SHADER_STORAGE_BLOCKS_OES 0x90D9
#define GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS_EXT 0x8E82
#define GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS_OES 0x8E82
#define GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS_EXT 0x8E8A
#define GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS_OES 0x8E8A
#define GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS_EXT 0x8E80
#define GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS_OES 0x8E80
#define GL_MAX_TESS_GEN_LEVEL_EXT            0x8E7E
#define GL_MAX_TESS_GEN_LEVEL_OES            0x8E7E
#define GL_MAX_TESS_PATCH_COMPONENTS_EXT     0x8E84
#define GL_MAX_TESS_PATCH_COMPONENTS_OES     0x8E84
#define GL_MAX_TEXTURE_BUFFER_SIZE_EXT       0x8C2B
#define GL_MAX_TEXTURE_BUFFER_SIZE_OES       0x8C2B
#define GL_MAX_TEXTURE_IMAGE_UNITS           0x8872
#define GL_MAX_TEXTURE_LOD_BIAS              0x84FD
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT    0x84FF
#define GL_MAX_TEXTURE_SIZE                  0xD33
#define GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS 0x8C8A
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS 0x8C8B
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS 0x8C80
#define GL_MAX_UNIFORM_BLOCK_SIZE            0x8A30
#define GL_MAX_UNIFORM_BUFFER_BINDINGS       0x8A2F
#define GL_MAX_VARYING_COMPONENTS            0x8B4B
#define GL_MAX_VARYING_VECTORS               0x8DFC
#define GL_MAX_VERTEX_ATTRIBS                0x8869
#define GL_MAX_VERTEX_OUTPUT_COMPONENTS      0x9122
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS    0x8B4C
#define GL_MAX_VERTEX_UNIFORM_BLOCKS         0x8A2B
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS     0x8B4A
#define GL_MAX_VERTEX_UNIFORM_VECTORS        0x8DFB
#define GL_MAX_VIEWPORTS_NV                  0x825B
#define GL_MAX_VIEWPORTS_OES                 0x825B
#define GL_MAX_VIEWPORT_DIMS                 0xD3A
#define GL_MAX_VIEWS_OVR                     0x9631
#define GL_MAX_WINDOW_RECTANGLES_EXT         0x8F14
#define GL_MEDIUM_FLOAT                      0x8DF1
#define GL_MEDIUM_INT                        0x8DF4
#define GL_MESA_program_binary_formats       1
#define GL_MESA_shader_integer_functions     1
#define GL_MIN                               0x8007
#define GL_MINOR_VERSION                     0x821C
#define GL_MINUS_CLAMPED_NV                  0x92B3
#define GL_MINUS_NV                          0x929F
#define GL_MIN_EXT                           0x8007
#define GL_MIN_FRAGMENT_INTERPOLATION_OFFSET_OES 0x8E5B
#define GL_MIN_PROGRAM_TEXEL_OFFSET          0x8904
#define GL_MIN_SAMPLE_SHADING_VALUE_OES      0x8C37
#define GL_MIRRORED_REPEAT                   0x8370
#define GL_MIRROR_CLAMP_TO_EDGE_EXT          0x8743
#define GL_MITER_REVERT_NV                   0x90A7
#define GL_MITER_TRUNCATE_NV                 0x90A8
#define GL_MIXED_DEPTH_SAMPLES_SUPPORTED_NV  0x932F
#define GL_MIXED_STENCIL_SAMPLES_SUPPORTED_NV 0x9330
#define GL_MOVE_TO_CONTINUES_NV              0x90B6
#define GL_MOVE_TO_NV                        2
#define GL_MOVE_TO_RESETS_NV                 0x90B5
#define GL_MULTIPLY_KHR                      0x9294
#define GL_MULTIPLY_NV                       0x9294
#define GL_MULTISAMPLES_NV                   0x9371
#define GL_MULTISAMPLE_BUFFER_BIT0_QCOM      0x1000000
#define GL_MULTISAMPLE_BUFFER_BIT1_QCOM      0x2000000
#define GL_MULTISAMPLE_BUFFER_BIT2_QCOM      0x4000000
#define GL_MULTISAMPLE_BUFFER_BIT3_QCOM      0x8000000
#define GL_MULTISAMPLE_BUFFER_BIT4_QCOM      0x10000000
#define GL_MULTISAMPLE_BUFFER_BIT5_QCOM      0x20000000
#define GL_MULTISAMPLE_BUFFER_BIT6_QCOM      0x40000000
#define GL_MULTISAMPLE_BUFFER_BIT7_QCOM      0x80000000
#define GL_MULTISAMPLE_EXT                   0x809D
#define GL_MULTISAMPLE_RASTERIZATION_ALLOWED_EXT 0x932B
#define GL_MULTIVIEW_EXT                     0x90F1
#define GL_NEAREST                           0x2600
#define GL_NEAREST_MIPMAP_LINEAR             0x2702
#define GL_NEAREST_MIPMAP_NEAREST            0x2700
#define GL_NEGATIVE_ONE_TO_ONE_EXT           0x935E
#define GL_NEVER                             0x200
#define GL_NICEST                            0x1102
#define GL_NONE                              0
#define GL_NOTEQUAL                          0x205
#define GL_NO_ERROR                          0
#define GL_NO_RESET_NOTIFICATION_EXT         0x8261
#define GL_NO_RESET_NOTIFICATION_KHR         0x8261
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS    0x86A2
#define GL_NUM_DEVICE_UUIDS_EXT              0x9596
#define GL_NUM_DOWNSAMPLE_SCALES_IMG         0x913D
#define GL_NUM_EXTENSIONS                    0x821D
#define GL_NUM_PROGRAM_BINARY_FORMATS        0x87FE
#define GL_NUM_PROGRAM_BINARY_FORMATS_OES    0x87FE
#define GL_NUM_SAMPLE_COUNTS                 0x9380
#define GL_NUM_SHADER_BINARY_FORMATS         0x8DF9
#define GL_NUM_SPARSE_LEVELS_EXT             0x91AA
#define GL_NUM_TILING_TYPES_EXT              0x9582
#define GL_NUM_VIRTUAL_PAGE_SIZES_EXT        0x91A8
#define GL_NUM_WINDOW_RECTANGLES_EXT         0x8F15
#define GL_NVX_blend_equation_advanced_multi_draw_buffers 1
#define GL_NV_bindless_texture               1
#define GL_NV_blend_equation_advanced        1
#define GL_NV_blend_equation_advanced_coherent 1
#define GL_NV_blend_minmax_factor            1
#define GL_NV_clip_space_w_scaling           1
#define GL_NV_conditional_render             1
#define GL_NV_conservative_raster            1
#define GL_NV_conservative_raster_pre_snap   1
#define GL_NV_conservative_raster_pre_snap_triangles 1
#define GL_NV_copy_buffer                    1
#define GL_NV_coverage_sample                1
#define GL_NV_depth_nonlinear                1
#define GL_NV_draw_buffers                   1
#define GL_NV_draw_instanced                 1
#define GL_NV_draw_vulkan_image              1
#define GL_NV_explicit_attrib_location       1
#define GL_NV_fbo_color_attachments          1
#define GL_NV_fence                          1
#define GL_NV_fill_rectangle                 1
#define GL_NV_fragment_coverage_to_color     1
#define GL_NV_fragment_shader_interlock      1
#define GL_NV_framebuffer_blit               1
#define GL_NV_framebuffer_mixed_samples      1
#define GL_NV_framebuffer_multisample        1
#define GL_NV_generate_mipmap_sRGB           1
#define GL_NV_geometry_shader_passthrough    1
#define GL_NV_gpu_shader5                    1
#define GL_NV_image_formats                  1
#define GL_NV_instanced_arrays               1
#define GL_NV_internalformat_sample_query    1
#define GL_NV_non_square_matrices            1
#define GL_NV_path_rendering                 1
#define GL_NV_path_rendering_shared_edge     1
#define GL_NV_pixel_buffer_object            1
#define GL_NV_polygon_mode                   1
#define GL_NV_read_buffer                    1
#define GL_NV_read_buffer_front              1
#define GL_NV_read_depth                     1
#define GL_NV_read_depth_stencil             1
#define GL_NV_read_stencil                   1
#define GL_NV_sRGB_formats                   1
#define GL_NV_sample_locations               1
#define GL_NV_sample_mask_override_coverage  1
#define GL_NV_shader_atomic_fp16_vector      1
#define GL_NV_shader_noperspective_interpolation 1
#define GL_NV_shadow_samplers_array          1
#define GL_NV_shadow_samplers_cube           1
#define GL_NV_stereo_view_rendering          1
#define GL_NV_texture_border_clamp           1
#define GL_NV_texture_compression_s3tc_update 1
#define GL_NV_texture_npot_2D_mipmap         1
#define GL_NV_viewport_array                 1
#define GL_NV_viewport_array2                1
#define GL_NV_viewport_swizzle               1
#define GL_OBJECT_TYPE                       0x9112
#define GL_OBJECT_TYPE_APPLE                 0x9112
#define GL_OES_EGL_image                     1
#define GL_OES_EGL_image_external            1
#define GL_OES_EGL_image_external_essl3      1
#define GL_OES_compressed_ETC1_RGB8_sub_texture 1
#define GL_OES_compressed_ETC1_RGB8_texture  1
#define GL_OES_compressed_paletted_texture   1
#define GL_OES_copy_image                    1
#define GL_OES_depth24                       1
#define GL_OES_depth32                       1
#define GL_OES_depth_texture                 1
#define GL_OES_draw_buffers_indexed          1
#define GL_OES_draw_elements_base_vertex     1
#define GL_OES_element_index_uint            1
#define GL_OES_fbo_render_mipmap             1
#define GL_OES_fragment_precision_high       1
#define GL_OES_geometry_point_size           1
#define GL_OES_geometry_shader               1
#define GL_OES_get_program_binary            1
#define GL_OES_gpu_shader5                   1
#define GL_OES_mapbuffer                     1
#define GL_OES_packed_depth_stencil          1
#define GL_OES_primitive_bounding_box        1
#define GL_OES_required_internalformat       1
#define GL_OES_rgb8_rgba8                    1
#define GL_OES_sample_shading                1
#define GL_OES_sample_variables              1
#define GL_OES_shader_image_atomic           1
#define GL_OES_shader_io_blocks              1
#define GL_OES_shader_multisample_interpolation 1
#define GL_OES_standard_derivatives          1
#define GL_OES_stencil1                      1
#define GL_OES_stencil4                      1
#define GL_OES_surfaceless_context           1
#define GL_OES_tessellation_point_size       1
#define GL_OES_tessellation_shader           1
#define GL_OES_texture_3D                    1
#define GL_OES_texture_border_clamp          1
#define GL_OES_texture_buffer                1
#define GL_OES_texture_compression_astc      1
#define GL_OES_texture_cube_map_array        1
#define GL_OES_texture_float                 1
#define GL_OES_texture_float_linear          1
#define GL_OES_texture_half_float            1
#define GL_OES_texture_half_float_linear     1
#define GL_OES_texture_npot                  1
#define GL_OES_texture_stencil8              1
#define GL_OES_texture_storage_multisample_2d_array 1
#define GL_OES_texture_view                  1
#define GL_OES_vertex_array_object           1
#define GL_OES_vertex_half_float             1
#define GL_OES_vertex_type_10_10_10_2        1
#define GL_OES_viewport_array                1
#define GL_ONE                               1
#define GL_ONE_MINUS_CONSTANT_ALPHA          0x8004
#define GL_ONE_MINUS_CONSTANT_COLOR          0x8002
#define GL_ONE_MINUS_DST_ALPHA               0x305
#define GL_ONE_MINUS_DST_COLOR               0x307
#define GL_ONE_MINUS_SRC1_ALPHA_EXT          0x88FB
#define GL_ONE_MINUS_SRC1_COLOR_EXT          0x88FA
#define GL_ONE_MINUS_SRC_ALPHA               0x303
#define GL_ONE_MINUS_SRC_COLOR               0x301
#define GL_OPTIMAL_TILING_EXT                0x9584
#define GL_OUT_OF_MEMORY                     0x505
#define GL_OVERLAY_KHR                       0x9296
#define GL_OVERLAY_NV                        0x9296
#define GL_OVR_multiview                     1
#define GL_OVR_multiview2                    1
#define GL_OVR_multiview_multisampled_render_to_texture 1
#define GL_PACK_ALIGNMENT                    0xD05
#define GL_PACK_REVERSE_ROW_ORDER_ANGLE      0x93A4
#define GL_PACK_ROW_LENGTH                   0xD02
#define GL_PACK_SKIP_PIXELS                  0xD04
#define GL_PACK_SKIP_ROWS                    0xD03
#define GL_PALETTE4_R5_G6_B5_OES             0x8B92
#define GL_PALETTE4_RGB5_A1_OES              0x8B94
#define GL_PALETTE4_RGB8_OES                 0x8B90
#define GL_PALETTE4_RGBA4_OES                0x8B93
#define GL_PALETTE4_RGBA8_OES                0x8B91
#define GL_PALETTE8_R5_G6_B5_OES             0x8B97
#define GL_PALETTE8_RGB5_A1_OES              0x8B99
#define GL_PALETTE8_RGB8_OES                 0x8B95
#define GL_PALETTE8_RGBA4_OES                0x8B98
#define GL_PALETTE8_RGBA8_OES                0x8B96
#define GL_PATCHES                           14
#define GL_PATCHES_EXT                       14
#define GL_PATCHES_OES                       14
#define GL_PATCH_VERTICES_EXT                0x8E72
#define GL_PATCH_VERTICES_OES                0x8E72
#define GL_PATH_CLIENT_LENGTH_NV             0x907F
#define GL_PATH_COMMAND_COUNT_NV             0x909D
#define GL_PATH_COMPUTED_LENGTH_NV           0x90A0
#define GL_PATH_COORD_COUNT_NV               0x909E
#define GL_PATH_COVER_DEPTH_FUNC_NV          0x90BF
#define GL_PATH_DASH_ARRAY_COUNT_NV          0x909F
#define GL_PATH_DASH_CAPS_NV                 0x907B
#define GL_PATH_DASH_OFFSET_NV               0x907E
#define GL_PATH_DASH_OFFSET_RESET_NV         0x90B4
#define GL_PATH_END_CAPS_NV                  0x9076
#define GL_PATH_ERROR_POSITION_NV            0x90AB
#define GL_PATH_FILL_BOUNDING_BOX_NV         0x90A1
#define GL_PATH_FILL_COVER_MODE_NV           0x9082
#define GL_PATH_FILL_MASK_NV                 0x9081
#define GL_PATH_FILL_MODE_NV                 0x9080
#define GL_PATH_FORMAT_PS_NV                 0x9071
#define GL_PATH_FORMAT_SVG_NV                0x9070
#define GL_PATH_GEN_COEFF_NV                 0x90B1
#define GL_PATH_GEN_COMPONENTS_NV            0x90B3
#define GL_PATH_GEN_MODE_NV                  0x90B0
#define GL_PATH_INITIAL_DASH_CAP_NV          0x907C
#define GL_PATH_INITIAL_END_CAP_NV           0x9077
#define GL_PATH_JOIN_STYLE_NV                0x9079
#define GL_PATH_MAX_MODELVIEW_STACK_DEPTH_NV 0xD36
#define GL_PATH_MAX_PROJECTION_STACK_DEPTH_NV 0xD38
#define GL_PATH_MITER_LIMIT_NV               0x907A
#define GL_PATH_MODELVIEW_MATRIX_NV          0xBA6
#define GL_PATH_MODELVIEW_NV                 0x1700
#define GL_PATH_MODELVIEW_STACK_DEPTH_NV     0xBA3
#define GL_PATH_OBJECT_BOUNDING_BOX_NV       0x908A
#define GL_PATH_PROJECTION_MATRIX_NV         0xBA7
#define GL_PATH_PROJECTION_NV                0x1701
#define GL_PATH_PROJECTION_STACK_DEPTH_NV    0xBA4
#define GL_PATH_STENCIL_DEPTH_OFFSET_FACTOR_NV 0x90BD
#define GL_PATH_STENCIL_DEPTH_OFFSET_UNITS_NV 0x90BE
#define GL_PATH_STENCIL_FUNC_NV              0x90B7
#define GL_PATH_STENCIL_REF_NV               0x90B8
#define GL_PATH_STENCIL_VALUE_MASK_NV        0x90B9
#define GL_PATH_STROKE_BOUNDING_BOX_NV       0x90A2
#define GL_PATH_STROKE_COVER_MODE_NV         0x9083
#define GL_PATH_STROKE_MASK_NV               0x9084
#define GL_PATH_STROKE_WIDTH_NV              0x9075
#define GL_PATH_TERMINAL_DASH_CAP_NV         0x907D
#define GL_PATH_TERMINAL_END_CAP_NV          0x9078
#define GL_PATH_TRANSPOSE_MODELVIEW_MATRIX_NV 0x84E3
#define GL_PATH_TRANSPOSE_PROJECTION_MATRIX_NV 0x84E4
#define GL_PERCENTAGE_AMD                    0x8BC3
#define GL_PERFMON_GLOBAL_MODE_QCOM          0x8FA0
#define GL_PERFMON_RESULT_AMD                0x8BC6
#define GL_PERFMON_RESULT_AVAILABLE_AMD      0x8BC4
#define GL_PERFMON_RESULT_SIZE_AMD           0x8BC5
#define GL_PERFQUERY_COUNTER_DATA_BOOL32_INTEL 0x94FC
#define GL_PERFQUERY_COUNTER_DATA_DOUBLE_INTEL 0x94FB
#define GL_PERFQUERY_COUNTER_DATA_FLOAT_INTEL 0x94FA
#define GL_PERFQUERY_COUNTER_DATA_UINT32_INTEL 0x94F8
#define GL_PERFQUERY_COUNTER_DATA_UINT64_INTEL 0x94F9
#define GL_PERFQUERY_COUNTER_DESC_LENGTH_MAX_INTEL 0x94FF
#define GL_PERFQUERY_COUNTER_DURATION_NORM_INTEL 0x94F1
#define GL_PERFQUERY_COUNTER_DURATION_RAW_INTEL 0x94F2
#define GL_PERFQUERY_COUNTER_EVENT_INTEL     0x94F0
#define GL_PERFQUERY_COUNTER_NAME_LENGTH_MAX_INTEL 0x94FE
#define GL_PERFQUERY_COUNTER_RAW_INTEL       0x94F4
#define GL_PERFQUERY_COUNTER_THROUGHPUT_INTEL 0x94F3
#define GL_PERFQUERY_COUNTER_TIMESTAMP_INTEL 0x94F5
#define GL_PERFQUERY_DONOT_FLUSH_INTEL       0x83F9
#define GL_PERFQUERY_FLUSH_INTEL             0x83FA
#define GL_PERFQUERY_GLOBAL_CONTEXT_INTEL    1
#define GL_PERFQUERY_GPA_EXTENDED_COUNTERS_INTEL 0x9500
#define GL_PERFQUERY_QUERY_NAME_LENGTH_MAX_INTEL 0x94FD
#define GL_PERFQUERY_SINGLE_CONTEXT_INTEL    0
#define GL_PERFQUERY_WAIT_INTEL              0x83FB
#define GL_PINLIGHT_NV                       0x92A8
#define GL_PIXEL_PACK_BUFFER                 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING         0x88ED
#define GL_PIXEL_PACK_BUFFER_BINDING_NV      0x88ED
#define GL_PIXEL_PACK_BUFFER_NV              0x88EB
#define GL_PIXEL_UNPACK_BUFFER               0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING       0x88EF
#define GL_PIXEL_UNPACK_BUFFER_BINDING_NV    0x88EF
#define GL_PIXEL_UNPACK_BUFFER_NV            0x88EC
#define GL_PLUS_CLAMPED_ALPHA_NV             0x92B2
#define GL_PLUS_CLAMPED_NV                   0x92B1
#define GL_PLUS_DARKER_NV                    0x9292
#define GL_PLUS_NV                           0x9291
#define GL_POINTS                            0
#define GL_POINT_NV                          0x1B00
#define GL_POLYGON_MODE_NV                   0xB40
#define GL_POLYGON_OFFSET_CLAMP_EXT          0x8E1B
#define GL_POLYGON_OFFSET_FACTOR             0x8038
#define GL_POLYGON_OFFSET_FILL               0x8037
#define GL_POLYGON_OFFSET_LINE_NV            0x2A02
#define GL_POLYGON_OFFSET_POINT_NV           0x2A01
#define GL_POLYGON_OFFSET_UNITS              0x2A00
#define GL_PRIMITIVES_GENERATED_EXT          0x8C87
#define GL_PRIMITIVES_GENERATED_OES          0x8C87
#define GL_PRIMITIVE_BOUNDING_BOX_EXT        0x92BE
#define GL_PRIMITIVE_BOUNDING_BOX_OES        0x92BE
#define GL_PRIMITIVE_RESTART_FIXED_INDEX     0x8D69
#define GL_PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED 0x8221
#define GL_PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED_OES 0x8221
#define GL_PROGRAMMABLE_SAMPLE_LOCATION_NV   0x9341
#define GL_PROGRAMMABLE_SAMPLE_LOCATION_TABLE_SIZE_NV 0x9340
#define GL_PROGRAM_BINARY_ANGLE              0x93A6
#define GL_PROGRAM_BINARY_FORMATS            0x87FF
#define GL_PROGRAM_BINARY_FORMATS_OES        0x87FF
#define GL_PROGRAM_BINARY_FORMAT_MESA        0x875F
#define GL_PROGRAM_BINARY_LENGTH             0x8741
#define GL_PROGRAM_BINARY_LENGTH_OES         0x8741
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT   0x8257
#define GL_PROGRAM_KHR                       0x82E2
#define GL_PROGRAM_OBJECT_EXT                0x8B40
#define GL_PROGRAM_PIPELINE_BINDING_EXT      0x825A
#define GL_PROGRAM_PIPELINE_KHR              0x82E4
#define GL_PROGRAM_PIPELINE_OBJECT_EXT       0x8A4F
#define GL_PROGRAM_SEPARABLE_EXT             0x8258
#define GL_PROTECTED_MEMORY_OBJECT_EXT       0x959B
#define GL_QCOM_alpha_test                   1
#define GL_QCOM_binning_control              1
#define GL_QCOM_driver_control               1
#define GL_QCOM_extended_get                 1
#define GL_QCOM_extended_get2                1
#define GL_QCOM_framebuffer_foveated         1
#define GL_QCOM_perfmon_global_mode          1
#define GL_QCOM_shader_framebuffer_fetch_noncoherent 1
#define GL_QCOM_texture_foveated             1
#define GL_QCOM_tiled_rendering              1
#define GL_QCOM_writeonly_rendering          1
#define GL_QUADRATIC_CURVE_TO_NV             10
#define GL_QUADS_EXT                         7
#define GL_QUADS_OES                         7
#define GL_QUERY_BY_REGION_NO_WAIT_NV        0x8E16
#define GL_QUERY_BY_REGION_WAIT_NV           0x8E15
#define GL_QUERY_COUNTER_BITS_EXT            0x8864
#define GL_QUERY_KHR                         0x82E3
#define GL_QUERY_NO_WAIT_NV                  0x8E14
#define GL_QUERY_OBJECT_EXT                  0x9153
#define GL_QUERY_RESULT                      0x8866
#define GL_QUERY_RESULT_AVAILABLE            0x8867
#define GL_QUERY_RESULT_AVAILABLE_EXT        0x8867
#define GL_QUERY_RESULT_EXT                  0x8866
#define GL_QUERY_WAIT_NV                     0x8E13
#define GL_R11F_G11F_B10F                    0x8C3A
#define GL_R11F_G11F_B10F_APPLE              0x8C3A
#define GL_R16F                              0x822D
#define GL_R16F_EXT                          0x822D
#define GL_R16I                              0x8233
#define GL_R16UI                             0x8234
#define GL_R16_EXT                           0x822A
#define GL_R16_SNORM_EXT                     0x8F98
#define GL_R32F                              0x822E
#define GL_R32F_EXT                          0x822E
#define GL_R32I                              0x8235
#define GL_R32UI                             0x8236
#define GL_R8                                0x8229
#define GL_R8I                               0x8231
#define GL_R8UI                              0x8232
#define GL_R8_EXT                            0x8229
#define GL_R8_SNORM                          0x8F94
#define GL_RASTERIZER_DISCARD                0x8C89
#define GL_RASTER_FIXED_SAMPLE_LOCATIONS_EXT 0x932A
#define GL_RASTER_MULTISAMPLE_EXT            0x9327
#define GL_RASTER_SAMPLES_EXT                0x9328
#define GL_READ_BUFFER                       0xC02
#define GL_READ_BUFFER_EXT                   0xC02
#define GL_READ_BUFFER_NV                    0xC02
#define GL_READ_FRAMEBUFFER                  0x8CA8
#define GL_READ_FRAMEBUFFER_ANGLE            0x8CA8
#define GL_READ_FRAMEBUFFER_APPLE            0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING          0x8CAA
#define GL_READ_FRAMEBUFFER_BINDING_ANGLE    0x8CAA
#define GL_READ_FRAMEBUFFER_BINDING_APPLE    0x8CAA
#define GL_READ_FRAMEBUFFER_BINDING_NV       0x8CAA
#define GL_READ_FRAMEBUFFER_NV               0x8CA8
#define GL_RECT_NV                           0xF6
#define GL_RED                               0x1903
#define GL_RED_BITS                          0xD52
#define GL_RED_EXT                           0x1903
#define GL_RED_INTEGER                       0x8D94
#define GL_RED_NV                            0x1903
#define GL_REFERENCED_BY_GEOMETRY_SHADER_EXT 0x9309
#define GL_REFERENCED_BY_GEOMETRY_SHADER_OES 0x9309
#define GL_REFERENCED_BY_TESS_CONTROL_SHADER_EXT 0x9307
#define GL_REFERENCED_BY_TESS_CONTROL_SHADER_OES 0x9307
#define GL_REFERENCED_BY_TESS_EVALUATION_SHADER_EXT 0x9308
#define GL_REFERENCED_BY_TESS_EVALUATION_SHADER_OES 0x9308
#define GL_RELATIVE_ARC_TO_NV                0xFF
#define GL_RELATIVE_CONIC_CURVE_TO_NV        0x1B
#define GL_RELATIVE_CUBIC_CURVE_TO_NV        13
#define GL_RELATIVE_HORIZONTAL_LINE_TO_NV    7
#define GL_RELATIVE_LARGE_CCW_ARC_TO_NV      0x17
#define GL_RELATIVE_LARGE_CW_ARC_TO_NV       0x19
#define GL_RELATIVE_LINE_TO_NV               5
#define GL_RELATIVE_MOVE_TO_NV               3
#define GL_RELATIVE_QUADRATIC_CURVE_TO_NV    11
#define GL_RELATIVE_RECT_NV                  0xF7
#define GL_RELATIVE_ROUNDED_RECT2_NV         0xEB
#define GL_RELATIVE_ROUNDED_RECT4_NV         0xED
#define GL_RELATIVE_ROUNDED_RECT8_NV         0xEF
#define GL_RELATIVE_ROUNDED_RECT_NV          0xE9
#define GL_RELATIVE_SMALL_CCW_ARC_TO_NV      0x13
#define GL_RELATIVE_SMALL_CW_ARC_TO_NV       0x15
#define GL_RELATIVE_SMOOTH_CUBIC_CURVE_TO_NV 0x11
#define GL_RELATIVE_SMOOTH_QUADRATIC_CURVE_TO_NV 15
#define GL_RELATIVE_VERTICAL_LINE_TO_NV      9
#define GL_RENDERBUFFER                      0x8D41
#define GL_RENDERBUFFER_ALPHA_SIZE           0x8D53
#define GL_RENDERBUFFER_BINDING              0x8CA7
#define GL_RENDERBUFFER_BLUE_SIZE            0x8D52
#define GL_RENDERBUFFER_DEPTH_SIZE           0x8D54
#define GL_RENDERBUFFER_GREEN_SIZE           0x8D51
#define GL_RENDERBUFFER_HEIGHT               0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT      0x8D44
#define GL_RENDERBUFFER_RED_SIZE             0x8D50
#define GL_RENDERBUFFER_SAMPLES              0x8CAB
#define GL_RENDERBUFFER_SAMPLES_ANGLE        0x8CAB
#define GL_RENDERBUFFER_SAMPLES_APPLE        0x8CAB
#define GL_RENDERBUFFER_SAMPLES_EXT          0x8CAB
#define GL_RENDERBUFFER_SAMPLES_IMG          0x9133
#define GL_RENDERBUFFER_SAMPLES_NV           0x8CAB
#define GL_RENDERBUFFER_STENCIL_SIZE         0x8D55
#define GL_RENDERBUFFER_WIDTH                0x8D42
#define GL_RENDERER                          0x1F01
#define GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM 0x8FB3
#define GL_REPEAT                            0x2901
#define GL_REPLACE                           0x1E01
#define GL_REQUIRED_TEXTURE_IMAGE_UNITS_OES  0x8D68
#define GL_RESET_NOTIFICATION_STRATEGY_EXT   0x8256
#define GL_RESET_NOTIFICATION_STRATEGY_KHR   0x8256
#define GL_RESTART_PATH_NV                   0xF0
#define GL_RG                                0x8227
#define GL_RG16F                             0x822F
#define GL_RG16F_EXT                         0x822F
#define GL_RG16I                             0x8239
#define GL_RG16UI                            0x823A
#define GL_RG16_EXT                          0x822C
#define GL_RG16_SNORM_EXT                    0x8F99
#define GL_RG32F                             0x8230
#define GL_RG32F_EXT                         0x8230
#define GL_RG32I                             0x823B
#define GL_RG32UI                            0x823C
#define GL_RG8                               0x822B
#define GL_RG8I                              0x8237
#define GL_RG8UI                             0x8238
#define GL_RG8_EXT                           0x822B
#define GL_RG8_SNORM                         0x8F95
#define GL_RGB                               0x1907
#define GL_RGB10_A2                          0x8059
#define GL_RGB10_A2UI                        0x906F
#define GL_RGB10_A2_EXT                      0x8059
#define GL_RGB10_EXT                         0x8052
#define GL_RGB16F                            0x881B
#define GL_RGB16F_EXT                        0x881B
#define GL_RGB16I                            0x8D89
#define GL_RGB16UI                           0x8D77
#define GL_RGB16_EXT                         0x8054
#define GL_RGB16_SNORM_EXT                   0x8F9A
#define GL_RGB32F                            0x8815
#define GL_RGB32F_EXT                        0x8815
#define GL_RGB32I                            0x8D83
#define GL_RGB32UI                           0x8D71
#define GL_RGB565                            0x8D62
#define GL_RGB565_OES                        0x8D62
#define GL_RGB5_A1                           0x8057
#define GL_RGB5_A1_OES                       0x8057
#define GL_RGB8                              0x8051
#define GL_RGB8I                             0x8D8F
#define GL_RGB8UI                            0x8D7D
#define GL_RGB8_OES                          0x8051
#define GL_RGB8_SNORM                        0x8F96
#define GL_RGB9_E5                           0x8C3D
#define GL_RGB9_E5_APPLE                     0x8C3D
#define GL_RGBA                              0x1908
#define GL_RGBA16F                           0x881A
#define GL_RGBA16F_EXT                       0x881A
#define GL_RGBA16I                           0x8D88
#define GL_RGBA16UI                          0x8D76
#define GL_RGBA16_EXT                        0x805B
#define GL_RGBA16_SNORM_EXT                  0x8F9B
#define GL_RGBA32F                           0x8814
#define GL_RGBA32F_EXT                       0x8814
#define GL_RGBA32I                           0x8D82
#define GL_RGBA32UI                          0x8D70
#define GL_RGBA4                             0x8056
#define GL_RGBA4_OES                         0x8056
#define GL_RGBA8                             0x8058
#define GL_RGBA8I                            0x8D8E
#define GL_RGBA8UI                           0x8D7C
#define GL_RGBA8_OES                         0x8058
#define GL_RGBA8_SNORM                       0x8F97
#define GL_RGBA_INTEGER                      0x8D99
#define GL_RGB_422_APPLE                     0x8A1F
#define GL_RGB_INTEGER                       0x8D98
#define GL_RGB_RAW_422_APPLE                 0x8A51
#define GL_RG_EXT                            0x8227
#define GL_RG_INTEGER                        0x8228
#define GL_ROUNDED_RECT2_NV                  0xEA
#define GL_ROUNDED_RECT4_NV                  0xEC
#define GL_ROUNDED_RECT8_NV                  0xEE
#define GL_ROUNDED_RECT_NV                   0xE8
#define GL_ROUND_NV                          0x90A4
#define GL_SAMPLER                           0x82E6
#define GL_SAMPLER_2D                        0x8B5E
#define GL_SAMPLER_2D_ARRAY                  0x8DC1
#define GL_SAMPLER_2D_ARRAY_SHADOW           0x8DC4
#define GL_SAMPLER_2D_ARRAY_SHADOW_NV        0x8DC4
#define GL_SAMPLER_2D_MULTISAMPLE_ARRAY_OES  0x910B
#define GL_SAMPLER_2D_SHADOW                 0x8B62
#define GL_SAMPLER_2D_SHADOW_EXT             0x8B62
#define GL_SAMPLER_3D                        0x8B5F
#define GL_SAMPLER_3D_OES                    0x8B5F
#define GL_SAMPLER_BINDING                   0x8919
#define GL_SAMPLER_BUFFER_EXT                0x8DC2
#define GL_SAMPLER_BUFFER_OES                0x8DC2
#define GL_SAMPLER_CUBE                      0x8B60
#define GL_SAMPLER_CUBE_MAP_ARRAY_EXT        0x900C
#define GL_SAMPLER_CUBE_MAP_ARRAY_OES        0x900C
#define GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW_EXT 0x900D
#define GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW_OES 0x900D
#define GL_SAMPLER_CUBE_SHADOW               0x8DC5
#define GL_SAMPLER_CUBE_SHADOW_NV            0x8DC5
#define GL_SAMPLER_EXTERNAL_2D_Y2Y_EXT       0x8BE7
#define GL_SAMPLER_EXTERNAL_OES              0x8D66
#define GL_SAMPLER_KHR                       0x82E6
#define GL_SAMPLES                           0x80A9
#define GL_SAMPLES_PASSED                    0x8914
#define GL_SAMPLE_ALPHA_TO_COVERAGE          0x809E
#define GL_SAMPLE_ALPHA_TO_ONE_EXT           0x809F
#define GL_SAMPLE_BUFFERS                    0x80A8
#define GL_SAMPLE_COVERAGE                   0x80A0
#define GL_SAMPLE_COVERAGE_INVERT            0x80AB
#define GL_SAMPLE_COVERAGE_VALUE             0x80AA
#define GL_SAMPLE_LOCATION_NV                0x8E50
#define GL_SAMPLE_LOCATION_PIXEL_GRID_HEIGHT_NV 0x933F
#define GL_SAMPLE_LOCATION_PIXEL_GRID_WIDTH_NV 0x933E
#define GL_SAMPLE_LOCATION_SUBPIXEL_BITS_NV  0x933D
#define GL_SAMPLE_SHADING_OES                0x8C36
#define GL_SCISSOR_BOX                       0xC10
#define GL_SCISSOR_TEST                      0xC11
#define GL_SCREEN_KHR                        0x9295
#define GL_SCREEN_NV                         0x9295
#define GL_SEPARATE_ATTRIBS                  0x8C8D
#define GL_SGX_BINARY_IMG                    0x8C0A
#define GL_SGX_PROGRAM_BINARY_IMG            0x9130
#define GL_SHADER_BINARY_DMP                 0x9250
#define GL_SHADER_BINARY_FORMATS             0x8DF8
#define GL_SHADER_BINARY_VIV                 0x8FC4
#define GL_SHADER_COMPILER                   0x8DFA
#define GL_SHADER_KHR                        0x82E1
#define GL_SHADER_OBJECT_EXT                 0x8B48
#define GL_SHADER_PIXEL_LOCAL_STORAGE_EXT    0x8F64
#define GL_SHADER_SOURCE_LENGTH              0x8B88
#define GL_SHADER_TYPE                       0x8B4F
#define GL_SHADING_LANGUAGE_VERSION          0x8B8C
#define GL_SHARED_EDGE_NV                    0xC0
#define GL_SHORT                             0x1402
#define GL_SIGNALED                          0x9119
#define GL_SIGNALED_APPLE                    0x9119
#define GL_SIGNED_NORMALIZED                 0x8F9C
#define GL_SKIP_DECODE_EXT                   0x8A4A
#define GL_SKIP_MISSING_GLYPH_NV             0x90A9
#define GL_SLUMINANCE8_ALPHA8_NV             0x8C45
#define GL_SLUMINANCE8_NV                    0x8C47
#define GL_SLUMINANCE_ALPHA_NV               0x8C44
#define GL_SLUMINANCE_NV                     0x8C46
#define GL_SMALL_CCW_ARC_TO_NV               0x12
#define GL_SMALL_CW_ARC_TO_NV                0x14
#define GL_SMAPHS30_PROGRAM_BINARY_DMP       0x9251
#define GL_SMAPHS_PROGRAM_BINARY_DMP         0x9252
#define GL_SMOOTH_CUBIC_CURVE_TO_NV          0x10
#define GL_SMOOTH_QUADRATIC_CURVE_TO_NV      14
#define GL_SOFTLIGHT_KHR                     0x929C
#define GL_SOFTLIGHT_NV                      0x929C
#define GL_SPARSE_TEXTURE_FULL_ARRAY_CUBE_MIPMAPS_EXT 0x91A9
#define GL_SQUARE_NV                         0x90A3
#define GL_SR8_EXT                           0x8FBD
#define GL_SRC1_ALPHA_EXT                    0x8589
#define GL_SRC1_COLOR_EXT                    0x88F9
#define GL_SRC_ALPHA                         0x302
#define GL_SRC_ALPHA_SATURATE                0x308
#define GL_SRC_ALPHA_SATURATE_EXT            0x308
#define GL_SRC_ATOP_NV                       0x928E
#define GL_SRC_COLOR                         0x300
#define GL_SRC_IN_NV                         0x928A
#define GL_SRC_NV                            0x9286
#define GL_SRC_OUT_NV                        0x928C
#define GL_SRC_OVER_NV                       0x9288
#define GL_SRG8_EXT                          0x8FBE
#define GL_SRGB                              0x8C40
#define GL_SRGB8                             0x8C41
#define GL_SRGB8_ALPHA8                      0x8C43
#define GL_SRGB8_ALPHA8_EXT                  0x8C43
#define GL_SRGB8_NV                          0x8C41
#define GL_SRGB_ALPHA_EXT                    0x8C42
#define GL_SRGB_EXT                          0x8C40
#define GL_STACK_OVERFLOW_KHR                0x503
#define GL_STACK_UNDERFLOW_KHR               0x504
#define GL_STANDARD_FONT_FORMAT_NV           0x936C
#define GL_STANDARD_FONT_NAME_NV             0x9072
#define GL_STATE_RESTORE                     0x8BDC
#define GL_STATIC_COPY                       0x88E6
#define GL_STATIC_DRAW                       0x88E4
#define GL_STATIC_READ                       0x88E5
#define GL_STENCIL                           0x1802
#define GL_STENCIL_ATTACHMENT                0x8D20
#define GL_STENCIL_BACK_FAIL                 0x8801
#define GL_STENCIL_BACK_FUNC                 0x8800
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL      0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS      0x8803
#define GL_STENCIL_BACK_REF                  0x8CA3
#define GL_STENCIL_BACK_VALUE_MASK           0x8CA4
#define GL_STENCIL_BACK_WRITEMASK            0x8CA5
#define GL_STENCIL_BITS                      0xD57
#define GL_STENCIL_BUFFER_BIT                0x400
#define GL_STENCIL_BUFFER_BIT0_QCOM          0x10000
#define GL_STENCIL_BUFFER_BIT1_QCOM          0x20000
#define GL_STENCIL_BUFFER_BIT2_QCOM          0x40000
#define GL_STENCIL_BUFFER_BIT3_QCOM          0x80000
#define GL_STENCIL_BUFFER_BIT4_QCOM          0x100000
#define GL_STENCIL_BUFFER_BIT5_QCOM          0x200000
#define GL_STENCIL_BUFFER_BIT6_QCOM          0x400000
#define GL_STENCIL_BUFFER_BIT7_QCOM          0x800000
#define GL_STENCIL_CLEAR_VALUE               0xB91
#define GL_STENCIL_EXT                       0x1802
#define GL_STENCIL_FAIL                      0xB94
#define GL_STENCIL_FUNC                      0xB92
#define GL_STENCIL_INDEX1_OES                0x8D46
#define GL_STENCIL_INDEX4_OES                0x8D47
#define GL_STENCIL_INDEX8                    0x8D48
#define GL_STENCIL_INDEX8_OES                0x8D48
#define GL_STENCIL_INDEX_OES                 0x1901
#define GL_STENCIL_PASS_DEPTH_FAIL           0xB95
#define GL_STENCIL_PASS_DEPTH_PASS           0xB96
#define GL_STENCIL_REF                       0xB97
#define GL_STENCIL_SAMPLES_NV                0x932E
#define GL_STENCIL_TEST                      0xB90
#define GL_STENCIL_VALUE_MASK                0xB93
#define GL_STENCIL_WRITEMASK                 0xB98
#define GL_STREAM_COPY                       0x88E2
#define GL_STREAM_DRAW                       0x88E0
#define GL_STREAM_READ                       0x88E1
#define GL_SUBPIXEL_BITS                     0xD50
#define GL_SUBPIXEL_PRECISION_BIAS_X_BITS_NV 0x9347
#define GL_SUBPIXEL_PRECISION_BIAS_Y_BITS_NV 0x9348
#define GL_SUPERSAMPLE_SCALE_X_NV            0x9372
#define GL_SUPERSAMPLE_SCALE_Y_NV            0x9373
#define GL_SYNC_CONDITION                    0x9113
#define GL_SYNC_CONDITION_APPLE              0x9113
#define GL_SYNC_FENCE                        0x9116
#define GL_SYNC_FENCE_APPLE                  0x9116
#define GL_SYNC_FLAGS                        0x9115
#define GL_SYNC_FLAGS_APPLE                  0x9115
#define GL_SYNC_FLUSH_COMMANDS_BIT           1
#define GL_SYNC_FLUSH_COMMANDS_BIT_APPLE     1
#define GL_SYNC_GPU_COMMANDS_COMPLETE        0x9117
#define GL_SYNC_GPU_COMMANDS_COMPLETE_APPLE  0x9117
#define GL_SYNC_OBJECT_APPLE                 0x8A53
#define GL_SYNC_STATUS                       0x9114
#define GL_SYNC_STATUS_APPLE                 0x9114
#define GL_SYSTEM_FONT_NAME_NV               0x9073
#define GL_TESS_CONTROL_OUTPUT_VERTICES_EXT  0x8E75
#define GL_TESS_CONTROL_OUTPUT_VERTICES_OES  0x8E75
#define GL_TESS_CONTROL_SHADER_BIT_EXT       8
#define GL_TESS_CONTROL_SHADER_BIT_OES       8
#define GL_TESS_CONTROL_SHADER_EXT           0x8E88
#define GL_TESS_CONTROL_SHADER_OES           0x8E88
#define GL_TESS_EVALUATION_SHADER_BIT_EXT    0x10
#define GL_TESS_EVALUATION_SHADER_BIT_OES    0x10
#define GL_TESS_EVALUATION_SHADER_EXT        0x8E87
#define GL_TESS_EVALUATION_SHADER_OES        0x8E87
#define GL_TESS_GEN_MODE_EXT                 0x8E76
#define GL_TESS_GEN_MODE_OES                 0x8E76
#define GL_TESS_GEN_POINT_MODE_EXT           0x8E79
#define GL_TESS_GEN_POINT_MODE_OES           0x8E79
#define GL_TESS_GEN_SPACING_EXT              0x8E77
#define GL_TESS_GEN_SPACING_OES              0x8E77
#define GL_TESS_GEN_VERTEX_ORDER_EXT         0x8E78
#define GL_TESS_GEN_VERTEX_ORDER_OES         0x8E78
#define GL_TEXTURE                           0x1702
#define GL_TEXTURE0                          0x84C0
#define GL_TEXTURE1                          0x84C1
#define GL_TEXTURE10                         0x84CA
#define GL_TEXTURE11                         0x84CB
#define GL_TEXTURE12                         0x84CC
#define GL_TEXTURE13                         0x84CD
#define GL_TEXTURE14                         0x84CE
#define GL_TEXTURE15                         0x84CF
#define GL_TEXTURE16                         0x84D0
#define GL_TEXTURE17                         0x84D1
#define GL_TEXTURE18                         0x84D2
#define GL_TEXTURE19                         0x84D3
#define GL_TEXTURE2                          0x84C2
#define GL_TEXTURE20                         0x84D4
#define GL_TEXTURE21                         0x84D5
#define GL_TEXTURE22                         0x84D6
#define GL_TEXTURE23                         0x84D7
#define GL_TEXTURE24                         0x84D8
#define GL_TEXTURE25                         0x84D9
#define GL_TEXTURE26                         0x84DA
#define GL_TEXTURE27                         0x84DB
#define GL_TEXTURE28                         0x84DC
#define GL_TEXTURE29                         0x84DD
#define GL_TEXTURE3                          0x84C3
#define GL_TEXTURE30                         0x84DE
#define GL_TEXTURE31                         0x84DF
#define GL_TEXTURE4                          0x84C4
#define GL_TEXTURE5                          0x84C5
#define GL_TEXTURE6                          0x84C6
#define GL_TEXTURE7                          0x84C7
#define GL_TEXTURE8                          0x84C8
#define GL_TEXTURE9                          0x84C9
#define GL_TEXTURE_2D                        0xDE1
#define GL_TEXTURE_2D_ARRAY                  0x8C1A
#define GL_TEXTURE_2D_MULTISAMPLE            0x9100
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY      0x9102
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY_OES  0x9102
#define GL_TEXTURE_3D                        0x806F
#define GL_TEXTURE_3D_OES                    0x806F
#define GL_TEXTURE_ASTC_DECODE_PRECISION_EXT 0x8F69
#define GL_TEXTURE_BASE_LEVEL                0x813C
#define GL_TEXTURE_BINDING_2D                0x8069
#define GL_TEXTURE_BINDING_2D_ARRAY          0x8C1D
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY_OES 0x9105
#define GL_TEXTURE_BINDING_3D                0x806A
#define GL_TEXTURE_BINDING_3D_OES            0x806A
#define GL_TEXTURE_BINDING_BUFFER_EXT        0x8C2C
#define GL_TEXTURE_BINDING_BUFFER_OES        0x8C2C
#define GL_TEXTURE_BINDING_CUBE_MAP          0x8514
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY_EXT 0x900A
#define GL_TEXTURE_BINDING_CUBE_MAP_ARRAY_OES 0x900A
#define GL_TEXTURE_BINDING_EXTERNAL_OES      0x8D67
#define GL_TEXTURE_BORDER_COLOR_EXT          0x1004
#define GL_TEXTURE_BORDER_COLOR_NV           0x1004
#define GL_TEXTURE_BORDER_COLOR_OES          0x1004
#define GL_TEXTURE_BUFFER_BINDING_EXT        0x8C2A
#define GL_TEXTURE_BUFFER_BINDING_OES        0x8C2A
#define GL_TEXTURE_BUFFER_DATA_STORE_BINDING_EXT 0x8C2D
#define GL_TEXTURE_BUFFER_DATA_STORE_BINDING_OES 0x8C2D
#define GL_TEXTURE_BUFFER_EXT                0x8C2A
#define GL_TEXTURE_BUFFER_OES                0x8C2A
#define GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT_EXT 0x919F
#define GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT_OES 0x919F
#define GL_TEXTURE_BUFFER_OFFSET_EXT         0x919D
#define GL_TEXTURE_BUFFER_OFFSET_OES         0x919D
#define GL_TEXTURE_BUFFER_SIZE_EXT           0x919E
#define GL_TEXTURE_BUFFER_SIZE_OES           0x919E
#define GL_TEXTURE_COMPARE_FUNC              0x884D
#define GL_TEXTURE_COMPARE_FUNC_EXT          0x884D
#define GL_TEXTURE_COMPARE_MODE              0x884C
#define GL_TEXTURE_COMPARE_MODE_EXT          0x884C
#define GL_TEXTURE_CUBE_MAP                  0x8513
#define GL_TEXTURE_CUBE_MAP_ARRAY_EXT        0x9009
#define GL_TEXTURE_CUBE_MAP_ARRAY_OES        0x9009
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X       0x8516
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y       0x8518
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z       0x851A
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X       0x8515
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y       0x8517
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z       0x8519
#define GL_TEXTURE_DEPTH_QCOM                0x8BD4
#define GL_TEXTURE_EXTERNAL_OES              0x8D65
#define GL_TEXTURE_FORMAT_QCOM               0x8BD6
#define GL_TEXTURE_FORMAT_SRGB_OVERRIDE_EXT  0x8FBF
#define GL_TEXTURE_FOVEATED_FEATURE_BITS_QCOM 0x8BFB
#define GL_TEXTURE_FOVEATED_FEATURE_QUERY_QCOM 0x8BFD
#define GL_TEXTURE_FOVEATED_MIN_PIXEL_DENSITY_QCOM 0x8BFC
#define GL_TEXTURE_FOVEATED_NUM_FOCAL_POINTS_QUERY_QCOM 0x8BFE
#define GL_TEXTURE_HEIGHT_QCOM               0x8BD3
#define GL_TEXTURE_IMAGE_VALID_QCOM          0x8BD8
#define GL_TEXTURE_IMMUTABLE_FORMAT          0x912F
#define GL_TEXTURE_IMMUTABLE_FORMAT_EXT      0x912F
#define GL_TEXTURE_IMMUTABLE_LEVELS          0x82DF
#define GL_TEXTURE_INTERNAL_FORMAT_QCOM      0x8BD5
#define GL_TEXTURE_MAG_FILTER                0x2800
#define GL_TEXTURE_MAX_ANISOTROPY_EXT        0x84FE
#define GL_TEXTURE_MAX_LEVEL                 0x813D
#define GL_TEXTURE_MAX_LEVEL_APPLE           0x813D
#define GL_TEXTURE_MAX_LOD                   0x813B
#define GL_TEXTURE_MIN_FILTER                0x2801
#define GL_TEXTURE_MIN_LOD                   0x813A
#define GL_TEXTURE_NUM_LEVELS_QCOM           0x8BD9
#define GL_TEXTURE_OBJECT_VALID_QCOM         0x8BDB
#define GL_TEXTURE_PROTECTED_EXT             0x8BFA
#define GL_TEXTURE_REDUCTION_MODE_EXT        0x9366
#define GL_TEXTURE_SAMPLES_IMG               0x9136
#define GL_TEXTURE_SPARSE_EXT                0x91A6
#define GL_TEXTURE_SRGB_DECODE_EXT           0x8A48
#define GL_TEXTURE_SWIZZLE_A                 0x8E45
#define GL_TEXTURE_SWIZZLE_B                 0x8E44
#define GL_TEXTURE_SWIZZLE_G                 0x8E43
#define GL_TEXTURE_SWIZZLE_R                 0x8E42
#define GL_TEXTURE_TARGET_QCOM               0x8BDA
#define GL_TEXTURE_TILING_EXT                0x9580
#define GL_TEXTURE_TYPE_QCOM                 0x8BD7
#define GL_TEXTURE_USAGE_ANGLE               0x93A2
#define GL_TEXTURE_VIEW_MIN_LAYER_EXT        0x82DD
#define GL_TEXTURE_VIEW_MIN_LAYER_OES        0x82DD
#define GL_TEXTURE_VIEW_MIN_LEVEL_EXT        0x82DB
#define GL_TEXTURE_VIEW_MIN_LEVEL_OES        0x82DB
#define GL_TEXTURE_VIEW_NUM_LAYERS_EXT       0x82DE
#define GL_TEXTURE_VIEW_NUM_LAYERS_OES       0x82DE
#define GL_TEXTURE_VIEW_NUM_LEVELS_EXT       0x82DC
#define GL_TEXTURE_VIEW_NUM_LEVELS_OES       0x82DC
#define GL_TEXTURE_WIDTH_QCOM                0x8BD2
#define GL_TEXTURE_WRAP_R                    0x8072
#define GL_TEXTURE_WRAP_R_OES                0x8072
#define GL_TEXTURE_WRAP_S                    0x2802
#define GL_TEXTURE_WRAP_T                    0x2803
#define GL_TILING_TYPES_EXT                  0x9583
#define GL_TIMEOUT_EXPIRED                   0x911B
#define GL_TIMEOUT_EXPIRED_APPLE             0x911B
#define GL_TIMEOUT_IGNORED                   0xFFFFFFFFFFFFFFFF
#define GL_TIMEOUT_IGNORED_APPLE             0xFFFFFFFFFFFFFFFF
#define GL_TIMESTAMP_EXT                     0x8E28
#define GL_TIME_ELAPSED_EXT                  0x88BF
#define GL_TRANSFORM_FEEDBACK                0x8E22
#define GL_TRANSFORM_FEEDBACK_ACTIVE         0x8E24
#define GL_TRANSFORM_FEEDBACK_BINDING        0x8E25
#define GL_TRANSFORM_FEEDBACK_BUFFER         0x8C8E
#define GL_TRANSFORM_FEEDBACK_BUFFER_BINDING 0x8C8F
#define GL_TRANSFORM_FEEDBACK_BUFFER_MODE    0x8C7F
#define GL_TRANSFORM_FEEDBACK_BUFFER_SIZE    0x8C85
#define GL_TRANSFORM_FEEDBACK_BUFFER_START   0x8C84
#define GL_TRANSFORM_FEEDBACK_PAUSED         0x8E23
#define GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN 0x8C88
#define GL_TRANSFORM_FEEDBACK_VARYINGS       0x8C83
#define GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH 0x8C76
#define GL_TRANSLATED_SHADER_SOURCE_LENGTH_ANGLE 0x93A0
#define GL_TRANSLATE_2D_NV                   0x9090
#define GL_TRANSLATE_3D_NV                   0x9091
#define GL_TRANSLATE_X_NV                    0x908E
#define GL_TRANSLATE_Y_NV                    0x908F
#define GL_TRANSPOSE_AFFINE_2D_NV            0x9096
#define GL_TRANSPOSE_AFFINE_3D_NV            0x9098
#define GL_TRIANGLES                         4
#define GL_TRIANGLES_ADJACENCY_EXT           12
#define GL_TRIANGLES_ADJACENCY_OES           12
#define GL_TRIANGLE_FAN                      6
#define GL_TRIANGLE_STRIP                    5
#define GL_TRIANGLE_STRIP_ADJACENCY_EXT      13
#define GL_TRIANGLE_STRIP_ADJACENCY_OES      13
#define GL_TRIANGULAR_NV                     0x90A5
#define GL_TRUE                              1
#define GL_UNCORRELATED_NV                   0x9282
#define GL_UNDEFINED_VERTEX_EXT              0x8260
#define GL_UNDEFINED_VERTEX_OES              0x8260
#define GL_UNIFORM_ARRAY_STRIDE              0x8A3C
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS     0x8A42
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x8A43
#define GL_UNIFORM_BLOCK_BINDING             0x8A3F
#define GL_UNIFORM_BLOCK_DATA_SIZE           0x8A40
#define GL_UNIFORM_BLOCK_INDEX               0x8A3A
#define GL_UNIFORM_BLOCK_NAME_LENGTH         0x8A41
#define GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER 0x8A46
#define GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER 0x8A44
#define GL_UNIFORM_BUFFER                    0x8A11
#define GL_UNIFORM_BUFFER_BINDING            0x8A28
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT   0x8A34
#define GL_UNIFORM_BUFFER_SIZE               0x8A2A
#define GL_UNIFORM_BUFFER_START              0x8A29
#define GL_UNIFORM_IS_ROW_MAJOR              0x8A3E
#define GL_UNIFORM_MATRIX_STRIDE             0x8A3D
#define GL_UNIFORM_NAME_LENGTH               0x8A39
#define GL_UNIFORM_OFFSET                    0x8A3B
#define GL_UNIFORM_SIZE                      0x8A38
#define GL_UNIFORM_TYPE                      0x8A37
#define GL_UNKNOWN_CONTEXT_RESET_EXT         0x8255
#define GL_UNKNOWN_CONTEXT_RESET_KHR         0x8255
#define GL_UNPACK_ALIGNMENT                  0xCF5
#define GL_UNPACK_IMAGE_HEIGHT               0x806E
#define GL_UNPACK_ROW_LENGTH                 0xCF2
#define GL_UNPACK_ROW_LENGTH_EXT             0xCF2
#define GL_UNPACK_SKIP_IMAGES                0x806D
#define GL_UNPACK_SKIP_PIXELS                0xCF4
#define GL_UNPACK_SKIP_PIXELS_EXT            0xCF4
#define GL_UNPACK_SKIP_ROWS                  0xCF3
#define GL_UNPACK_SKIP_ROWS_EXT              0xCF3
#define GL_UNSIGNALED                        0x9118
#define GL_UNSIGNALED_APPLE                  0x9118
#define GL_UNSIGNED_BYTE                     0x1401
#define GL_UNSIGNED_INT                      0x1405
#define GL_UNSIGNED_INT16_NV                 0x8FF0
#define GL_UNSIGNED_INT16_VEC2_NV            0x8FF1
#define GL_UNSIGNED_INT16_VEC3_NV            0x8FF2
#define GL_UNSIGNED_INT16_VEC4_NV            0x8FF3
#define GL_UNSIGNED_INT64_AMD                0x8BC2
#define GL_UNSIGNED_INT64_NV                 0x140F
#define GL_UNSIGNED_INT64_VEC2_NV            0x8FF5
#define GL_UNSIGNED_INT64_VEC3_NV            0x8FF6
#define GL_UNSIGNED_INT64_VEC4_NV            0x8FF7
#define GL_UNSIGNED_INT8_NV                  0x8FEC
#define GL_UNSIGNED_INT8_VEC2_NV             0x8FED
#define GL_UNSIGNED_INT8_VEC3_NV             0x8FEE
#define GL_UNSIGNED_INT8_VEC4_NV             0x8FEF
#define GL_UNSIGNED_INT_10F_11F_11F_REV      0x8C3B
#define GL_UNSIGNED_INT_10F_11F_11F_REV_APPLE 0x8C3B
#define GL_UNSIGNED_INT_10_10_10_2_OES       0x8DF6
#define GL_UNSIGNED_INT_24_8                 0x84FA
#define GL_UNSIGNED_INT_24_8_OES             0x84FA
#define GL_UNSIGNED_INT_2_10_10_10_REV       0x8368
#define GL_UNSIGNED_INT_2_10_10_10_REV_EXT   0x8368
#define GL_UNSIGNED_INT_5_9_9_9_REV          0x8C3E
#define GL_UNSIGNED_INT_5_9_9_9_REV_APPLE    0x8C3E
#define GL_UNSIGNED_INT_IMAGE_BUFFER_EXT     0x9067
#define GL_UNSIGNED_INT_IMAGE_BUFFER_OES     0x9067
#define GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY_EXT 0x906A
#define GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY_OES 0x906A
#define GL_UNSIGNED_INT_SAMPLER_2D           0x8DD2
#define GL_UNSIGNED_INT_SAMPLER_2D_ARRAY     0x8DD7
#define GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY_OES 0x910D
#define GL_UNSIGNED_INT_SAMPLER_3D           0x8DD3
#define GL_UNSIGNED_INT_SAMPLER_BUFFER_EXT   0x8DD8
#define GL_UNSIGNED_INT_SAMPLER_BUFFER_OES   0x8DD8
#define GL_UNSIGNED_INT_SAMPLER_CUBE         0x8DD4
#define GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY_EXT 0x900F
#define GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY_OES 0x900F
#define GL_UNSIGNED_INT_VEC2                 0x8DC6
#define GL_UNSIGNED_INT_VEC3                 0x8DC7
#define GL_UNSIGNED_INT_VEC4                 0x8DC8
#define GL_UNSIGNED_NORMALIZED               0x8C17
#define GL_UNSIGNED_NORMALIZED_EXT           0x8C17
#define GL_UNSIGNED_SHORT                    0x1403
#define GL_UNSIGNED_SHORT_1_5_5_5_REV_EXT    0x8366
#define GL_UNSIGNED_SHORT_4_4_4_4            0x8033
#define GL_UNSIGNED_SHORT_4_4_4_4_REV_EXT    0x8365
#define GL_UNSIGNED_SHORT_4_4_4_4_REV_IMG    0x8365
#define GL_UNSIGNED_SHORT_5_5_5_1            0x8034
#define GL_UNSIGNED_SHORT_5_6_5              0x8363
#define GL_UNSIGNED_SHORT_8_8_APPLE          0x85BA
#define GL_UNSIGNED_SHORT_8_8_REV_APPLE      0x85BB
#define GL_UPPER_LEFT_EXT                    0x8CA2
#define GL_USE_MISSING_GLYPH_NV              0x90AA
#define GL_UTF16_NV                          0x909B
#define GL_UTF8_NV                           0x909A
#define GL_UUID_SIZE_EXT                     0x10
#define GL_VALIDATE_STATUS                   0x8B83
#define GL_VENDOR                            0x1F00
#define GL_VERSION                           0x1F02
#define GL_VERTEX_ARRAY_BINDING              0x85B5
#define GL_VERTEX_ARRAY_BINDING_OES          0x85B5
#define GL_VERTEX_ARRAY_KHR                  0x8074
#define GL_VERTEX_ARRAY_OBJECT_EXT           0x9154
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR       0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE 0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR_EXT   0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR_NV    0x88FE
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED       0x8622
#define GL_VERTEX_ATTRIB_ARRAY_INTEGER       0x88FD
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED    0x886A
#define GL_VERTEX_ATTRIB_ARRAY_POINTER       0x8645
#define GL_VERTEX_ATTRIB_ARRAY_SIZE          0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE        0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE          0x8625
#define GL_VERTEX_SHADER                     0x8B31
#define GL_VERTEX_SHADER_BIT_EXT             1
#define GL_VERTICAL_LINE_TO_NV               8
#define GL_VIEWPORT                          0xBA2
#define GL_VIEWPORT_BOUNDS_RANGE_NV          0x825D
#define GL_VIEWPORT_BOUNDS_RANGE_OES         0x825D
#define GL_VIEWPORT_INDEX_PROVOKING_VERTEX_NV 0x825F
#define GL_VIEWPORT_INDEX_PROVOKING_VERTEX_OES 0x825F
#define GL_VIEWPORT_POSITION_W_SCALE_NV      0x937C
#define GL_VIEWPORT_POSITION_W_SCALE_X_COEFF_NV 0x937D
#define GL_VIEWPORT_POSITION_W_SCALE_Y_COEFF_NV 0x937E
#define GL_VIEWPORT_SUBPIXEL_BITS_NV         0x825C
#define GL_VIEWPORT_SUBPIXEL_BITS_OES        0x825C
#define GL_VIEWPORT_SWIZZLE_NEGATIVE_W_NV    0x9357
#define GL_VIEWPORT_SWIZZLE_NEGATIVE_X_NV    0x9351
#define GL_VIEWPORT_SWIZZLE_NEGATIVE_Y_NV    0x9353
#define GL_VIEWPORT_SWIZZLE_NEGATIVE_Z_NV    0x9355
#define GL_VIEWPORT_SWIZZLE_POSITIVE_W_NV    0x9356
#define GL_VIEWPORT_SWIZZLE_POSITIVE_X_NV    0x9350
#define GL_VIEWPORT_SWIZZLE_POSITIVE_Y_NV    0x9352
#define GL_VIEWPORT_SWIZZLE_POSITIVE_Z_NV    0x9354
#define GL_VIEWPORT_SWIZZLE_W_NV             0x935B
#define GL_VIEWPORT_SWIZZLE_X_NV             0x9358
#define GL_VIEWPORT_SWIZZLE_Y_NV             0x9359
#define GL_VIEWPORT_SWIZZLE_Z_NV             0x935A
#define GL_VIRTUAL_PAGE_SIZE_INDEX_EXT       0x91A7
#define GL_VIRTUAL_PAGE_SIZE_X_EXT           0x9195
#define GL_VIRTUAL_PAGE_SIZE_Y_EXT           0x9196
#define GL_VIRTUAL_PAGE_SIZE_Z_EXT           0x9197
#define GL_VIVIDLIGHT_NV                     0x92A6
#define GL_VIV_shader_binary                 1
#define GL_WAIT_FAILED                       0x911D
#define GL_WAIT_FAILED_APPLE                 0x911D
#define GL_WEIGHTED_AVERAGE_EXT              0x9367
#define GL_WINDOW_RECTANGLE_EXT              0x8F12
#define GL_WINDOW_RECTANGLE_MODE_EXT         0x8F13
#define GL_WRITEONLY_RENDERING_QCOM          0x8823
#define GL_WRITE_ONLY_OES                    0x88B9
#define GL_XOR_NV                            0x1506
#define GL_Z400_BINARY_AMD                   0x8740
#define GL_ZERO                              0
#define GL_ZERO_TO_ONE_EXT                   0x935F
/* ============================================================
 * EGL core types (opaque handles travel as uintptr values)
 * ============================================================ */
typedef void*    EGLDisplay;
typedef void*    EGLSurface;
typedef void*    EGLContext;
typedef void*    EGLConfig;
typedef int32_t  EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;

/* Every plain-integer EGL token is generated below straight from egl.h (see
 * load_constants()). Only the typed ones stay hand-written: their value IS the
 * cast, and a generated `#define EGL_NO_CONTEXT 0` would silently lose it. */
#define EGL_DONT_CARE        ((EGLint)-1)

#define EGL_DEFAULT_DISPLAY  ((EGLDisplay)0)
#define EGL_NO_DISPLAY       ((EGLDisplay)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)

/* ============================================================
 * EGL symbolic constants
 *
 * 164 tokens, parsed out of the NDK headers - nothing in here is
 * hand-maintained, so a guest can reach for every symbolic constant
 * the real headers offer instead of only the ones somebody typed in.
 *
 * Only the integer tokens: the typed ones (EGL_NO_CONTEXT and friends)
 * stay in the type block above, where their cast is part of the value.
 * Both EGL_CONTEXT_CLIENT_VERSION and EGL_CONTEXT_MAJOR_VERSION are
 * 0x3098 - one token under two names, exactly as in egl.h.
 * ============================================================ */
#define EGL_ALPHA_FORMAT                     0x3088
#define EGL_ALPHA_FORMAT_NONPRE              0x308B
#define EGL_ALPHA_FORMAT_PRE                 0x308C
#define EGL_ALPHA_MASK_SIZE                  0x303E
#define EGL_ALPHA_SIZE                       0x3021
#define EGL_BACK_BUFFER                      0x3084
#define EGL_BAD_ACCESS                       0x3002
#define EGL_BAD_ALLOC                        0x3003
#define EGL_BAD_ATTRIBUTE                    0x3004
#define EGL_BAD_CONFIG                       0x3005
#define EGL_BAD_CONTEXT                      0x3006
#define EGL_BAD_CURRENT_SURFACE              0x3007
#define EGL_BAD_DISPLAY                      0x3008
#define EGL_BAD_MATCH                        0x3009
#define EGL_BAD_NATIVE_PIXMAP                0x300A
#define EGL_BAD_NATIVE_WINDOW                0x300B
#define EGL_BAD_PARAMETER                    0x300C
#define EGL_BAD_SURFACE                      0x300D
#define EGL_BIND_TO_TEXTURE_RGB              0x3039
#define EGL_BIND_TO_TEXTURE_RGBA             0x303A
#define EGL_BLUE_SIZE                        0x3022
#define EGL_BUFFER_DESTROYED                 0x3095
#define EGL_BUFFER_PRESERVED                 0x3094
#define EGL_BUFFER_SIZE                      0x3020
#define EGL_CLIENT_APIS                      0x308D
#define EGL_CL_EVENT_HANDLE                  0x309C
#define EGL_COLORSPACE                       0x3087
#define EGL_COLORSPACE_LINEAR                0x308A
#define EGL_COLORSPACE_sRGB                  0x3089
#define EGL_COLOR_BUFFER_TYPE                0x303F
#define EGL_CONDITION_SATISFIED              0x30F6
#define EGL_CONFIG_CAVEAT                    0x3027
#define EGL_CONFIG_ID                        0x3028
#define EGL_CONFORMANT                       0x3042
#define EGL_CONTEXT_CLIENT_TYPE              0x3097
#define EGL_CONTEXT_CLIENT_VERSION           0x3098
#define EGL_CONTEXT_LOST                     0x300E
#define EGL_CONTEXT_MAJOR_VERSION            0x3098
#define EGL_CONTEXT_MINOR_VERSION            0x30FB
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 2
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT  1
#define EGL_CONTEXT_OPENGL_DEBUG             0x31B0
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE 0x31B1
#define EGL_CONTEXT_OPENGL_PROFILE_MASK      0x30FD
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY 0x31BD
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS     0x31B2
#define EGL_CORE_NATIVE_ENGINE               0x305B
#define EGL_DEPTH_SIZE                       0x3025
#define EGL_DISPLAY_SCALING                  0x2710
#define EGL_DRAW                             0x3059
#define EGL_EXTENSIONS                       0x3055
#define EGL_FALSE                            0
#define EGL_FOREVER                          0xFFFFFFFFFFFFFFFF
#define EGL_GL_COLORSPACE                    0x309D
#define EGL_GL_COLORSPACE_LINEAR             0x308A
#define EGL_GL_COLORSPACE_SRGB               0x3089
#define EGL_GL_RENDERBUFFER                  0x30B9
#define EGL_GL_TEXTURE_2D                    0x30B1
#define EGL_GL_TEXTURE_3D                    0x30B2
#define EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_X   0x30B4
#define EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Y   0x30B6
#define EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z   0x30B8
#define EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X   0x30B3
#define EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Y   0x30B5
#define EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Z   0x30B7
#define EGL_GL_TEXTURE_LEVEL                 0x30BC
#define EGL_GL_TEXTURE_ZOFFSET               0x30BD
#define EGL_GREEN_SIZE                       0x3023
#define EGL_HEIGHT                           0x3056
#define EGL_HORIZONTAL_RESOLUTION            0x3090
#define EGL_IMAGE_PRESERVED                  0x30D2
#define EGL_LARGEST_PBUFFER                  0x3058
#define EGL_LEVEL                            0x3029
#define EGL_LOSE_CONTEXT_ON_RESET            0x31BF
#define EGL_LUMINANCE_BUFFER                 0x308F
#define EGL_LUMINANCE_SIZE                   0x303D
#define EGL_MATCH_NATIVE_PIXMAP              0x3041
#define EGL_MAX_PBUFFER_HEIGHT               0x302A
#define EGL_MAX_PBUFFER_PIXELS               0x302B
#define EGL_MAX_PBUFFER_WIDTH                0x302C
#define EGL_MAX_SWAP_INTERVAL                0x303C
#define EGL_MIN_SWAP_INTERVAL                0x303B
#define EGL_MIPMAP_LEVEL                     0x3083
#define EGL_MIPMAP_TEXTURE                   0x3082
#define EGL_MULTISAMPLE_RESOLVE              0x3099
#define EGL_MULTISAMPLE_RESOLVE_BOX          0x309B
#define EGL_MULTISAMPLE_RESOLVE_BOX_BIT      0x200
#define EGL_MULTISAMPLE_RESOLVE_DEFAULT      0x309A
#define EGL_NATIVE_RENDERABLE                0x302D
#define EGL_NATIVE_VISUAL_ID                 0x302E
#define EGL_NATIVE_VISUAL_TYPE               0x302F
#define EGL_NONE                             0x3038
#define EGL_NON_CONFORMANT_CONFIG            0x3051
#define EGL_NOT_INITIALIZED                  0x3001
#define EGL_NO_RESET_NOTIFICATION            0x31BE
#define EGL_NO_TEXTURE                       0x305C
#define EGL_OPENGL_API                       0x30A2
#define EGL_OPENGL_BIT                       8
#define EGL_OPENGL_ES2_BIT                   4
#define EGL_OPENGL_ES3_BIT                   0x40
#define EGL_OPENGL_ES3_BIT_KHR               0x40
#define EGL_OPENGL_ES_API                    0x30A0
#define EGL_OPENGL_ES_BIT                    1
#define EGL_OPENVG_API                       0x30A1
#define EGL_OPENVG_BIT                       2
#define EGL_OPENVG_IMAGE                     0x3096
#define EGL_PBUFFER_BIT                      1
#define EGL_PIXEL_ASPECT_RATIO               0x3092
#define EGL_PIXMAP_BIT                       2
#define EGL_READ                             0x305A
#define EGL_RED_SIZE                         0x3024
#define EGL_RENDERABLE_TYPE                  0x3040
#define EGL_RENDER_BUFFER                    0x3086
#define EGL_RGB_BUFFER                       0x308E
#define EGL_SAMPLES                          0x3031
#define EGL_SAMPLE_BUFFERS                   0x3032
#define EGL_SIGNALED                         0x30F2
#define EGL_SINGLE_BUFFER                    0x3085
#define EGL_SLOW_CONFIG                      0x3050
#define EGL_STENCIL_SIZE                     0x3026
#define EGL_SUCCESS                          0x3000
#define EGL_SURFACE_TYPE                     0x3033
#define EGL_SWAP_BEHAVIOR                    0x3093
#define EGL_SWAP_BEHAVIOR_PRESERVED_BIT      0x400
#define EGL_SYNC_CL_EVENT                    0x30FE
#define EGL_SYNC_CL_EVENT_COMPLETE           0x30FF
#define EGL_SYNC_CONDITION                   0x30F8
#define EGL_SYNC_FENCE                       0x30F9
#define EGL_SYNC_FLUSH_COMMANDS_BIT          1
#define EGL_SYNC_PRIOR_COMMANDS_COMPLETE     0x30F0
#define EGL_SYNC_STATUS                      0x30F1
#define EGL_SYNC_TYPE                        0x30F7
#define EGL_TEXTURE_2D                       0x305F
#define EGL_TEXTURE_FORMAT                   0x3080
#define EGL_TEXTURE_RGB                      0x305D
#define EGL_TEXTURE_RGBA                     0x305E
#define EGL_TEXTURE_TARGET                   0x3081
#define EGL_TIMEOUT_EXPIRED                  0x30F5
#define EGL_TRANSPARENT_BLUE_VALUE           0x3035
#define EGL_TRANSPARENT_GREEN_VALUE          0x3036
#define EGL_TRANSPARENT_RED_VALUE            0x3037
#define EGL_TRANSPARENT_RGB                  0x3052
#define EGL_TRANSPARENT_TYPE                 0x3034
#define EGL_TRUE                             1
#define EGL_UNSIGNALED                       0x30F3
#define EGL_VENDOR                           0x3053
#define EGL_VERSION                          0x3054
#define EGL_VERSION_1_0                      1
#define EGL_VERSION_1_1                      1
#define EGL_VERSION_1_2                      1
#define EGL_VERSION_1_3                      1
#define EGL_VERSION_1_4                      1
#define EGL_VERSION_1_5                      1
#define EGL_VERTICAL_RESOLUTION              0x3091
#define EGL_VG_ALPHA_FORMAT                  0x3088
#define EGL_VG_ALPHA_FORMAT_NONPRE           0x308B
#define EGL_VG_ALPHA_FORMAT_PRE              0x308C
#define EGL_VG_ALPHA_FORMAT_PRE_BIT          0x40
#define EGL_VG_COLORSPACE                    0x3087
#define EGL_VG_COLORSPACE_LINEAR             0x308A
#define EGL_VG_COLORSPACE_LINEAR_BIT         0x20
#define EGL_VG_COLORSPACE_sRGB               0x3089
#define EGL_WIDTH                            0x3057
#define EGL_WINDOW_BIT                       4
/* ============================================================
 * Function IDs (single source of truth)
 * ============================================================ */

#define GL_FN_BASE 1
#define GL_FN_ACTIVETEXTURE 1
#define GL_FN_ATTACHSHADER 2
#define GL_FN_BINDATTRIBLOCATION 3
#define GL_FN_BINDBUFFER 4
#define GL_FN_BINDFRAMEBUFFER 5
#define GL_FN_BINDRENDERBUFFER 6
#define GL_FN_BINDTEXTURE 7
#define GL_FN_BLENDCOLOR 8
#define GL_FN_BLENDEQUATION 9
#define GL_FN_BLENDEQUATIONSEPARATE 10
#define GL_FN_BLENDFUNC 11
#define GL_FN_BLENDFUNCSEPARATE 12
#define GL_FN_BUFFERDATA 13
#define GL_FN_BUFFERSUBDATA 14
#define GL_FN_CHECKFRAMEBUFFERSTATUS 15
#define GL_FN_CLEAR 16
#define GL_FN_CLEARCOLOR 17
#define GL_FN_CLEARDEPTHF 18
#define GL_FN_CLEARSTENCIL 19
#define GL_FN_COLORMASK 20
#define GL_FN_COMPILESHADER 21
#define GL_FN_COMPRESSEDTEXIMAGE2D 22
#define GL_FN_COMPRESSEDTEXSUBIMAGE2D 23
#define GL_FN_COPYTEXIMAGE2D 24
#define GL_FN_COPYTEXSUBIMAGE2D 25
#define GL_FN_CREATEPROGRAM 26
#define GL_FN_CREATESHADER 27
#define GL_FN_CULLFACE 28
#define GL_FN_DELETEBUFFERS 29
#define GL_FN_DELETEFRAMEBUFFERS 30
#define GL_FN_DELETEPROGRAM 31
#define GL_FN_DELETERENDERBUFFERS 32
#define GL_FN_DELETESHADER 33
#define GL_FN_DELETETEXTURES 34
#define GL_FN_DEPTHFUNC 35
#define GL_FN_DEPTHMASK 36
#define GL_FN_DEPTHRANGEF 37
#define GL_FN_DETACHSHADER 38
#define GL_FN_DISABLE 39
#define GL_FN_DISABLEVERTEXATTRIBARRAY 40
#define GL_FN_DRAWARRAYS 41
#define GL_FN_DRAWELEMENTS 42
#define GL_FN_ENABLE 43
#define GL_FN_ENABLEVERTEXATTRIBARRAY 44
#define GL_FN_FINISH 45
#define GL_FN_FLUSH 46
#define GL_FN_FRAMEBUFFERRENDERBUFFER 47
#define GL_FN_FRAMEBUFFERTEXTURE2D 48
#define GL_FN_FRONTFACE 49
#define GL_FN_GENBUFFERS 50
#define GL_FN_GENERATEMIPMAP 51
#define GL_FN_GENFRAMEBUFFERS 52
#define GL_FN_GENRENDERBUFFERS 53
#define GL_FN_GENTEXTURES 54
#define GL_FN_GETACTIVEATTRIB 55
#define GL_FN_GETACTIVEUNIFORM 56
#define GL_FN_GETATTACHEDSHADERS 57
#define GL_FN_GETATTRIBLOCATION 58
#define GL_FN_GETBOOLEANV 59
#define GL_FN_GETBUFFERPARAMETERIV 60
#define GL_FN_GETERROR 61
#define GL_FN_GETFLOATV 62
#define GL_FN_GETFRAMEBUFFERATTACHMENTPARAMETERIV 63
#define GL_FN_GETINTEGERV 64
#define GL_FN_GETPROGRAMIV 65
#define GL_FN_GETPROGRAMINFOLOG 66
#define GL_FN_GETRENDERBUFFERPARAMETERIV 67
#define GL_FN_GETSHADERIV 68
#define GL_FN_GETSHADERINFOLOG 69
#define GL_FN_GETSHADERPRECISIONFORMAT 70
#define GL_FN_GETSHADERSOURCE 71
#define GL_FN_GETSTRING 72
#define GL_FN_GETTEXPARAMETERFV 73
#define GL_FN_GETTEXPARAMETERIV 74
#define GL_FN_GETUNIFORMFV 75
#define GL_FN_GETUNIFORMIV 76
#define GL_FN_GETUNIFORMLOCATION 77
#define GL_FN_GETVERTEXATTRIBFV 78
#define GL_FN_GETVERTEXATTRIBIV 79
#define GL_FN_GETVERTEXATTRIBPOINTERV 80
#define GL_FN_HINT 81
#define GL_FN_ISBUFFER 82
#define GL_FN_ISENABLED 83
#define GL_FN_ISFRAMEBUFFER 84
#define GL_FN_ISPROGRAM 85
#define GL_FN_ISRENDERBUFFER 86
#define GL_FN_ISSHADER 87
#define GL_FN_ISTEXTURE 88
#define GL_FN_LINEWIDTH 89
#define GL_FN_LINKPROGRAM 90
#define GL_FN_PIXELSTOREI 91
#define GL_FN_POLYGONOFFSET 92
#define GL_FN_READPIXELS 93
#define GL_FN_RELEASESHADERCOMPILER 94
#define GL_FN_RENDERBUFFERSTORAGE 95
#define GL_FN_SAMPLECOVERAGE 96
#define GL_FN_SCISSOR 97
#define GL_FN_SHADERBINARY 98
#define GL_FN_SHADERSOURCE 99
#define GL_FN_STENCILFUNC 100
#define GL_FN_STENCILFUNCSEPARATE 101
#define GL_FN_STENCILMASK 102
#define GL_FN_STENCILMASKSEPARATE 103
#define GL_FN_STENCILOP 104
#define GL_FN_STENCILOPSEPARATE 105
#define GL_FN_TEXIMAGE2D 106
#define GL_FN_TEXPARAMETERF 107
#define GL_FN_TEXPARAMETERFV 108
#define GL_FN_TEXPARAMETERI 109
#define GL_FN_TEXPARAMETERIV 110
#define GL_FN_TEXSUBIMAGE2D 111
#define GL_FN_UNIFORM1F 112
#define GL_FN_UNIFORM1FV 113
#define GL_FN_UNIFORM1I 114
#define GL_FN_UNIFORM1IV 115
#define GL_FN_UNIFORM2F 116
#define GL_FN_UNIFORM2FV 117
#define GL_FN_UNIFORM2I 118
#define GL_FN_UNIFORM2IV 119
#define GL_FN_UNIFORM3F 120
#define GL_FN_UNIFORM3FV 121
#define GL_FN_UNIFORM3I 122
#define GL_FN_UNIFORM3IV 123
#define GL_FN_UNIFORM4F 124
#define GL_FN_UNIFORM4FV 125
#define GL_FN_UNIFORM4I 126
#define GL_FN_UNIFORM4IV 127
#define GL_FN_UNIFORMMATRIX2FV 128
#define GL_FN_UNIFORMMATRIX3FV 129
#define GL_FN_UNIFORMMATRIX4FV 130
#define GL_FN_USEPROGRAM 131
#define GL_FN_VALIDATEPROGRAM 132
#define GL_FN_VERTEXATTRIB1F 133
#define GL_FN_VERTEXATTRIB1FV 134
#define GL_FN_VERTEXATTRIB2F 135
#define GL_FN_VERTEXATTRIB2FV 136
#define GL_FN_VERTEXATTRIB3F 137
#define GL_FN_VERTEXATTRIB3FV 138
#define GL_FN_VERTEXATTRIB4F 139
#define GL_FN_VERTEXATTRIB4FV 140
#define GL_FN_VERTEXATTRIBPOINTER 141
#define GL_FN_VIEWPORT 142
#define GL_FN_READBUFFER 143
#define GL_FN_DRAWRANGEELEMENTS 144
#define GL_FN_TEXIMAGE3D 145
#define GL_FN_TEXSUBIMAGE3D 146
#define GL_FN_COPYTEXSUBIMAGE3D 147
#define GL_FN_COMPRESSEDTEXIMAGE3D 148
#define GL_FN_COMPRESSEDTEXSUBIMAGE3D 149
#define GL_FN_GENQUERIES 150
#define GL_FN_DELETEQUERIES 151
#define GL_FN_ISQUERY 152
#define GL_FN_BEGINQUERY 153
#define GL_FN_ENDQUERY 154
#define GL_FN_GETQUERYIV 155
#define GL_FN_GETQUERYOBJECTUIV 156
#define GL_FN_UNMAPBUFFER 157
#define GL_FN_GETBUFFERPOINTERV 158
#define GL_FN_DRAWBUFFERS 159
#define GL_FN_UNIFORMMATRIX2X3FV 160
#define GL_FN_UNIFORMMATRIX3X2FV 161
#define GL_FN_UNIFORMMATRIX2X4FV 162
#define GL_FN_UNIFORMMATRIX4X2FV 163
#define GL_FN_UNIFORMMATRIX3X4FV 164
#define GL_FN_UNIFORMMATRIX4X3FV 165
#define GL_FN_BLITFRAMEBUFFER 166
#define GL_FN_RENDERBUFFERSTORAGEMULTISAMPLE 167
#define GL_FN_FRAMEBUFFERTEXTURELAYER 168
#define GL_FN_MAPBUFFERRANGE 169
#define GL_FN_FLUSHMAPPEDBUFFERRANGE 170
#define GL_FN_BINDVERTEXARRAY 171
#define GL_FN_DELETEVERTEXARRAYS 172
#define GL_FN_GENVERTEXARRAYS 173
#define GL_FN_ISVERTEXARRAY 174
#define GL_FN_GETINTEGERI_V 175
#define GL_FN_BEGINTRANSFORMFEEDBACK 176
#define GL_FN_ENDTRANSFORMFEEDBACK 177
#define GL_FN_BINDBUFFERRANGE 178
#define GL_FN_BINDBUFFERBASE 179
#define GL_FN_TRANSFORMFEEDBACKVARYINGS 180
#define GL_FN_GETTRANSFORMFEEDBACKVARYING 181
#define GL_FN_VERTEXATTRIBIPOINTER 182
#define GL_FN_GETVERTEXATTRIBIIV 183
#define GL_FN_GETVERTEXATTRIBIUIV 184
#define GL_FN_VERTEXATTRIBI4I 185
#define GL_FN_VERTEXATTRIBI4UI 186
#define GL_FN_VERTEXATTRIBI4IV 187
#define GL_FN_VERTEXATTRIBI4UIV 188
#define GL_FN_GETUNIFORMUIV 189
#define GL_FN_GETFRAGDATALOCATION 190
#define GL_FN_UNIFORM1UI 191
#define GL_FN_UNIFORM2UI 192
#define GL_FN_UNIFORM3UI 193
#define GL_FN_UNIFORM4UI 194
#define GL_FN_UNIFORM1UIV 195
#define GL_FN_UNIFORM2UIV 196
#define GL_FN_UNIFORM3UIV 197
#define GL_FN_UNIFORM4UIV 198
#define GL_FN_CLEARBUFFERIV 199
#define GL_FN_CLEARBUFFERUIV 200
#define GL_FN_CLEARBUFFERFV 201
#define GL_FN_CLEARBUFFERFI 202
#define GL_FN_GETSTRINGI 203
#define GL_FN_COPYBUFFERSUBDATA 204
#define GL_FN_GETUNIFORMINDICES 205
#define GL_FN_GETACTIVEUNIFORMSIV 206
#define GL_FN_GETUNIFORMBLOCKINDEX 207
#define GL_FN_GETACTIVEUNIFORMBLOCKIV 208
#define GL_FN_GETACTIVEUNIFORMBLOCKNAME 209
#define GL_FN_UNIFORMBLOCKBINDING 210
#define GL_FN_DRAWARRAYSINSTANCED 211
#define GL_FN_DRAWELEMENTSINSTANCED 212
#define GL_FN_FENCESYNC 213
#define GL_FN_ISSYNC 214
#define GL_FN_DELETESYNC 215
#define GL_FN_CLIENTWAITSYNC 216
#define GL_FN_WAITSYNC 217
#define GL_FN_GETINTEGER64V 218
#define GL_FN_GETSYNCIV 219
#define GL_FN_GETINTEGER64I_V 220
#define GL_FN_GETBUFFERPARAMETERI64V 221
#define GL_FN_GENSAMPLERS 222
#define GL_FN_DELETESAMPLERS 223
#define GL_FN_ISSAMPLER 224
#define GL_FN_BINDSAMPLER 225
#define GL_FN_SAMPLERPARAMETERI 226
#define GL_FN_SAMPLERPARAMETERIV 227
#define GL_FN_SAMPLERPARAMETERF 228
#define GL_FN_SAMPLERPARAMETERFV 229
#define GL_FN_GETSAMPLERPARAMETERIV 230
#define GL_FN_GETSAMPLERPARAMETERFV 231
#define GL_FN_VERTEXATTRIBDIVISOR 232
#define GL_FN_BINDTRANSFORMFEEDBACK 233
#define GL_FN_DELETETRANSFORMFEEDBACKS 234
#define GL_FN_GENTRANSFORMFEEDBACKS 235
#define GL_FN_ISTRANSFORMFEEDBACK 236
#define GL_FN_PAUSETRANSFORMFEEDBACK 237
#define GL_FN_RESUMETRANSFORMFEEDBACK 238
#define GL_FN_GETPROGRAMBINARY 239
#define GL_FN_PROGRAMBINARY 240
#define GL_FN_PROGRAMPARAMETERI 241
#define GL_FN_INVALIDATEFRAMEBUFFER 242
#define GL_FN_INVALIDATESUBFRAMEBUFFER 243
#define GL_FN_TEXSTORAGE2D 244
#define GL_FN_TEXSTORAGE3D 245
#define GL_FN_GETINTERNALFORMATIV 246

#define EGL_FN_BASE 0x100
#define EGL_FN_CHOOSECONFIG 0x100
#define EGL_FN_CREATECONTEXT 0x101
#define EGL_FN_CREATEPBUFFERSURFACE 0x102
#define EGL_FN_CREATEWINDOWSURFACE 0x103
#define EGL_FN_DESTROYCONTEXT 0x104
#define EGL_FN_DESTROYSURFACE 0x105
#define EGL_FN_GETCONFIGATTRIB 0x106
#define EGL_FN_GETCURRENTDISPLAY 0x107
#define EGL_FN_GETCURRENTSURFACE 0x108
#define EGL_FN_GETDISPLAY 0x109
#define EGL_FN_GETERROR 0x10A
#define EGL_FN_INITIALIZE 0x10B
#define EGL_FN_MAKECURRENT 0x10C
#define EGL_FN_QUERYCONTEXT 0x10D
#define EGL_FN_QUERYSTRING 0x10E
#define EGL_FN_QUERYSURFACE 0x10F
#define EGL_FN_SWAPBUFFERS 0x110
#define EGL_FN_TERMINATE 0x111
#define EGL_FN_SWAPINTERVAL 0x112
#define EGL_FN_BINDAPI 0x113
#define EGL_FN_GETCURRENTCONTEXT 0x114

#define GL_CALL_MAX_ARGS 12
/* gl_call.args[] slot carrying the guest scratch buffer for the
 * calls returning a host-owned string (glGetString/glGetStringi/
 * eglQueryString). GL_CALL_MAX_ARGS is one wider than the widest real
 * call (glTexSubImage3D, 11), so this slot can never collide with a
 * parameter. The buffer must outlive the call: the stub owns it
 * statically. */
#define GL_CALL_RETBUF_SLOT (GL_CALL_MAX_ARGS - 1)
#define GL_CALL_RETBUF_CAP  8192

/* The marshalled-call syscall numbers (SYS_GL_CALL / SYS_EGL_CALL) come
 * from the shared ABI header, together with every other hypercall. */
#include "virtpass/vp_syscall.h"
/* ============================================================
 * Marshalling struct (guest fills, host consumes)
 *
 * Guest allocates gl_call on its stack and passes its address in a0.
 * Pointers inside args[] are GUEST addresses: guest memory is no longer
 * mapped into the host, so the backend must run each data pointer through
 * rvvm_user_guest_ptr() before dereferencing it. Opaque host values
 * (EGLDisplay/EGLConfig/EGLSurface/EGLContext and the EGLNative* types) are
 * only passed back by the guest, never dereferenced, so they pass through.
 *
 * args[GL_CALL_RETBUF_SLOT] holds the address of a guest scratch buffer
 * (GL_CALL_RETBUF_CAP bytes) for calls that hand back a host-owned string
 * (glGetString/glGetStringi/eglQueryString): the host copies the string there
 * and answers with that guest address. The stub keeps it in a static, not on
 * its stack - the pointer outlives the call. Only the string-returning calls
 * use the slot (glGetStringi passes its index in args[1]); everywhere else it
 * is unused, and nothing can reach it as a parameter because it sits one slot
 * above the widest call.
 * ============================================================ */
typedef struct {
    uint32_t fn_id;                    /* GL_FN_* / EGL_FN_*            */
    uint32_t nargs;                    /* number of real args[] slots   */
    int64_t  ret;                      /* host writes the return value  */
    int64_t  args[GL_CALL_MAX_ARGS];   /* 32-bit ints are zero/sign
                                        * extended; floats bit-packed;
                                        * data pointers as guest VA;
                                        * handles as opaque host values;
                                        * last slot = string scratch     */
} gl_call;

/* ============================================================
 * Guest-facing API (same signatures as the NDK headers)
 * ============================================================ */

uint32_t eglChooseConfig(void* dpy, const int32_t* attrib_list, void* configs, int32_t config_size, int32_t* num_config);
void* eglCreateContext(void* dpy, void* config, void* share_context, const int32_t* attrib_list);
void* eglCreatePbufferSurface(void* dpy, void* config, const int32_t* attrib_list);
void* eglCreateWindowSurface(void* dpy, void* config, void* win, const int32_t* attrib_list);
uint32_t eglDestroyContext(void* dpy, void* ctx);
uint32_t eglDestroySurface(void* dpy, void* surface);
uint32_t eglGetConfigAttrib(void* dpy, void* config, int32_t attribute, int32_t* value);
void* eglGetCurrentDisplay(void);
void* eglGetCurrentSurface(int32_t readdraw);
void* eglGetDisplay(void* display_id);
int32_t eglGetError(void);
void* eglGetProcAddress(const char* procname);
uint32_t eglInitialize(void* dpy, int32_t* major, int32_t* minor);
uint32_t eglMakeCurrent(void* dpy, void* draw, void* read, void* ctx);
uint32_t eglQueryContext(void* dpy, void* ctx, int32_t attribute, int32_t* value);
const char* eglQueryString(void* dpy, int32_t name);
uint32_t eglQuerySurface(void* dpy, void* surface, int32_t attribute, int32_t* value);
uint32_t eglSwapBuffers(void* dpy, void* surface);
uint32_t eglTerminate(void* dpy);
uint32_t eglSwapInterval(void* dpy, int32_t interval);
uint32_t eglBindAPI(uint32_t api);
void* eglGetCurrentContext(void);

void glActiveTexture(uint32_t texture);
void glAttachShader(uint32_t program, uint32_t shader);
void glBindAttribLocation(uint32_t program, uint32_t index, const char* name);
void glBindBuffer(uint32_t target, uint32_t buffer);
void glBindFramebuffer(uint32_t target, uint32_t framebuffer);
void glBindRenderbuffer(uint32_t target, uint32_t renderbuffer);
void glBindTexture(uint32_t target, uint32_t texture);
void glBlendColor(float red, float green, float blue, float alpha);
void glBlendEquation(uint32_t mode);
void glBlendEquationSeparate(uint32_t modeRGB, uint32_t modeAlpha);
void glBlendFunc(uint32_t sfactor, uint32_t dfactor);
void glBlendFuncSeparate(uint32_t sfactorRGB, uint32_t dfactorRGB, uint32_t sfactorAlpha, uint32_t dfactorAlpha);
void glBufferData(uint32_t target, long size, const void* data, uint32_t usage);
void glBufferSubData(uint32_t target, long offset, long size, const void* data);
uint32_t glCheckFramebufferStatus(uint32_t target);
void glClear(uint32_t mask);
void glClearColor(float red, float green, float blue, float alpha);
void glClearDepthf(float d);
void glClearStencil(int32_t s);
void glColorMask(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
void glCompileShader(uint32_t shader);
void glCompressedTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t border, int32_t imageSize, const void* data);
void glCompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, int32_t imageSize, const void* data);
void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y, int32_t width, int32_t height, int32_t border);
void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x, int32_t y, int32_t width, int32_t height);
uint32_t glCreateProgram(void);
uint32_t glCreateShader(uint32_t type);
void glCullFace(uint32_t mode);
void glDeleteBuffers(int32_t n, const uint32_t* buffers);
void glDeleteFramebuffers(int32_t n, const uint32_t* framebuffers);
void glDeleteProgram(uint32_t program);
void glDeleteRenderbuffers(int32_t n, const uint32_t* renderbuffers);
void glDeleteShader(uint32_t shader);
void glDeleteTextures(int32_t n, const uint32_t* textures);
void glDepthFunc(uint32_t func);
void glDepthMask(uint8_t flag);
void glDepthRangef(float n, float f);
void glDetachShader(uint32_t program, uint32_t shader);
void glDisable(uint32_t cap);
void glDisableVertexAttribArray(uint32_t index);
void glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void glDrawElements(uint32_t mode, int32_t count, uint32_t type, const void* indices);
void glEnable(uint32_t cap);
void glEnableVertexAttribArray(uint32_t index);
void glFinish(void);
void glFlush(void);
void glFramebufferRenderbuffer(uint32_t target, uint32_t attachment, uint32_t renderbuffertarget, uint32_t renderbuffer);
void glFramebufferTexture2D(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level);
void glFrontFace(uint32_t mode);
void glGenBuffers(int32_t n, uint32_t* buffers);
void glGenerateMipmap(uint32_t target);
void glGenFramebuffers(int32_t n, uint32_t* framebuffers);
void glGenRenderbuffers(int32_t n, uint32_t* renderbuffers);
void glGenTextures(int32_t n, uint32_t* textures);
void glGetActiveAttrib(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glGetActiveUniform(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glGetAttachedShaders(uint32_t program, int32_t maxCount, int32_t* count, uint32_t* shaders);
int32_t glGetAttribLocation(uint32_t program, const char* name);
void glGetBooleanv(uint32_t pname, uint8_t* data);
void glGetBufferParameteriv(uint32_t target, uint32_t pname, int32_t* params);
uint32_t glGetError(void);
void glGetFloatv(uint32_t pname, float* data);
void glGetFramebufferAttachmentParameteriv(uint32_t target, uint32_t attachment, uint32_t pname, int32_t* params);
void glGetIntegerv(uint32_t pname, int32_t* data);
void glGetProgramiv(uint32_t program, uint32_t pname, int32_t* params);
void glGetProgramInfoLog(uint32_t program, int32_t bufSize, int32_t* length, char* infoLog);
void glGetRenderbufferParameteriv(uint32_t target, uint32_t pname, int32_t* params);
void glGetShaderiv(uint32_t shader, uint32_t pname, int32_t* params);
void glGetShaderInfoLog(uint32_t shader, int32_t bufSize, int32_t* length, char* infoLog);
void glGetShaderPrecisionFormat(uint32_t shadertype, uint32_t precisiontype, int32_t* range, int32_t* precision);
void glGetShaderSource(uint32_t shader, int32_t bufSize, int32_t* length, char* source);
const uint8_t* glGetString(uint32_t name);
void glGetTexParameterfv(uint32_t target, uint32_t pname, float* params);
void glGetTexParameteriv(uint32_t target, uint32_t pname, int32_t* params);
void glGetUniformfv(uint32_t program, int32_t location, float* params);
void glGetUniformiv(uint32_t program, int32_t location, int32_t* params);
int32_t glGetUniformLocation(uint32_t program, const char* name);
void glGetVertexAttribfv(uint32_t index, uint32_t pname, float* params);
void glGetVertexAttribiv(uint32_t index, uint32_t pname, int32_t* params);
void glGetVertexAttribPointerv(uint32_t index, uint32_t pname, void** pointer);
void glHint(uint32_t target, uint32_t mode);
uint8_t glIsBuffer(uint32_t buffer);
uint8_t glIsEnabled(uint32_t cap);
uint8_t glIsFramebuffer(uint32_t framebuffer);
uint8_t glIsProgram(uint32_t program);
uint8_t glIsRenderbuffer(uint32_t renderbuffer);
uint8_t glIsShader(uint32_t shader);
uint8_t glIsTexture(uint32_t texture);
void glLineWidth(float width);
void glLinkProgram(uint32_t program);
void glPixelStorei(uint32_t pname, int32_t param);
void glPolygonOffset(float factor, float units);
void glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type, void* pixels);
void glReleaseShaderCompiler(void);
void glRenderbufferStorage(uint32_t target, uint32_t internalformat, int32_t width, int32_t height);
void glSampleCoverage(float value, uint8_t invert);
void glScissor(int32_t x, int32_t y, int32_t width, int32_t height);
void glShaderBinary(int32_t count, const uint32_t* shaders, uint32_t binaryformat, const void* binary, int32_t length);
void glShaderSource(uint32_t shader, int32_t count, const char** string, const int32_t* length);
void glStencilFunc(uint32_t func, int32_t ref, uint32_t mask);
void glStencilFuncSeparate(uint32_t face, uint32_t func, int32_t ref, uint32_t mask);
void glStencilMask(uint32_t mask);
void glStencilMaskSeparate(uint32_t face, uint32_t mask);
void glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass);
void glStencilOpSeparate(uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t border, uint32_t format, uint32_t type, const void* pixels);
void glTexParameterf(uint32_t target, uint32_t pname, float param);
void glTexParameterfv(uint32_t target, uint32_t pname, const float* params);
void glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
void glTexParameteriv(uint32_t target, uint32_t pname, const int32_t* params);
void glTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels);
void glUniform1f(int32_t location, float v0);
void glUniform1fv(int32_t location, int32_t count, const float* value);
void glUniform1i(int32_t location, int32_t v0);
void glUniform1iv(int32_t location, int32_t count, const int32_t* value);
void glUniform2f(int32_t location, float v0, float v1);
void glUniform2fv(int32_t location, int32_t count, const float* value);
void glUniform2i(int32_t location, int32_t v0, int32_t v1);
void glUniform2iv(int32_t location, int32_t count, const int32_t* value);
void glUniform3f(int32_t location, float v0, float v1, float v2);
void glUniform3fv(int32_t location, int32_t count, const float* value);
void glUniform3i(int32_t location, int32_t v0, int32_t v1, int32_t v2);
void glUniform3iv(int32_t location, int32_t count, const int32_t* value);
void glUniform4f(int32_t location, float v0, float v1, float v2, float v3);
void glUniform4fv(int32_t location, int32_t count, const float* value);
void glUniform4i(int32_t location, int32_t v0, int32_t v1, int32_t v2, int32_t v3);
void glUniform4iv(int32_t location, int32_t count, const int32_t* value);
void glUniformMatrix2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUseProgram(uint32_t program);
void glValidateProgram(uint32_t program);
void glVertexAttrib1f(uint32_t index, float x);
void glVertexAttrib1fv(uint32_t index, const float* v);
void glVertexAttrib2f(uint32_t index, float x, float y);
void glVertexAttrib2fv(uint32_t index, const float* v);
void glVertexAttrib3f(uint32_t index, float x, float y, float z);
void glVertexAttrib3fv(uint32_t index, const float* v);
void glVertexAttrib4f(uint32_t index, float x, float y, float z, float w);
void glVertexAttrib4fv(uint32_t index, const float* v);
void glVertexAttribPointer(uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride, const void* pointer);
void glViewport(int32_t x, int32_t y, int32_t width, int32_t height);
void glReadBuffer(uint32_t src);
void glDrawRangeElements(uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, const void* indices);
void glTexImage3D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, uint32_t format, uint32_t type, const void* pixels);
void glTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, uint32_t type, const void* pixels);
void glCopyTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t x, int32_t y, int32_t width, int32_t height);
void glCompressedTexImage3D(uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height, int32_t depth, int32_t border, int32_t imageSize, const void* data);
void glCompressedTexSubImage3D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t zoffset, int32_t width, int32_t height, int32_t depth, uint32_t format, int32_t imageSize, const void* data);
void glGenQueries(int32_t n, uint32_t* ids);
void glDeleteQueries(int32_t n, const uint32_t* ids);
uint8_t glIsQuery(uint32_t id);
void glBeginQuery(uint32_t target, uint32_t id);
void glEndQuery(uint32_t target);
void glGetQueryiv(uint32_t target, uint32_t pname, int32_t* params);
void glGetQueryObjectuiv(uint32_t id, uint32_t pname, uint32_t* params);
uint8_t glUnmapBuffer(uint32_t target);
void glGetBufferPointerv(uint32_t target, uint32_t pname, void** params);
void glDrawBuffers(int32_t n, const uint32_t* bufs);
void glUniformMatrix2x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix2x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4x2fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix3x4fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glUniformMatrix4x3fv(int32_t location, int32_t count, uint8_t transpose, const float* value);
void glBlitFramebuffer(int32_t srcX0, int32_t srcY0, int32_t srcX1, int32_t srcY1, int32_t dstX0, int32_t dstY0, int32_t dstX1, int32_t dstY1, uint32_t mask, uint32_t filter);
void glRenderbufferStorageMultisample(uint32_t target, int32_t samples, uint32_t internalformat, int32_t width, int32_t height);
void glFramebufferTextureLayer(uint32_t target, uint32_t attachment, uint32_t texture, int32_t level, int32_t layer);
void* glMapBufferRange(uint32_t target, long offset, long length, uint32_t access);
void glFlushMappedBufferRange(uint32_t target, long offset, long length);
void glBindVertexArray(uint32_t array);
void glDeleteVertexArrays(int32_t n, const uint32_t* arrays);
void glGenVertexArrays(int32_t n, uint32_t* arrays);
uint8_t glIsVertexArray(uint32_t array);
void glGetIntegeri_v(uint32_t target, uint32_t index, int32_t* data);
void glBeginTransformFeedback(uint32_t primitiveMode);
void glEndTransformFeedback(void);
void glBindBufferRange(uint32_t target, uint32_t index, uint32_t buffer, long offset, long size);
void glBindBufferBase(uint32_t target, uint32_t index, uint32_t buffer);
void glTransformFeedbackVaryings(uint32_t program, int32_t count, const char** varyings, uint32_t bufferMode);
void glGetTransformFeedbackVarying(uint32_t program, uint32_t index, int32_t bufSize, int32_t* length, int32_t* size, uint32_t* type, char* name);
void glVertexAttribIPointer(uint32_t index, int32_t size, uint32_t type, int32_t stride, const void* pointer);
void glGetVertexAttribIiv(uint32_t index, uint32_t pname, int32_t* params);
void glGetVertexAttribIuiv(uint32_t index, uint32_t pname, uint32_t* params);
void glVertexAttribI4i(uint32_t index, int32_t x, int32_t y, int32_t z, int32_t w);
void glVertexAttribI4ui(uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w);
void glVertexAttribI4iv(uint32_t index, const int32_t* v);
void glVertexAttribI4uiv(uint32_t index, const uint32_t* v);
void glGetUniformuiv(uint32_t program, int32_t location, uint32_t* params);
int32_t glGetFragDataLocation(uint32_t program, const char* name);
void glUniform1ui(int32_t location, uint32_t v0);
void glUniform2ui(int32_t location, uint32_t v0, uint32_t v1);
void glUniform3ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2);
void glUniform4ui(int32_t location, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3);
void glUniform1uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform2uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform3uiv(int32_t location, int32_t count, const uint32_t* value);
void glUniform4uiv(int32_t location, int32_t count, const uint32_t* value);
void glClearBufferiv(uint32_t buffer, int32_t drawbuffer, const int32_t* value);
void glClearBufferuiv(uint32_t buffer, int32_t drawbuffer, const uint32_t* value);
void glClearBufferfv(uint32_t buffer, int32_t drawbuffer, const float* value);
void glClearBufferfi(uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil);
const uint8_t* glGetStringi(uint32_t name, uint32_t index);
void glCopyBufferSubData(uint32_t readTarget, uint32_t writeTarget, long readOffset, long writeOffset, long size);
void glGetUniformIndices(uint32_t program, int32_t uniformCount, const char** uniformNames, uint32_t* uniformIndices);
void glGetActiveUniformsiv(uint32_t program, int32_t uniformCount, const uint32_t* uniformIndices, uint32_t pname, int32_t* params);
uint32_t glGetUniformBlockIndex(uint32_t program, const char* uniformBlockName);
void glGetActiveUniformBlockiv(uint32_t program, uint32_t uniformBlockIndex, uint32_t pname, int32_t* params);
void glGetActiveUniformBlockName(uint32_t program, uint32_t uniformBlockIndex, int32_t bufSize, int32_t* length, char* uniformBlockName);
void glUniformBlockBinding(uint32_t program, uint32_t uniformBlockIndex, uint32_t uniformBlockBinding);
void glDrawArraysInstanced(uint32_t mode, int32_t first, int32_t count, int32_t instancecount);
void glDrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type, const void* indices, int32_t instancecount);
void* glFenceSync(uint32_t condition, uint32_t flags);
uint8_t glIsSync(void* sync);
void glDeleteSync(void* sync);
uint32_t glClientWaitSync(void* sync, uint32_t flags, uint64_t timeout);
void glWaitSync(void* sync, uint32_t flags, uint64_t timeout);
void glGetInteger64v(uint32_t pname, int64_t* data);
void glGetSynciv(void* sync, uint32_t pname, int32_t bufSize, int32_t* length, int32_t* values);
void glGetInteger64i_v(uint32_t target, uint32_t index, int64_t* data);
void glGetBufferParameteri64v(uint32_t target, uint32_t pname, int64_t* params);
void glGenSamplers(int32_t count, uint32_t* samplers);
void glDeleteSamplers(int32_t count, const uint32_t* samplers);
uint8_t glIsSampler(uint32_t sampler);
void glBindSampler(uint32_t unit, uint32_t sampler);
void glSamplerParameteri(uint32_t sampler, uint32_t pname, int32_t param);
void glSamplerParameteriv(uint32_t sampler, uint32_t pname, const int32_t* param);
void glSamplerParameterf(uint32_t sampler, uint32_t pname, float param);
void glSamplerParameterfv(uint32_t sampler, uint32_t pname, const float* param);
void glGetSamplerParameteriv(uint32_t sampler, uint32_t pname, int32_t* params);
void glGetSamplerParameterfv(uint32_t sampler, uint32_t pname, float* params);
void glVertexAttribDivisor(uint32_t index, uint32_t divisor);
void glBindTransformFeedback(uint32_t target, uint32_t id);
void glDeleteTransformFeedbacks(int32_t n, const uint32_t* ids);
void glGenTransformFeedbacks(int32_t n, uint32_t* ids);
uint8_t glIsTransformFeedback(uint32_t id);
void glPauseTransformFeedback(void);
void glResumeTransformFeedback(void);
void glGetProgramBinary(uint32_t program, int32_t bufSize, int32_t* length, uint32_t* binaryFormat, void* binary);
void glProgramBinary(uint32_t program, uint32_t binaryFormat, const void* binary, int32_t length);
void glProgramParameteri(uint32_t program, uint32_t pname, int32_t value);
void glInvalidateFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments);
void glInvalidateSubFramebuffer(uint32_t target, int32_t numAttachments, const uint32_t* attachments, int32_t x, int32_t y, int32_t width, int32_t height);
void glTexStorage2D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height);
void glTexStorage3D(uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height, int32_t depth);
void glGetInternalformativ(uint32_t target, uint32_t internalformat, uint32_t pname, int32_t bufSize, int32_t* params);

#endif /* VIRTPASS_GL */
