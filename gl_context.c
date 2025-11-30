#define _POSIX_C_SOURCE 200112L  // POSIX.1-2001 for posix_memalign
#include "gl_context.h"
#include "drm_display.h"
#include "keystone.h"
#include "video_decoder.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

// PRODUCTION: Validate EGL context before critical operations
static bool validate_egl_context(void) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) {
        LOG_ERROR("GL", "EGL context lost");
        return false;
    }
    return true;
}

// PRODUCTION: Check and clear GL errors
__attribute__((unused))
static void check_gl_errors(const char *operation) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        LOG_ERROR("GL", "GL error in %s: 0x%x", operation, err);
    }
}

// Double-buffered Pixel Buffer Objects for staging
// PBO code removed - was causing flickering issues
#define PLANE_Y 0
#define PLANE_U 1
#define PLANE_V 2

#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0040
#endif

// NEON SIMD optimization for stride copying on ARM
#ifdef __ARM_NEON__
    #include <arm_neon.h>
    #define HAS_NEON 1
#else
    #define HAS_NEON 0
#endif

// OPTIMIZED: NEON-accelerated stride copy for YUV planes with prefetching
// Copies 'rows' rows of 'width' bytes from source to destination with strides
static inline void copy_with_stride_simd(uint8_t *dst, const uint8_t *src,
                                         int width, int height,
                                         int dst_stride, int src_stride) {
    #if HAS_NEON
    // OPTIMIZATION: Process 32 bytes per iteration (2x NEON registers)
    int width_32 = (width / 32) * 32;

    for (int row = 0; row < height; row++) {
        uint8_t *s = (uint8_t *)src + (row * src_stride);
        uint8_t *d = dst + (row * dst_stride);

        // OPTIMIZATION: Prefetch 8 rows ahead for better cache utilization
        if (row + 8 < height) {
            __builtin_prefetch(src + ((row + 8) * src_stride), 0, 0);
        }

        // Process 32 bytes at a time (10-15% faster than 16-byte)
        for (int col = 0; col < width_32; col += 32) {
            uint8x16_t data1 = vld1q_u8(s + col);
            uint8x16_t data2 = vld1q_u8(s + col + 16);
            vst1q_u8(d + col, data1);
            vst1q_u8(d + col + 16, data2);
        }

        // Handle remaining bytes
        if (width_32 < width) {
            memcpy(d + width_32, s + width_32, width - width_32);
        }
    }
    #else
    for (int row = 0; row < height; row++) {
        memcpy(dst + (row * dst_stride), src + (row * src_stride), width);
    }
    #endif
}

// Forward declarations
static void draw_char_simple(float *vertices, int *vertex_count, char c, float x, float y, float size);
static void draw_text_simple(float *vertices, int *vertex_count, const char *text, float x, float y, float size);

#ifndef GLeglImageOES
typedef void* GLeglImageOES;
#endif

typedef EGLImage (*eglCreateImageKHR_func)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef void (*glEGLImageTargetTexture2DOES_func)(GLenum target, GLeglImageOES image);
typedef EGLBoolean (*eglDestroyImageKHR_func)(EGLDisplay dpy, EGLImage image);

static eglCreateImageKHR_func eglCreateImageKHR = NULL;
static glEGLImageTargetTexture2DOES_func glEGLImageTargetTexture2DOES = NULL;
static eglDestroyImageKHR_func eglDestroyImageKHR = NULL;

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE1_FD_EXT
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#endif
#ifndef EGL_DMA_BUF_PLANE1_OFFSET_EXT
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#endif
#ifndef EGL_DMA_BUF_PLANE1_PITCH_EXT
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#endif
#ifndef EGL_DMA_BUF_PLANE2_FD_EXT
#define EGL_DMA_BUF_PLANE2_FD_EXT 0x3278
#endif
#ifndef EGL_DMA_BUF_PLANE2_OFFSET_EXT
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT 0x3279
#endif
#ifndef EGL_DMA_BUF_PLANE2_PITCH_EXT
#define EGL_DMA_BUF_PLANE2_PITCH_EXT 0x327A
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e
#endif
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 0x20203852
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef DRM_FORMAT_YUV420
#define DRM_FORMAT_YUV420 0x32315559  // YU12
#endif

// EGL colorspace hint extensions for accurate YUV→RGB conversion
// These ensure the GPU driver uses BT.709 TV-range (16-235) instead of BT.601 or full-range
#ifndef EGL_YUV_COLOR_SPACE_HINT_EXT
#define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
#endif
#ifndef EGL_SAMPLE_RANGE_HINT_EXT
#define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
#endif
#ifndef EGL_ITU_REC709_EXT
#define EGL_ITU_REC709_EXT 0x3280
#endif
#ifndef EGL_YUV_NARROW_RANGE_EXT
#define EGL_YUV_NARROW_RANGE_EXT 0x3283
#endif

typedef struct {
    uint8_t *y_temp_buffer;
    uint8_t *u_temp_buffer;
    uint8_t *v_temp_buffer;
    int allocated_size;
} yuv_temp_buffers_t;

static yuv_temp_buffers_t g_yuv_buffers = {NULL, NULL, NULL, 0};
static pthread_once_t yuv_cleanup_registered = PTHREAD_ONCE_INIT;

// Forward declaration
static void free_yuv_buffers(void);

static void register_yuv_cleanup(void) {
    atexit(free_yuv_buffers);
}

static void allocate_yuv_buffers(int needed_size) {
    // Register cleanup handler on first allocation (thread-safe)
    pthread_once(&yuv_cleanup_registered, register_yuv_cleanup);

    // Only allocate if needed or size changed
    if (g_yuv_buffers.allocated_size < needed_size) {
        // Free old buffers if they exist
        free(g_yuv_buffers.y_temp_buffer);
        free(g_yuv_buffers.u_temp_buffer);
        free(g_yuv_buffers.v_temp_buffer);
        
        // Allocate new buffers with 20% headroom to reduce reallocations
        // Example: 1920x1080 YUV420P = 3.1MB, allocate 3.7MB to handle variances
        // Prevents frequent realloc() calls for minor resolution changes
        int alloc_size = needed_size * 1.2;

        // Round up to 64-byte alignment for cache efficiency (Cortex-A72 has 64-byte cache lines)
        // This improves NEON load/store performance and reduces cache misses
        alloc_size = ((alloc_size + 63) / 64) * 64;
        int alloc_size_uv = (((alloc_size / 4) + 63) / 64) * 64;

        // Use posix_memalign for aligned allocation (POSIX.1-2001)
        if (posix_memalign((void**)&g_yuv_buffers.y_temp_buffer, 64, alloc_size) != 0) {
            g_yuv_buffers.y_temp_buffer = NULL;
        }
        if (posix_memalign((void**)&g_yuv_buffers.u_temp_buffer, 64, alloc_size_uv) != 0) {
            g_yuv_buffers.u_temp_buffer = NULL;
        }
        if (posix_memalign((void**)&g_yuv_buffers.v_temp_buffer, 64, alloc_size_uv) != 0) {
            g_yuv_buffers.v_temp_buffer = NULL;
        }
        
        // Check for allocation failures (critical for production)
        if (!g_yuv_buffers.y_temp_buffer || !g_yuv_buffers.u_temp_buffer || !g_yuv_buffers.v_temp_buffer) {
            LOG_ERROR("GL", "CRITICAL: Failed to allocate YUV temp buffers (%d bytes)", alloc_size);
            free(g_yuv_buffers.y_temp_buffer);
            free(g_yuv_buffers.u_temp_buffer);
            free(g_yuv_buffers.v_temp_buffer);
            g_yuv_buffers.y_temp_buffer = NULL;
            g_yuv_buffers.u_temp_buffer = NULL;
            g_yuv_buffers.v_temp_buffer = NULL;
            g_yuv_buffers.allocated_size = 0;
            return;  // Return without updating allocated_size to trigger fallback
        }
        
        g_yuv_buffers.allocated_size = alloc_size;
    }
}

static void free_yuv_buffers(void) {
    if (g_yuv_buffers.y_temp_buffer) {
        free(g_yuv_buffers.y_temp_buffer);
        g_yuv_buffers.y_temp_buffer = NULL;
    }
    if (g_yuv_buffers.u_temp_buffer) {
        free(g_yuv_buffers.u_temp_buffer);
        g_yuv_buffers.u_temp_buffer = NULL;
    }
    if (g_yuv_buffers.v_temp_buffer) {
        free(g_yuv_buffers.v_temp_buffer);
        g_yuv_buffers.v_temp_buffer = NULL;
    }
    g_yuv_buffers.allocated_size = 0;
}

// Modern vertex shader for keystone correction and video rendering (OpenGL ES 3.1)
// OPTIMIZATION: Use highp for geometry (precision needed for matrix math)
const char *vertex_shader_source = 
    "#version 310 es\n"
    "precision highp float;\n"
    "\n"
    "// Vertex attributes with explicit locations\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "\n"
    "// Legacy uniforms for compatibility\n"
    "uniform mat4 u_mvp_matrix;\n"
    "uniform mat4 u_keystone_matrix;\n"
    "uniform float u_flip_y;\n"
    "\n"
    "// Output to fragment shader\n"
    "out vec2 v_texcoord;\n"
    "\n"
    "void main() {\n"
    "    // Apply keystone correction first, then MVP transformation\n"
    "    vec4 pos = vec4(a_position, 0.0, 1.0);\n"
    "    pos = u_keystone_matrix * pos;\n"
    "    gl_Position = u_mvp_matrix * pos;\n"
    "    \n"
    "    // Pass texture coordinates to fragment shader, optionally flipping Y\n"
    "    v_texcoord = vec2(a_texcoord.x, u_flip_y > 0.5 ? 1.0 - a_texcoord.y : a_texcoord.y);\n"
    "}\n";

// Optimized YUV→RGB conversion fragment shader (OpenGL ES 3.1)
// OPTIMIZATIONS:
// 1. Pre-computed combined matrix (TV range + BT.709 conversion in one step)
// 2. Uses mat3 for GPU-optimized parallel matrix-vector multiply
// 3. Single clamp() call on vec3
// 4. Minimal ALU operations - all constants folded at compile time
const char *fragment_shader_source =
    "#version 310 es\n"
    "precision highp float;\n"  // KEEP highp for color accuracy
    "in vec2 v_texcoord;\n"
    "\n"
    "uniform sampler2D u_texture_y;\n"
    "uniform sampler2D u_texture_u;\n"
    "uniform sampler2D u_texture_v;\n"
    "uniform sampler2D u_texture_nv12;\n"
    "uniform int u_use_nv12;\n"
    "\n"
    "out vec4 fragColor;\n"
    "\n"
    "// Combined BT.709 TV-range YUV to RGB matrix (pre-computed)\n"
    "// This combines: TV range expansion + UV centering + BT.709 matrix\n"
    "// rgb = M * yuv + offset\n"
    "// Where M incorporates Y_SCALE(255/219), UV_SCALE(255/224), and BT.709 coefficients\n"
    "const mat3 YUV_TO_RGB = mat3(\n"
    "    1.16438356,  1.16438356,  1.16438356,   // Y column (scaled)\n"
    "    0.0,        -0.21322098,  2.11240179,   // U column (scaled, centered)\n"
    "    1.79274107, -0.53288170,  0.0           // V column (scaled, centered)\n"
    ");\n"
    "// Offset: accounts for Y offset + UV offset contributions to RGB\n"
    "const vec3 YUV_OFFSET = vec3(-0.97294508, 0.30145492, -1.13340222);\n"
    "\n"
    "void main() {\n"
    "    vec3 yuv;\n"
    "    if (u_use_nv12 > 0) {\n"
    "        vec2 uv = texture(u_texture_nv12, v_texcoord).rg;\n"
    "        yuv = vec3(texture(u_texture_y, v_texcoord).r, uv.r, uv.g);\n"
    "    } else {\n"
    "        yuv = vec3(\n"
    "            texture(u_texture_y, v_texcoord).r,\n"
    "            texture(u_texture_u, v_texcoord).r,\n"
    "            texture(u_texture_v, v_texcoord).r\n"
    "        );\n"
    "    }\n"
    "    \n"
    "    // Single matrix multiply + offset (GPU executes in parallel)\n"
    "    vec3 rgb = YUV_TO_RGB * yuv + YUV_OFFSET;\n"
    "    \n"
    "    // Clamp to valid range\n"
    "    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

// Corner highlight shaders (OpenGL ES 3.1) - OPTIMIZED
const char *corner_vertex_shader_source = 
    "#version 310 es\n"
    "precision mediump float;\n"  // OPTIMIZED: mediump for overlay geometry
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec4 a_color;\n"
    "uniform mat4 u_mvp_matrix;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp_matrix * vec4(a_position, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

const char *corner_fragment_shader_source =
    "#version 310 es\n"
    "precision mediump float;\n"  // OPTIMIZED: mediump for overlay colors
    "in vec4 v_color;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = v_color;\n"
    "}\n";

// External texture shader for zero-copy YUV EGLImage import
// Uses GL_OES_EGL_image_external extension with samplerExternalOES
// Note: ESSL 1.00 required for samplerExternalOES (not ESSL 3.x)
static const char *external_vertex_shader_source =
    "#version 100\n"
    "precision highp float;\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "uniform mat4 u_mvp_matrix;\n"
    "uniform mat4 u_keystone_matrix;\n"
    "uniform float u_flip_y;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    vec4 pos = vec4(a_position, 0.0, 1.0);\n"
    "    pos = u_keystone_matrix * pos;\n"
    "    gl_Position = u_mvp_matrix * pos;\n"
    "    v_texcoord = vec2(a_texcoord.x, u_flip_y > 0.5 ? 1.0 - a_texcoord.y : a_texcoord.y);\n"
    "}\n";

static const char *external_fragment_shader_source =
    "#version 100\n"
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES u_texture_external;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    // Sample from external YUV texture - driver does YUV->RGB conversion\n"
    "    gl_FragColor = texture2D(u_texture_external, v_texcoord);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        if (!log) {
            LOG_ERROR("GL", "Shader compilation failed (out of memory for log)");
            glDeleteShader(shader);
            return 0;
        }
        glGetShaderInfoLog(shader, length, NULL, log);
        LOG_ERROR("GL", "Shader compilation failed: %s", log);
        free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int create_program(gl_context_t *gl) {
    gl->vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    if (!gl->vertex_shader) return -1;

    gl->fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!gl->fragment_shader) {
        glDeleteShader(gl->vertex_shader);
        return -1;
    }

    gl->program = glCreateProgram();
    glAttachShader(gl->program, gl->vertex_shader);
    glAttachShader(gl->program, gl->fragment_shader);
    glLinkProgram(gl->program);

    GLint linked;
    glGetProgramiv(gl->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length;
        glGetProgramiv(gl->program, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        if (!log) {
            LOG_ERROR("GL", "Program linking failed (out of memory for log)");
            glDeleteProgram(gl->program);
            glDeleteShader(gl->vertex_shader);
            glDeleteShader(gl->fragment_shader);
            return -1;
        }
        glGetProgramInfoLog(gl->program, length, NULL, log);
        LOG_ERROR("GL", "Program linking failed: %s", log);
        free(log);
        glDeleteProgram(gl->program);
        glDeleteShader(gl->vertex_shader);
        glDeleteShader(gl->fragment_shader);
        return -1;
    }

    // Get uniform and attribute locations
    gl->u_mvp_matrix = glGetUniformLocation(gl->program, "u_mvp_matrix");
    gl->u_texture_y = glGetUniformLocation(gl->program, "u_texture_y");
    gl->u_texture_u = glGetUniformLocation(gl->program, "u_texture_u");
    gl->u_texture_v = glGetUniformLocation(gl->program, "u_texture_v");
    gl->u_texture_nv12 = glGetUniformLocation(gl->program, "u_texture_nv12");
    gl->u_use_nv12 = glGetUniformLocation(gl->program, "u_use_nv12");
    gl->u_keystone_matrix = glGetUniformLocation(gl->program, "u_keystone_matrix");
    gl->u_flip_y = glGetUniformLocation(gl->program, "u_flip_y");
    gl->a_position = glGetAttribLocation(gl->program, "a_position");
    gl->a_texcoord = glGetAttribLocation(gl->program, "a_texcoord");

    return 0;
}

static int create_corner_program(gl_context_t *gl) {
    GLuint corner_vertex_shader = compile_shader(GL_VERTEX_SHADER, corner_vertex_shader_source);
    if (!corner_vertex_shader) return -1;

    GLuint corner_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, corner_fragment_shader_source);
    if (!corner_fragment_shader) {
        glDeleteShader(corner_vertex_shader);
        return -1;
    }

    gl->corner_program = glCreateProgram();
    glAttachShader(gl->corner_program, corner_vertex_shader);
    glAttachShader(gl->corner_program, corner_fragment_shader);
    glLinkProgram(gl->corner_program);

    GLint linked;
    glGetProgramiv(gl->corner_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length;
        glGetProgramiv(gl->corner_program, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        if (!log) {
            LOG_ERROR("GL", "Corner program linking failed (out of memory for log)");
            glDeleteProgram(gl->corner_program);
            glDeleteShader(corner_vertex_shader);
            glDeleteShader(corner_fragment_shader);
            return -1;
        }
        glGetProgramInfoLog(gl->corner_program, length, NULL, log);
        LOG_ERROR("GL", "Corner program linking failed: %s", log);
        free(log);
        glDeleteProgram(gl->corner_program);
        glDeleteShader(corner_vertex_shader);
        glDeleteShader(corner_fragment_shader);
        return -1;
    }

    // Get uniform and attribute locations for corner rendering
    gl->corner_u_mvp_matrix = glGetUniformLocation(gl->corner_program, "u_mvp_matrix");
    gl->corner_u_color = glGetUniformLocation(gl->corner_program, "u_color");
    gl->corner_a_position = glGetAttribLocation(gl->corner_program, "a_position");

    // Clean up shaders (they're linked into the program now)
    glDeleteShader(corner_vertex_shader);
    glDeleteShader(corner_fragment_shader);

    return 0;
}

static int create_external_program(gl_context_t *gl) {
    // Check for GL_OES_EGL_image_external extension
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "GL_OES_EGL_image_external")) {
        LOG_INFO("GL", "GL_OES_EGL_image_external not supported - external texture disabled");
        gl->supports_external_texture = false;
        return 0;  // Not an error, just not supported
    }

    GLuint ext_vertex_shader = compile_shader(GL_VERTEX_SHADER, external_vertex_shader_source);
    if (!ext_vertex_shader) {
        LOG_ERROR("GL", "Failed to compile external vertex shader");
        gl->supports_external_texture = false;
        return 0;
    }

    GLuint ext_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, external_fragment_shader_source);
    if (!ext_fragment_shader) {
        LOG_ERROR("GL", "Failed to compile external fragment shader");
        glDeleteShader(ext_vertex_shader);
        gl->supports_external_texture = false;
        return 0;
    }

    gl->external_program = glCreateProgram();
    glAttachShader(gl->external_program, ext_vertex_shader);
    glAttachShader(gl->external_program, ext_fragment_shader);
    glLinkProgram(gl->external_program);

    GLint linked;
    glGetProgramiv(gl->external_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint length;
        glGetProgramiv(gl->external_program, GL_INFO_LOG_LENGTH, &length);
        char *log = malloc(length);
        if (!log) {
            LOG_ERROR("GL", "External program linking failed (out of memory for log)");
            glDeleteProgram(gl->external_program);
            glDeleteShader(ext_vertex_shader);
            glDeleteShader(ext_fragment_shader);
            gl->external_program = 0;
            gl->supports_external_texture = false;
            return 0;
        }
        glGetProgramInfoLog(gl->external_program, length, NULL, log);
        LOG_ERROR("GL", "External program linking failed: %s", log);
        free(log);
        glDeleteProgram(gl->external_program);
        glDeleteShader(ext_vertex_shader);
        glDeleteShader(ext_fragment_shader);
        gl->external_program = 0;
        gl->supports_external_texture = false;
        return 0;
    }

    // Get uniform locations
    gl->ext_u_mvp_matrix = glGetUniformLocation(gl->external_program, "u_mvp_matrix");
    gl->ext_u_keystone_matrix = glGetUniformLocation(gl->external_program, "u_keystone_matrix");
    gl->ext_u_flip_y = glGetUniformLocation(gl->external_program, "u_flip_y");
    gl->ext_u_texture_external = glGetUniformLocation(gl->external_program, "u_texture_external");

    // Clean up shaders
    glDeleteShader(ext_vertex_shader);
    glDeleteShader(ext_fragment_shader);

    // Create external textures
    glGenTextures(1, &gl->texture_external);
    glGenTextures(1, &gl->texture_external2);

    gl->supports_external_texture = true;
    LOG_INFO("GL", "External texture program created (GL_OES_EGL_image_external)");

    return 0;
}

int gl_init(gl_context_t *gl, display_ctx_t *drm) {
    memset(gl, 0, sizeof(*gl));

    // Get EGL display
    gl->egl_display = eglGetDisplay((EGLNativeDisplayType)drm->gbm_device);
    if (gl->egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("GL", "Failed to get EGL display");
        return -1;
    }

    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(gl->egl_display, &major, &minor)) {
        LOG_ERROR("GL", "Failed to initialize EGL");
        return -1;
    }

    LOG_INFO("GL", "EGL version: %d.%d", major, minor);

    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOG_ERROR("GL", "Failed to bind OpenGL ES API");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Choose EGL config
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };

    EGLint num_configs;
    if (!eglChooseConfig(gl->egl_display, config_attrs, &gl->egl_config, 1, &num_configs)) {
        LOG_ERROR("GL", "Failed to choose EGL config");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Create EGL context for OpenGL ES 3.1
    EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_NONE
    };

    gl->egl_context = eglCreateContext(gl->egl_display, gl->egl_config, EGL_NO_CONTEXT, context_attrs);
    if (gl->egl_context == EGL_NO_CONTEXT) {
        LOG_ERROR("GL", "Failed to create EGL context");
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Create EGL surface
    gl->egl_surface = eglCreateWindowSurface(gl->egl_display, gl->egl_config,
                                            (EGLNativeWindowType)drm->gbm_surface, NULL);
    if (gl->egl_surface == EGL_NO_SURFACE) {
        LOG_ERROR("GL", "Failed to create EGL surface");
        eglDestroyContext(gl->egl_display, gl->egl_context);
        eglTerminate(gl->egl_display);
        return -1;
    }

    // Make current
    if (!eglMakeCurrent(gl->egl_display, gl->egl_surface, gl->egl_surface, gl->egl_context)) {
        LOG_ERROR("GL", "Failed to make EGL context current");
        eglDestroySurface(gl->egl_display, gl->egl_surface);
        eglDestroyContext(gl->egl_display, gl->egl_context);
        eglTerminate(gl->egl_display);
        return -1;
    }

    // FIXED: Enable VSync for smooth, tear-free playback
    // Set swap interval to 1 to sync with display refresh (eliminates jitter)
    if (!eglSwapInterval(gl->egl_display, 1)) {
        LOG_WARN("GL", "Could not enable VSync (swap interval) - playback may be jittery");
    } else {
        LOG_INFO("GL", "VSync enabled for smooth playback (synced to display refresh)");
    }

    // Detect EGL DMA buffer import support (zero-copy rendering)
    gl->supports_egl_image = false;
    const char *egl_extensions = eglQueryString(gl->egl_display, EGL_EXTENSIONS);
    if (egl_extensions && strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        gl->supports_egl_image = true;
        if (hw_debug_enabled) {
            LOG_INFO("GL", "DMA buffer import supported - zero-copy rendering enabled!");
        }
        
        // Load EGL extension function pointers
        eglCreateImageKHR = (eglCreateImageKHR_func)eglGetProcAddress("eglCreateImageKHR");
        glEGLImageTargetTexture2DOES = (glEGLImageTargetTexture2DOES_func)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        eglDestroyImageKHR = (eglDestroyImageKHR_func)eglGetProcAddress("eglDestroyImageKHR");
        
        if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES || !eglDestroyImageKHR) {
            LOG_ERROR("GL", "Failed to load DMA buffer extension functions");
            gl->supports_egl_image = false;
        } else if (hw_debug_enabled) {
            LOG_INFO("GL", "Extension functions loaded successfully");
        }
    } else if (hw_debug_enabled) {
        LOG_INFO("GL", "DMA buffer import NOT supported, using standard texture upload");
    }

    // Check for YUV colorspace hint support (for accurate BT.709 TV-range conversion)
    if (egl_extensions && strstr(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
        LOG_INFO("GL", "EGL YUV colorspace hints supported (BT.709 TV-range enabled)");
    } else {
        LOG_WARN("GL", "EGL YUV colorspace hints not supported - HW decode colors may be inaccurate");
    }

    // Initialize EGL image handles
    gl->egl_image_y = EGL_NO_IMAGE;
    gl->egl_image_uv = EGL_NO_IMAGE;
    gl->egl_image_y2 = EGL_NO_IMAGE;
    gl->egl_image_uv2 = EGL_NO_IMAGE;

    // Create shaders and program
    if (create_program(gl) != 0) {
        gl_cleanup(gl);
        return -1;
    }

    // Create corner rendering program
    if (create_corner_program(gl) != 0) {
        gl_cleanup(gl);
        return -1;
    }

    // Create external texture program (for zero-copy YUV import)
    create_external_program(gl);  // Non-fatal if unsupported

    // Optimize GL pixel store state for YUV texture uploads
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // Tight packing for single-byte YUV planes
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // Use width from glTexSubImage2D calls

    LOG_INFO("GL", "Using direct glTexSubImage2D uploads (PBOs removed for stability)");

    // OpenGL ES initialization complete
    return 0;
}

// Calculate aspect-ratio-preserving MVP matrix
// Prevents video stretching by letterboxing/pillarboxing as needed
static void calculate_aspect_ratio_matrix(float *mvp_matrix,
                                         int video_width, int video_height,
                                         int display_width, int display_height) {
    // Calculate aspect ratios
    float video_aspect = (float)video_width / (float)video_height;
    float display_aspect = (float)display_width / (float)display_height;

    // Initialize as identity matrix
    for (int i = 0; i < 16; i++) {
        mvp_matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;  // 1 on diagonal, 0 elsewhere
    }

    // Scale to preserve aspect ratio
    if (video_aspect > display_aspect) {
        // Video is wider than display - add pillarbox (black bars left/right)
        // Scale Y to fit, X stays at 1.0
        mvp_matrix[5] = display_aspect / video_aspect;  // scale_y
    } else if (video_aspect < display_aspect) {
        // Video is taller than display - add letterbox (black bars top/bottom)
        // Scale X to fit, Y stays at 1.0
        mvp_matrix[0] = video_aspect / display_aspect;  // scale_x
    }
    // else: aspects match perfectly, no scaling needed (identity matrix)
}

void gl_setup_buffers(gl_context_t *gl) {
    // Quad vertices (position + texture coordinates)  
    float vertices[] = {
        // Position  // TexCoord (standard orientation)
        -1.0f, -1.0f, 0.0f, 0.0f,  // Bottom-left
         1.0f, -1.0f, 1.0f, 0.0f,  // Bottom-right
         1.0f,  1.0f, 1.0f, 1.0f,  // Top-right
        -1.0f,  1.0f, 0.0f, 1.0f   // Top-left
    };

    GLuint indices[] = {
        0, 1, 2,  // First triangle
        2, 3, 0   // Second triangle
    };

    // Ensure tightly packed pixel transfers for YUV/NV12 uploads
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    // Generate and bind VBO (no VAO for compatibility)
    glGenBuffers(1, &gl->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Generate and bind EBO
    glGenBuffers(1, &gl->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Store for later use - don't set attributes here since no VAO

    // Generate YUV textures for video 1
    glGenTextures(1, &gl->texture_y);
    glGenTextures(1, &gl->texture_u);
    glGenTextures(1, &gl->texture_v);
    glGenTextures(1, &gl->texture_nv12);
    glGenTextures(1, &gl->texture_nv12_2);
    
    // Generate YUV textures for video 2
    glGenTextures(1, &gl->texture_y2);
    glGenTextures(1, &gl->texture_u2);
    glGenTextures(1, &gl->texture_v2);
    
    // Setup Y texture (video 1)
    glBindTexture(GL_TEXTURE_2D, gl->texture_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup U texture (video 1)
    glBindTexture(GL_TEXTURE_2D, gl->texture_u);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup V texture (video 1)
    glBindTexture(GL_TEXTURE_2D, gl->texture_v);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup Y texture (video 2)
    glBindTexture(GL_TEXTURE_2D, gl->texture_y2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup U texture (video 2)
    glBindTexture(GL_TEXTURE_2D, gl->texture_u2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup V texture (video 2)
    glBindTexture(GL_TEXTURE_2D, gl->texture_v2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup NV12 UV textures (interleaved U/V planes)
    glBindTexture(GL_TEXTURE_2D, gl->texture_nv12);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, gl->texture_nv12_2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup corner highlight VBO - will be updated with actual corner positions
    glGenBuffers(1, &gl->corner_vbo);
    
    // Setup border VBO - OPTIMIZED: pre-allocate buffer for 8 vertices * 6 floats
    // This avoids glBufferData reallocation every frame when border is visible
    glGenBuffers(1, &gl->border_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl->border_vbo);
    glBufferData(GL_ARRAY_BUFFER, 48 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    
    // Setup help overlay VBO - will be updated with help overlay geometry
    glGenBuffers(1, &gl->help_vbo);
    
    // Modern ES 3.1 UBO setup (disabled for compatibility)
    // setup_uniform_buffers(gl);
}

// Helper function to upload NV12 data to GPU
void gl_render_nv12(gl_context_t *gl, uint8_t *nv12_data, int width, int height, int stride,
                    display_ctx_t *drm, keystone_context_t *keystone, bool clear_screen, int video_index) {
    // PRODUCTION: Validate EGL context before rendering
    if (!validate_egl_context()) {
        LOG_ERROR("GL", "Cannot render NV12 - EGL context lost");
        return;
    }
    
    static int frame_rendered[2] = {0, 0};
    static int last_width[2] = {0, 0};
    static int last_height[2] = {0, 0};
    static bool gl_state_set = false;

    GLuint tex_y = (video_index == 0) ? gl->texture_y : gl->texture_y2;
    GLuint tex_uv = (video_index == 0) ? gl->texture_nv12 : gl->texture_nv12_2;

    if (frame_rendered[video_index] == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }

    if (clear_screen) {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (!gl_state_set) {
        glViewport(0, 0, drm->width, drm->height);
        gl_state_set = true;
    }

    if (video_index == 0) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glUseProgram(0);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);

        glUseProgram(gl->program);
        glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }

    float mvp_matrix[16];
    calculate_aspect_ratio_matrix(mvp_matrix, width, height, drm->width, drm->height);
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);

    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    glUniform1f(gl->u_flip_y, 1.0f);
    if (gl->u_use_nv12 >= 0) {
        glUniform1i(gl->u_use_nv12, 1);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    glUniform1i(gl->u_texture_y, 0);
    glUniform1i(gl->u_texture_nv12, 1);
    glUniform1i(gl->u_texture_u, 2);
    glUniform1i(gl->u_texture_v, 3);

    if (nv12_data) {
        bool size_changed = (width != last_width[video_index] ||
                             height != last_height[video_index] ||
                             frame_rendered[video_index] == 0);

        int y_stride = (stride > 0) ? stride : width;
        uint8_t *y_plane = nv12_data;
        uint8_t *uv_plane = nv12_data + (size_t)y_stride * height;
        int uv_width = width / 2;
        int uv_height = height / 2;

        if (size_changed) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex_y);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tex_uv);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_width, uv_height, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
        }

        // Direct texture upload (PBO code removed for stability)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_y);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_plane);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_uv);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RG, GL_UNSIGNED_BYTE, uv_plane);

        last_width[video_index] = width;
        last_height[video_index] = height;
        frame_rendered[video_index]++;
    }

    if (frame_rendered[video_index] > 0) {
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void gl_render_frame(gl_context_t *gl, uint8_t *y_data, uint8_t *u_data, uint8_t *v_data,
                    int width, int height, int y_stride, int u_stride, int v_stride,
                    display_ctx_t *drm, keystone_context_t *keystone, bool clear_screen, int video_index) {
    // PRODUCTION: Validate EGL context before rendering
    if (!validate_egl_context()) {
        LOG_ERROR("GL", "Cannot render - EGL context lost");
        return;
    }
    
    // Separate tracking for each video
    static int frame_rendered[2] = {0, 0};
    static int last_width[2] = {0, 0};
    static int last_height[2] = {0, 0};
    static bool gl_state_set = false;
    
    // Select appropriate texture set based on video index
    GLuint tex_y = (video_index == 0) ? gl->texture_y : gl->texture_y2;
    GLuint tex_u = (video_index == 0) ? gl->texture_u : gl->texture_u2;
    GLuint tex_v = (video_index == 0) ? gl->texture_v : gl->texture_v2;
    
    // Handle stride for YUV data with potential padding
    
    // Set black clear color for video background
    if (frame_rendered[video_index] == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Black background
    }
    
    // Only clear screen if requested (first video clears, second doesn't)
    if (clear_screen) {
        glClear(GL_COLOR_BUFFER_BIT);
    }
    
    // Set up OpenGL state only once or when needed
    if (!gl_state_set) {
        glViewport(0, 0, drm->width, drm->height);
        gl_state_set = true;
    }
    
    // CRITICAL: Always set up shader program - external_program may have been used by video 1 HW decode
    // This ensures gl_render_frame uses the correct program regardless of what rendered before it
    glUseProgram(gl->program);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Explicitly disable blending - overlays might have enabled it
    glDisable(GL_BLEND);

    // Disable depth test - make sure video is always visible
    glDisable(GL_DEPTH_TEST);

    // CRITICAL: Unbind any external textures that may have been bound by HW decode path
    // This prevents GL_TEXTURE_EXTERNAL_OES from interfering with GL_TEXTURE_2D
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    // Set MVP matrix with aspect ratio preservation (recalculate for each video!)
    // This is OUTSIDE the video_index==0 block so each video gets its own aspect ratio
    float mvp_matrix[16];
    calculate_aspect_ratio_matrix(mvp_matrix, width, height, drm->width, drm->height);
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);

    // Set keystone matrix (may change dynamically)
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    
    // Set flip_y uniform (flip both videos - they're both encoded upside down)
    glUniform1f(gl->u_flip_y, 1.0f);
    
    // OPTIMIZED: Bind textures without excessive glTexParameteri calls
    // Texture parameters are set once during initialization, not per-frame
    if (gl->u_use_nv12 >= 0) {
        glUniform1i(gl->u_use_nv12, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v);
    
    // Reset to texture unit 0
    glActiveTexture(GL_TEXTURE0);
    
    // Set texture sampler uniforms
    glUniform1i(gl->u_texture_y, 0);
    glUniform1i(gl->u_texture_u, 1);
    glUniform1i(gl->u_texture_v, 2);
    
    // TIMER: Measure texture upload time
    struct timespec tex_start, tex_end;
    clock_gettime(CLOCK_MONOTONIC, &tex_start);
    
    // Update YUV textures if video data is provided
    if (y_data && u_data && v_data) {
        // Calculate UV dimensions (usually half resolution for 4:2:0)
        int uv_width = width / 2;
        int uv_height = height / 2;
        
        // OPTIMIZED: Check if we can use direct upload for all planes
        bool y_direct = (y_stride == width);
        bool u_direct = (u_stride == uv_width);
        bool v_direct = (v_stride == uv_width);
        bool size_changed = (width != last_width[video_index] || height != last_height[video_index] || frame_rendered[video_index] == 0);
        
        // PRODUCTION: Only log on first frame to reduce console spam
        if (size_changed && frame_rendered[video_index] == 0) {
            LOG_DEBUG("GL", "YUV strides: Y=%d U=%d V=%d (dimensions: %dx%d, UV: %dx%d)",
                   y_stride, u_stride, v_stride, width, height, uv_width, uv_height);
            LOG_DEBUG("GL", "Direct upload: Y=%s U=%s V=%s", y_direct?"YES":"NO", u_direct?"YES":"NO", v_direct?"YES":"NO");
        }
        
        // ULTRA-OPTIMIZED: Use glTexStorage2D once, then glTexSubImage2D for updates
        // This is faster on some GPUs because storage is immutable
        // PRODUCTION FIX: glTexStorage2D creates immutable storage - must delete and recreate
        // textures on resolution change to avoid undefined behavior
        static bool storage_initialized[2] = {false, false};
        static int stored_width[2] = {0, 0};
        static int stored_height[2] = {0, 0};
        
        if (!storage_initialized[video_index] || size_changed) {
            // If storage was already initialized and size changed, we MUST recreate textures
            // because glTexStorage2D creates immutable storage
            if (storage_initialized[video_index] && size_changed) {
                LOG_DEBUG("GL", "Resolution changed from %dx%d to %dx%d - recreating textures",
                         stored_width[video_index], stored_height[video_index], width, height);
                // Delete old textures
                glDeleteTextures(1, &tex_y);
                glDeleteTextures(1, &tex_u);
                glDeleteTextures(1, &tex_v);
                // Regenerate texture names
                glGenTextures(1, &tex_y);
                glGenTextures(1, &tex_u);
                glGenTextures(1, &tex_v);
                // Update gl_context texture references
                if (video_index == 0) {
                    gl->texture_y = tex_y;
                    gl->texture_u = tex_u;
                    gl->texture_v = tex_v;
                } else {
                    gl->texture_y2 = tex_y;
                    gl->texture_u2 = tex_u;
                    gl->texture_v2 = tex_v;
                }
            }
            
            // First time or resolution changed - allocate storage
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex_y);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
            
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tex_u);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, uv_width, uv_height);
            
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, tex_v);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, uv_width, uv_height);
            
            storage_initialized[video_index] = true;
            stored_width[video_index] = width;
            stored_height[video_index] = height;
        }
        
        // Direct texture upload (PBO code removed for stability)
        // PRODUCTION OPTIMIZATION: Allocate buffers once before all texture uploads
        // This avoids 3 allocation checks per frame
        if (!y_direct || !u_direct || !v_direct) {
            int needed_size = width * height;  // Y plane size
            allocate_yuv_buffers(needed_size);
        }

        // Y texture upload - Use GL_UNPACK_ROW_LENGTH to let GPU handle stride (eliminates CPU memcpy)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_y);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, y_direct ? 0 : y_stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, y_data);

        // U texture upload - Use GL_UNPACK_ROW_LENGTH to let GPU handle stride
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_u);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, u_direct ? 0 : u_stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, u_data);

        // V texture upload - Use GL_UNPACK_ROW_LENGTH to let GPU handle stride
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex_v);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, v_direct ? 0 : v_stride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, v_data);

        // Reset GL_UNPACK_ROW_LENGTH once at end (reduces GL state changes from 6 to 4 calls)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        if (frame_rendered[video_index] == 0) {
            LOG_INFO("GL", "GPU YUV→RGB rendering started (%dx%d)", width, height);
        }
        last_width[video_index] = width;
        last_height[video_index] = height;

        frame_rendered[video_index]++;
    } else if (frame_rendered[video_index] == 0) {
        // Skip test pattern for now - YUV textures need proper initialization
        // (Don't print "Waiting for video data..." - it's just noise)
    }
    
    clock_gettime(CLOCK_MONOTONIC, &tex_end);
    double tex_upload_time = (tex_end.tv_sec - tex_start.tv_sec) + (tex_end.tv_nsec - tex_start.tv_nsec) / 1e9;
    
    // TIMER: Measure draw call time
    struct timespec draw_start, draw_end;
    clock_gettime(CLOCK_MONOTONIC, &draw_start);
    
    // Draw quad only if we have valid texture data
    if (frame_rendered[video_index] > 0) {
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &draw_end);
    double draw_time = (draw_end.tv_sec - draw_start.tv_sec) + (draw_end.tv_nsec - draw_start.tv_nsec) / 1e9;
    
    // PRODUCTION: Diagnostic output only on first 3 slow frames
    static int frame_diag = 0;
    if (frame_diag < 3 && (tex_upload_time > 0.008 || draw_time > 0.010)) {
        LOG_DEBUG("GL", "Video%d - Upload: %.1fms, Draw: %.1fms",
               video_index, tex_upload_time * 1000, draw_time * 1000);
        frame_diag++;
        if (frame_diag == 3) {
            LOG_DEBUG("GL", "Further render timing available with --timing flag");
        }
    }
}
void gl_swap_buffers(gl_context_t *gl, struct display_ctx *drm) {
    static int swap_count = 0;
    static struct timespec last_swap_time = {0, 0};
    struct timespec t1, t2;
    
    // Measure swap time for diagnostics
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    // EGL swap buffers (this makes the rendered frame available and blocks on VSync)
    EGLBoolean swap_result = eglSwapBuffers(gl->egl_display, gl->egl_surface);
    if (!swap_result && swap_count < 5) {
        LOG_ERROR("GL", "EGL swap failed: 0x%x", eglGetError());
        clock_gettime(CLOCK_MONOTONIC, &last_swap_time);
        return;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &t2);
    
    // Calculate swap time (includes VSync wait)
    double swap_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + 
                     (t2.tv_nsec - t1.tv_nsec) / 1000000.0;
    
    // Warn on long swaps (indicates late arrival to VBlank)
    if (swap_ms > 20.0 && swap_count > 10) {
        LOG_WARN("GL", "Long swap: %.1fms (late frame, missed VBlank window)", swap_ms);
    }

    // Present to display via DRM
    if (drm_swap_buffers(drm) != 0 && swap_count < 5) {
        LOG_ERROR("DRM", "DRM swap failed");
    }
    
    last_swap_time = t2;
    swap_count++;
}

int gl_create_shaders(gl_context_t *gl) {
    return create_program(gl);
}

void gl_render_corners(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_corners) {
        return; // Don't render corners if not visible
    }

    // PRODUCTION FIX: Separate VBOs and state tracking for each keystone
    // This prevents flickering when rendering corners for dual keystones
    static float corner_vertices1[10000];  // Keystone 1 vertex buffer
    static float corner_vertices2[10000];  // Keystone 2 vertex buffer
    static GLuint corner_vbo1 = 0;
    static GLuint corner_vbo2 = 0;
    static bool vbo_initialized = false;

    // Per-keystone state tracking (indexed by keystone: 0 = first seen, 1 = second seen)
    static keystone_context_t *keystone_ptrs[2] = {NULL, NULL};
    static int cached_selected_corners[2] = {-2, -2};
    static bool last_show_corners[2] = {false, false};

    // Initialize VBOs once on first call
    if (!vbo_initialized) {
        glGenBuffers(1, &corner_vbo1);
        glBindBuffer(GL_ARRAY_BUFFER, corner_vbo1);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corner_vertices1), NULL, GL_DYNAMIC_DRAW);

        glGenBuffers(1, &corner_vbo2);
        glBindBuffer(GL_ARRAY_BUFFER, corner_vbo2);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corner_vertices2), NULL, GL_DYNAMIC_DRAW);

        vbo_initialized = true;
    }

    // Determine which keystone index (0 or 1) based on pointer
    int keystone_idx = -1;
    if (keystone == keystone_ptrs[0] || keystone_ptrs[0] == NULL) {
        keystone_idx = 0;
        keystone_ptrs[0] = keystone;
    } else if (keystone == keystone_ptrs[1] || keystone_ptrs[1] == NULL) {
        keystone_idx = 1;
        keystone_ptrs[1] = keystone;
    } else {
        // Fallback: use index 0 if pointer doesn't match either
        keystone_idx = 0;
    }

    // Select the appropriate VBO and vertex buffer for this keystone
    GLuint corner_vbo = (keystone_idx == 0) ? corner_vbo1 : corner_vbo2;
    float *corner_vertices = (keystone_idx == 0) ? corner_vertices1 : corner_vertices2;

    // Check if update needed for THIS specific keystone
    bool visibility_changed = (last_show_corners[keystone_idx] != keystone->show_corners);
    bool selection_changed = (cached_selected_corners[keystone_idx] != keystone->selected_corner);
    bool needs_update = keystone->corners_dirty || visibility_changed || selection_changed;

    // Update cached state for THIS keystone
    last_show_corners[keystone_idx] = keystone->show_corners;
    
    // Always prepare corner colors (outside needs_update so available for rendering)
    float corner_colors[4][4];
    for (int i = 0; i < 4; i++) {
        if (keystone->selected_corner == i) {
            // Bright green for selected - semi-transparent to show border
            corner_colors[i][0] = 0.0f;
            corner_colors[i][1] = 1.0f;
            corner_colors[i][2] = 0.0f;
            corner_colors[i][3] = 0.5f;  // 50% opacity for selected
        } else {
            // White with transparency for unselected - allow border to show through
            corner_colors[i][0] = 1.0f;
            corner_colors[i][1] = 1.0f;
            corner_colors[i][2] = 1.0f;
            corner_colors[i][3] = 0.3f;  // 30% opacity for unselected
        }
    }
    
    if (needs_update) {
        cached_selected_corners[keystone_idx] = keystone->selected_corner;
        keystone->corners_dirty = false;  // Clear dirty flag
        
        // Create corner positions (small squares)
        float corner_size = 0.008f; // 0.8% of screen size (refined and elegant)
        
        // Corner vertices with colors: [x, y, r, g, b, a] per vertex
        int vertex_count = 0;
        
        // Render corner indicators at the keystone corner positions
        for (int i = 0; i < 4; i++) {
            // Get the keystone corner position
            float tx = keystone->corners[i].x;
            float ty = keystone->corners[i].y;
            float *color = corner_colors[i];
            
            // Create a small square with per-vertex colors (6 floats per vertex: x, y, r, g, b, a)
            if (vertex_count + 4 <= 1600) { // Leave room for text
                // Bottom-left
                corner_vertices[vertex_count*6 + 0] = tx - corner_size;
                corner_vertices[vertex_count*6 + 1] = ty - corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Bottom-right
                corner_vertices[vertex_count*6 + 0] = tx + corner_size;
                corner_vertices[vertex_count*6 + 1] = ty - corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Top-right
                corner_vertices[vertex_count*6 + 0] = tx + corner_size;
                corner_vertices[vertex_count*6 + 1] = ty + corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
                
                // Top-left
                corner_vertices[vertex_count*6 + 0] = tx - corner_size;
                corner_vertices[vertex_count*6 + 1] = ty + corner_size;
                corner_vertices[vertex_count*6 + 2] = color[0];
                corner_vertices[vertex_count*6 + 3] = color[1];
                corner_vertices[vertex_count*6 + 4] = color[2];
                corner_vertices[vertex_count*6 + 5] = color[3];
                vertex_count++;
            }
        }
        
        // OPTIMIZED: Use glBufferSubData instead of glBufferData
        // glBufferSubData only updates the data, much faster than reallocating
        glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_count * 6 * sizeof(float), corner_vertices);
    }  // End needs_update block
    
    // Always bind and render (even if not updated)
    glBindBuffer(GL_ARRAY_BUFFER, corner_vbo);
    
    // Enable blending for transparent overlays
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);
    
    // Use corner shader program
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes - interleaved position (2) + color (4)
    int stride = 6 * sizeof(float);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Enable color attribute (location 1)
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP (corners in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Draw all corner squares - each corner is 4 vertices
    for (int i = 0; i < 4; i++) {
        glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
    }
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1); // Disable color attribute
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    // CRITICAL: Restore GL state - unbind VBO to prevent interference
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_render_border(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_border) {
        return; // Don't render border if not visible
    }
    
    // Enable blending for transparent overlays
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);
    
    // Create border line vertices with color (4 lines connecting the corners)
    // Each vertex: x, y, r, g, b, a (6 floats per vertex)
    float border_vertices[48]; // 8 vertices * 6 floats = 48 floats
    
    // Yellow color for all border vertices
    float r = 1.0f, g = 1.0f, b = 0.0f, a = 1.0f;
    
    // Show the control point positions (where user has positioned corners)
    point_t *corners = keystone->corners;
    
    // Line 1: Top-left to top-right (use direct Y coordinates)
    border_vertices[0] = corners[CORNER_TOP_LEFT].x;
    border_vertices[1] = corners[CORNER_TOP_LEFT].y;
    border_vertices[2] = r; border_vertices[3] = g; border_vertices[4] = b; border_vertices[5] = a;
    border_vertices[6] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[7] = corners[CORNER_TOP_RIGHT].y;
    border_vertices[8] = r; border_vertices[9] = g; border_vertices[10] = b; border_vertices[11] = a;
    
    // Line 2: Top-right to bottom-right
    border_vertices[12] = corners[CORNER_TOP_RIGHT].x;
    border_vertices[13] = corners[CORNER_TOP_RIGHT].y;
    border_vertices[14] = r; border_vertices[15] = g; border_vertices[16] = b; border_vertices[17] = a;
    border_vertices[18] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[19] = corners[CORNER_BOTTOM_RIGHT].y;
    border_vertices[20] = r; border_vertices[21] = g; border_vertices[22] = b; border_vertices[23] = a;
    
    // Line 3: Bottom-right to bottom-left
    border_vertices[24] = corners[CORNER_BOTTOM_RIGHT].x;
    border_vertices[25] = corners[CORNER_BOTTOM_RIGHT].y;
    border_vertices[26] = r; border_vertices[27] = g; border_vertices[28] = b; border_vertices[29] = a;
    border_vertices[30] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[31] = corners[CORNER_BOTTOM_LEFT].y;
    border_vertices[32] = r; border_vertices[33] = g; border_vertices[34] = b; border_vertices[35] = a;
    
    // Line 4: Bottom-left to top-left
    border_vertices[36] = corners[CORNER_BOTTOM_LEFT].x;
    border_vertices[37] = corners[CORNER_BOTTOM_LEFT].y;
    border_vertices[38] = r; border_vertices[39] = g; border_vertices[40] = b; border_vertices[41] = a;
    border_vertices[42] = corners[CORNER_TOP_LEFT].x;
    border_vertices[43] = corners[CORNER_TOP_LEFT].y;
    border_vertices[44] = r; border_vertices[45] = g; border_vertices[46] = b; border_vertices[47] = a;
    
    // Update border VBO with new positions - OPTIMIZED: use glBufferSubData
    glBindBuffer(GL_ARRAY_BUFFER, gl->border_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(border_vertices), border_vertices);
    
    // Use corner shader program (same as corners, just different geometry)
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float); // x, y, r, g, b, a
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP (border in normalized coordinates)
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Set line width for refined appearance
    glLineWidth(2.0f);
    
    // Draw border as 4 separate lines (8 vertices with colors from vertex data)
    glDrawArrays(GL_LINES, 0, 8); // 8 vertices (4 lines * 2 vertices each)
    
    // Reset line width
    glLineWidth(1.0f);
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    // NOTE: Don't unbind buffers here - video_player.c handles complete state restoration
}

// Render display boundary (red rectangle showing max projector/display area)
void gl_render_display_boundary(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_border) {
        return; // Don't render boundary if border not visible
    }
    
    // Enable blending for transparent overlay
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable depth testing to ensure overlay is always visible
    glDisable(GL_DEPTH_TEST);
    
    // Create boundary line vertices - rectangle covering full display
    // Normalized coordinates: (-1, -1) to (1, 1)
    // Each vertex: x, y, r, g, b, a (6 floats per vertex)
    float boundary_vertices[48]; // 8 vertices * 6 floats = 48 floats
    
    // Red color for boundary
    float r = 1.0f, g = 0.0f, b = 0.0f, a = 0.8f;  // 80% opaque red
    
    // Line 1: Top edge (left to right)
    boundary_vertices[0] = -1.0f;   // top-left x
    boundary_vertices[1] = 1.0f;    // top-left y
    boundary_vertices[2] = r; boundary_vertices[3] = g; boundary_vertices[4] = b; boundary_vertices[5] = a;
    
    boundary_vertices[6] = 1.0f;    // top-right x
    boundary_vertices[7] = 1.0f;    // top-right y
    boundary_vertices[8] = r; boundary_vertices[9] = g; boundary_vertices[10] = b; boundary_vertices[11] = a;
    
    // Line 2: Right edge (top to bottom)
    boundary_vertices[12] = 1.0f;   // top-right x
    boundary_vertices[13] = 1.0f;   // top-right y
    boundary_vertices[14] = r; boundary_vertices[15] = g; boundary_vertices[16] = b; boundary_vertices[17] = a;
    
    boundary_vertices[18] = 1.0f;   // bottom-right x
    boundary_vertices[19] = -1.0f;  // bottom-right y
    boundary_vertices[20] = r; boundary_vertices[21] = g; boundary_vertices[22] = b; boundary_vertices[23] = a;
    
    // Line 3: Bottom edge (right to left)
    boundary_vertices[24] = 1.0f;   // bottom-right x
    boundary_vertices[25] = -1.0f;  // bottom-right y
    boundary_vertices[26] = r; boundary_vertices[27] = g; boundary_vertices[28] = b; boundary_vertices[29] = a;
    
    boundary_vertices[30] = -1.0f;  // bottom-left x
    boundary_vertices[31] = -1.0f;  // bottom-left y
    boundary_vertices[32] = r; boundary_vertices[33] = g; boundary_vertices[34] = b; boundary_vertices[35] = a;
    
    // Line 4: Left edge (bottom to top)
    boundary_vertices[36] = -1.0f;  // bottom-left x
    boundary_vertices[37] = -1.0f;  // bottom-left y
    boundary_vertices[38] = r; boundary_vertices[39] = g; boundary_vertices[40] = b; boundary_vertices[41] = a;
    
    boundary_vertices[42] = -1.0f;  // top-left x
    boundary_vertices[43] = 1.0f;   // top-left y
    boundary_vertices[44] = r; boundary_vertices[45] = g; boundary_vertices[46] = b; boundary_vertices[47] = a;
    
    // Create temporary VBO for boundary if needed
    static GLuint boundary_vbo = 0;
    if (boundary_vbo == 0) {
        glGenBuffers(1, &boundary_vbo);
    }
    
    // Update boundary VBO
    glBindBuffer(GL_ARRAY_BUFFER, boundary_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(boundary_vertices), boundary_vertices, GL_DYNAMIC_DRAW);
    
    // Use corner shader program
    glUseProgram(gl->corner_program);
    
    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    
    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Set identity matrix for MVP
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Set line width - thinner than keystone border for clarity
    glLineWidth(1.5f);
    
    // Draw boundary as 4 lines (8 vertices)
    glDrawArrays(GL_LINES, 0, 8);
    
    // Reset line width
    glLineWidth(1.0f);
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
static const unsigned char font_5x7[128][7] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00},
    ['/'] = {0x08, 0x08, 0x10, 0x20, 0x40, 0x40, 0x00},
    [':'] = {0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x00},
    ['-'] = {0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00},
    ['('] = {0x10, 0x20, 0x20, 0x20, 0x20, 0x10, 0x00},
    [')'] = {0x20, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00},
    ['0'] = {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x70, 0x00},
    ['1'] = {0x20, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00},
    ['2'] = {0x70, 0x88, 0x08, 0x30, 0x40, 0xF8, 0x00},
    ['3'] = {0x70, 0x88, 0x30, 0x08, 0x88, 0x70, 0x00},
    ['4'] = {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x00},
    ['5'] = {0xF8, 0x80, 0xF0, 0x08, 0x88, 0x70, 0x00},
    ['6'] = {0x30, 0x40, 0x80, 0xF0, 0x88, 0x70, 0x00},
    ['7'] = {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x00},
    ['8'] = {0x70, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00},
    ['9'] = {0x70, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00},
    ['A'] = {0x20, 0x50, 0x88, 0x88, 0xF8, 0x88, 0x00},
    ['B'] = {0xF0, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00},
    ['C'] = {0x70, 0x88, 0x80, 0x80, 0x88, 0x70, 0x00},
    ['D'] = {0xF0, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00},
    ['E'] = {0xF8, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00},
    ['F'] = {0xF8, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00},
    ['G'] = {0x70, 0x88, 0x80, 0xB8, 0x88, 0x78, 0x00},
    ['H'] = {0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00},
    ['I'] = {0x70, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00},
    ['J'] = {0x38, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00},
    ['K'] = {0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x00},
    ['L'] = {0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00},
    ['M'] = {0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x00},
    ['N'] = {0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x00},
    ['O'] = {0x70, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    ['P'] = {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x00},
    ['Q'] = {0x70, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00},
    ['R'] = {0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x00},
    ['S'] = {0x70, 0x88, 0x60, 0x10, 0x88, 0x70, 0x00},
    ['T'] = {0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00},
    ['U'] = {0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00},
    ['V'] = {0x88, 0x88, 0x88, 0x50, 0x50, 0x20, 0x00},
    ['W'] = {0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88, 0x00},
    ['X'] = {0x88, 0x50, 0x20, 0x20, 0x50, 0x88, 0x00},
    ['Y'] = {0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x00},
    ['Z'] = {0xF8, 0x08, 0x10, 0x20, 0x40, 0xF8, 0x00},
    ['a'] = {0x00, 0x00, 0x70, 0x08, 0x78, 0x88, 0x78},
    ['b'] = {0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0xF0},
    ['c'] = {0x00, 0x00, 0x70, 0x88, 0x80, 0x88, 0x70},
    ['d'] = {0x08, 0x08, 0x78, 0x88, 0x88, 0x88, 0x78},
    ['e'] = {0x00, 0x00, 0x70, 0x88, 0xF8, 0x80, 0x70},
    ['f'] = {0x30, 0x48, 0x40, 0xF0, 0x40, 0x40, 0x40},
    ['g'] = {0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x70},
    ['h'] = {0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0x88},
    ['i'] = {0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70},
    ['j'] = {0x10, 0x00, 0x30, 0x10, 0x10, 0x90, 0x60},
    ['k'] = {0x80, 0x80, 0x90, 0xA0, 0xC0, 0xA0, 0x90},
    ['l'] = {0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70},
    ['m'] = {0x00, 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0xA8},
    ['n'] = {0x00, 0x00, 0xF0, 0x88, 0x88, 0x88, 0x88},
    ['o'] = {0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70},
    ['p'] = {0x00, 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80},
    ['q'] = {0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x08},
    ['r'] = {0x00, 0x00, 0xB0, 0xC8, 0x80, 0x80, 0x80},
    ['s'] = {0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xF0},
    ['t'] = {0x40, 0x40, 0xF0, 0x40, 0x40, 0x48, 0x30},
    ['u'] = {0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0x78},
    ['v'] = {0x00, 0x00, 0x88, 0x88, 0x88, 0x50, 0x20},
    ['w'] = {0x00, 0x00, 0x88, 0xA8, 0xA8, 0xA8, 0x50},
    ['x'] = {0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88},
    ['y'] = {0x00, 0x88, 0x88, 0x88, 0x78, 0x08, 0x70},
    ['z'] = {0x00, 0x00, 0xF8, 0x10, 0x20, 0x40, 0xF8},
};

static void draw_char_simple(float *vertices, int *vertex_count, char c, float x, float y, float size) {
    if (*vertex_count >= 15000) return; // Safety limit - increased for longer help text
    
    // Check bounds and handle unknown characters
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return;
    
    const unsigned char *char_data = font_5x7[uc];
    
    // Check if character has any data (handle sparse array)
    bool has_data = false;
    for (int i = 0; i < 7; i++) {
        if (char_data[i] != 0) {
            has_data = true;
            break;
        }
    }
    if (!has_data && c != ' ') return; // Skip undefined chars except space
    
    float pixel_size = size / 7.0f;  // 5x7 font, divide by 7 for height
    
    for (int row = 0; row < 7; row++) {
        unsigned char row_data = char_data[row];
        for (int col = 0; col < 8; col++) {  // Check all 8 bits
            if ((row_data >> (7-col)) & 1) { // Check if pixel is set (MSB first)
                if (*vertex_count + 4 <= 15000) {
                    float px = x + col * pixel_size;
                    float py = y - (row * pixel_size); // Top to bottom
                    
                    // Add rectangle for this pixel
                    vertices[(*vertex_count)*2] = px; 
                    vertices[(*vertex_count)*2+1] = py;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px + pixel_size; 
                    vertices[(*vertex_count)*2+1] = py;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px + pixel_size; 
                    vertices[(*vertex_count)*2+1] = py - pixel_size;
                    (*vertex_count)++;
                    vertices[(*vertex_count)*2] = px; 
                    vertices[(*vertex_count)*2+1] = py - pixel_size;
                    (*vertex_count)++;
                }
            }
        }
    }
}

static void draw_text_simple(float *vertices, int *vertex_count, const char *text, float x, float y, float size) {
    float char_width = size * 1.2f;    // Character width including spacing
    float line_height = size * 1.3f;   // Tighter line spacing to fit more text
    float current_x = x;
    float current_y = y;
    
    while (*text && *vertex_count < 15000) {
        if (*text == '\n') {
            current_x = x;
            current_y -= line_height;
        } else {
            draw_char_simple(vertices, vertex_count, *text, current_x, current_y, size);
            current_x += char_width;
        }
        text++;
    }
}

void gl_render_help_overlay(gl_context_t *gl, keystone_context_t *keystone) {
    if (!keystone || !keystone->show_help) {
        return; // Don't render help overlay if not visible
    }

    // OPTIMIZATION: Cache text geometry - only generate once on first display
    static bool help_initialized = false;
    static float help_bg_vertices[24]; // Background rectangle
    static float help_text_vertices[30000]; // Text geometry
    static int text_vertex_count = 0;

    if (!help_initialized) {
        // Generate background vertices (only once)
        // Bottom-left
        help_bg_vertices[0] = -0.9f; help_bg_vertices[1] = -0.7f;
        help_bg_vertices[2] = 0.0f; help_bg_vertices[3] = 0.0f; help_bg_vertices[4] = 0.0f; help_bg_vertices[5] = 0.95f;
        // Bottom-right
        help_bg_vertices[6] = 0.9f; help_bg_vertices[7] = -0.7f;
        help_bg_vertices[8] = 0.0f; help_bg_vertices[9] = 0.0f; help_bg_vertices[10] = 0.0f; help_bg_vertices[11] = 0.95f;
        // Top-right
        help_bg_vertices[12] = 0.9f; help_bg_vertices[13] = 0.7f;
        help_bg_vertices[14] = 0.0f; help_bg_vertices[15] = 0.0f; help_bg_vertices[16] = 0.0f; help_bg_vertices[17] = 0.95f;
        // Top-left
        help_bg_vertices[18] = -0.9f; help_bg_vertices[19] = 0.7f;
        help_bg_vertices[20] = 0.0f; help_bg_vertices[21] = 0.0f; help_bg_vertices[22] = 0.0f; help_bg_vertices[23] = 0.95f;
    
        // Generate text geometry (only once)
        const char* help_text =
            "Copyright Dilworth Creative LLC\n"
            "\n"
            "PICKLE KEYSTONE\n"
            "\n"
            "GAMEPAD\n"
            "X      Cycle Corner\n"
            "DPAD   Move Corner\n"
            "B      Show Keysone Border\n"
            "Y      Show Help\n"
            "L1     Step Down\n"
            "R1     Step Up\n"
            "START  Save\n"
            "SELECT Reset Keystone";

        draw_text_simple(help_text_vertices, &text_vertex_count, help_text, -0.85f, 0.62f, 0.022f);
        help_initialized = true;
        LOG_DEBUG("GL", "HELP text geometry generated: %d vertices (cached for reuse)", text_vertex_count);
    }

    // Now render the cached geometry (fast!)
    glBindBuffer(GL_ARRAY_BUFFER, gl->help_vbo);
    
    // Upload cached background geometry
    glBufferData(GL_ARRAY_BUFFER, 4 * 6 * sizeof(float), help_bg_vertices, GL_DYNAMIC_DRAW);

    // Use corner shader program for help overlay rendering
    glUseProgram(gl->corner_program);

    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float); // x, y, r, g, b, a
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);

    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Set identity matrix for MVP
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);

    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Disable depth testing to ensure overlays are always visible
    glDisable(GL_DEPTH_TEST);

    // Draw help overlay background (semi-transparent black from vertex colors)
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4); // Background rectangle

    // OPTIMIZATION: Reuse cached text geometry (no regeneration every frame!)
    // Create bright white color vertices for text using cached geometry
    static float colored_vertices[180000]; // Persistent buffer
    static bool colors_initialized = false;

    if (!colors_initialized) {
        // Only add colors once
        for (int i = 0; i < text_vertex_count && i * 6 < 180000; i++) {
            colored_vertices[i * 6 + 0] = help_text_vertices[i * 2 + 0]; // x
            colored_vertices[i * 6 + 1] = help_text_vertices[i * 2 + 1]; // y
            colored_vertices[i * 6 + 2] = 1.0f; // r - bright white
            colored_vertices[i * 6 + 3] = 1.0f; // g
            colored_vertices[i * 6 + 4] = 1.0f; // b
            colored_vertices[i * 6 + 5] = 1.0f; // a
        }
        colors_initialized = true;
    }

    // Upload cached text with colors (just one memcpy, no regeneration!)
    glBufferData(GL_ARRAY_BUFFER, text_vertex_count * 6 * sizeof(float), colored_vertices, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // OPTIMIZATION: Draw all text in ONE batched call instead of 3000+ separate calls!
    // Each rectangle is 4 vertices (triangle fan), draw them all as quads
    // Use GL_POINTS with point sprites would be better, but GLES doesn't support it well
    // So we'll convert to indexed triangles for a single draw call
    static GLuint text_indices_vbo = 0;
    static int last_text_vertex_count = 0;

    if (text_indices_vbo == 0) {
        glGenBuffers(1, &text_indices_vbo);
    }

    // Generate indices if needed (convert quads to triangles)
    if (text_vertex_count != last_text_vertex_count) {
        int num_quads = text_vertex_count / 4;
        GLuint *indices = malloc(num_quads * 6 * sizeof(GLuint)); // 2 triangles per quad

        for (int i = 0; i < num_quads; i++) {
            int base = i * 4;
            indices[i * 6 + 0] = base + 0;
            indices[i * 6 + 1] = base + 1;
            indices[i * 6 + 2] = base + 2;
            indices[i * 6 + 3] = base + 0;
            indices[i * 6 + 4] = base + 2;
            indices[i * 6 + 5] = base + 3;
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, text_indices_vbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, num_quads * 6 * sizeof(GLuint), indices, GL_STATIC_DRAW);
        free(indices);
        last_text_vertex_count = text_vertex_count;
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, text_indices_vbo);
    }

    // Draw ALL text in ONE call!
    int num_quads = text_vertex_count / 4;
    glDrawElements(GL_TRIANGLES, num_quads * 6, GL_UNSIGNED_INT, 0);
    
    // Disable blending and restore depth testing
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);

    // CRITICAL: Restore GL state - unbind VBO to prevent interference
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gl_render_notification_overlay(gl_context_t *gl, const char *message) {
    if (!gl || !message) return;

    // Create notification overlay with background and text
    float notify_vertices[1000]; // Buffer for background vertices with color
    int vertex_count = 0;

    // Centered notification box with darker green background for better readability
    // Bottom-left
    notify_vertices[0] = -0.35f; notify_vertices[1] = -0.15f;
    notify_vertices[2] = 0.0f; notify_vertices[3] = 0.6f; notify_vertices[4] = 0.0f; notify_vertices[5] = 0.95f;
    // Bottom-right
    notify_vertices[6] = 0.35f; notify_vertices[7] = -0.15f;
    notify_vertices[8] = 0.0f; notify_vertices[9] = 0.6f; notify_vertices[10] = 0.0f; notify_vertices[11] = 0.95f;
    // Top-right
    notify_vertices[12] = 0.35f; notify_vertices[13] = 0.15f;
    notify_vertices[14] = 0.0f; notify_vertices[15] = 0.6f; notify_vertices[16] = 0.0f; notify_vertices[17] = 0.95f;
    // Top-left
    notify_vertices[18] = -0.35f; notify_vertices[19] = 0.15f;
    notify_vertices[20] = 0.0f; notify_vertices[21] = 0.6f; notify_vertices[22] = 0.0f; notify_vertices[23] = 0.95f;
    vertex_count = 4;

    // Upload background geometry
    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * 6 * sizeof(float), notify_vertices, GL_DYNAMIC_DRAW);

    // Use corner shader program
    glUseProgram(gl->corner_program);

    // Set up vertex attributes (interleaved position + color)
    int stride = 6 * sizeof(float); // x, y, r, g, b, a
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);

    // Color attribute at location 1
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Set identity matrix for MVP
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);

    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    // Draw notification background
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    // Render notification text centered
    float text_vertices[3000];
    int text_vertex_count = 0;
    draw_text_simple(text_vertices, &text_vertex_count, message, -0.32f, 0.02f, 0.035f);

    // Create colored vertices for text (white color)
    float colored_vertices[18000]; // 3000 * 6
    for (int i = 0; i < text_vertex_count && i * 6 < 18000; i++) {
        colored_vertices[i * 6 + 0] = text_vertices[i * 2 + 0]; // x
        colored_vertices[i * 6 + 1] = text_vertices[i * 2 + 1]; // y
        colored_vertices[i * 6 + 2] = 1.0f; // r - white color
        colored_vertices[i * 6 + 3] = 1.0f; // g
        colored_vertices[i * 6 + 4] = 1.0f; // b
        colored_vertices[i * 6 + 5] = 1.0f; // a
    }

    // Upload text with colors
    glBufferData(GL_ARRAY_BUFFER, text_vertex_count * 6 * sizeof(float), colored_vertices, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Draw text pixels
    int text_pixels = text_vertex_count / 4;
    for (int i = 0; i < text_pixels; i++) {
        glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
    }

    // Restore GL state
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// WiFi overlay function disabled - requires wifi_manager_t which is not defined
#if 0
void gl_render_wifi_overlay(gl_context_t *gl, wifi_manager_t *mgr) {
    if (!gl || !mgr) return;
    
    // DEBUG: Log what mgr pointer value we received
    #ifndef NDEBUG
    static int render_call_count = 0;
    if (render_call_count++ % 60 == 0) {
        LOG_TRACE("WIFI", "Render called - mgr pointer=%p, selected_index=%d", (void*)mgr, mgr->selected_index);
    }
    #endif

    // Print WiFi menu to console every 60 frames (every 1-2 seconds depending on video FPS)
    static int print_counter = 0;
    if (print_counter++ % 60 == 0) {
        LOG_DEBUG("WIFI", "=== WiFi Networks ===");
        for (int i = 0; i < mgr->network_count && i < 5; i++) {
            LOG_DEBUG("WIFI", "  [%d] %s - Signal: %d%% %s",
                   i, mgr->networks[i].ssid, (int)mgr->networks[i].signal_strength,
                   i == mgr->selected_index ? "← SELECTED" : "");
        }
        LOG_DEBUG("WIFI", "Use D-pad UP/DOWN to select, SELECT button to confirm");
        LOG_DEBUG("WIFI", "======================");
    }
    
    // Enable blending for semi-transparent overlay
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    
    glUseProgram(gl->corner_program);
    
    // Set MVP matrix
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    glUniformMatrix4fv(gl->corner_u_mvp_matrix, 1, GL_FALSE, identity);
    
    // Render full-screen semi-transparent black background
    float bg_vertices[24] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
         1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
         1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.9f,
        -1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.9f
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bg_vertices), bg_vertices, GL_DYNAMIC_DRAW);
    
    int stride = 6 * sizeof(float);
    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(gl->corner_a_position);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    
    // Draw a highlight bar behind the currently selected network line (only in list mode)
    // This makes selection visible even if the '>' glyph isn't present in the font
    if (mgr->state == WIFI_STATE_NETWORK_LIST) {
        // Text layout parameters must match draw_text_simple()
        const float base_y = 0.5f;
        const float text_size = 0.025f;
        const float line_height = text_size * 1.3f;
        // Header (WiFi Networks) + blank line before networks start
        int selected_line_index = 2 + mgr->selected_index; // 0-based from top
        float line_y = base_y - selected_line_index * line_height;
        
        // Highlight rectangle extents
        float rect_left = -0.9f;
        float rect_right = 0.5f;
        float rect_top = line_y + (line_height * 0.55f);
        float rect_bottom = line_y - (line_height * 0.55f);
        
        // Semi-transparent green highlight
        float hl_vertices[24] = {
            rect_left,  rect_bottom, 0.0f, 0.6f, 0.2f, 0.45f,
            rect_right, rect_bottom, 0.0f, 0.6f, 0.2f, 0.45f,
            rect_right, rect_top,    0.0f, 0.6f, 0.2f, 0.45f,
            rect_left,  rect_top,    0.0f, 0.6f, 0.2f, 0.45f
        };
        
        glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(hl_vertices), hl_vertices, GL_DYNAMIC_DRAW);
        int hl_stride = 6 * sizeof(float);
        glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, hl_stride, (void*)0);
        glEnableVertexAttribArray(gl->corner_a_position);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, hl_stride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    
    // Build WiFi overlay text depending on state
    char wifi_text[1024];
    int offset = 0;
    if (mgr->state == WIFI_STATE_NETWORK_LIST) {
        offset = snprintf(wifi_text, sizeof(wifi_text), "WiFi Networks\n\n");
        for (int i = 0; i < mgr->network_count && i < 5; i++) {
            if (i == mgr->selected_index) {
                offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                                 "> %s (%d%%)\n", mgr->networks[i].ssid, 
                                 (int)mgr->networks[i].signal_strength);
            } else {
                offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                                 "  %s (%d%%)\n", mgr->networks[i].ssid,
                                 (int)mgr->networks[i].signal_strength);
            }
        }
        snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                 "\nArrows: Select\nEnter/SELECT: Choose");
    } else if (mgr->state == WIFI_STATE_PASSWORD_ENTRY) {
        // Password display (masked or visible)
        char shown[256];
        if (mgr->show_password) {
            snprintf(shown, sizeof(shown), "%s", mgr->password);
        } else {
            int len = (mgr->password_length >= 0) ? mgr->password_length : 0;
            if (len > (int)sizeof(shown) - 1) len = (int)sizeof(shown) - 1;
            memset(shown, '*', len);
            shown[len] = '\0';
        }

        offset = snprintf(wifi_text, sizeof(wifi_text),
                          "WiFi Password\n\nSSID: %s\nPassword: %s (%s)\n\n",
                          mgr->networks[mgr->selected_index].ssid,
                          shown,
                          mgr->show_password ? "visible" : "hidden");
        offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                           "Arrows: Move  Enter/SELECT: Press Key  Backspace: Delete\n");
        if (mgr->status[0] != '\0') {
            // Append status/error message
            offset += snprintf(wifi_text + offset, sizeof(wifi_text) - offset,
                               "\n%s\n",
                               mgr->status);
        }
        
        // After the text, draw a simple on-screen keyboard grid
        // Layout is in mgr->keyboard_layout[4] with varying row lengths
    const float key_text_size = 0.028f;
    const float start_y = 0.12f; // slightly lower to reduce overlap
    const float row_gap = key_text_size * 2.6f;
    const float key_w = 0.11f;   // slightly smaller keys
    const float key_h = row_gap * 0.45f; // a bit shorter for clearer rows
    const float key_gap = 0.02f; // spacing between keys
        
    // Draw each key; render selection background before its label to avoid tinting

        for (int r = 0; r < 4; ++r) {
            int row_len = (int)strlen(mgr->keyboard_layout[r]);
            // Center the row based on total width including gaps
            float row_total_w = row_len * key_w + (row_len - 1) * key_gap;
            float row_offset_x = -0.5f * row_total_w;
            float y = start_y - r * row_gap;
            for (int c = 0; c < row_len; ++c) {
                float x = row_offset_x + c * (key_w + key_gap);
                // Highlight if selected
                int cursor_row = mgr->keyboard_cursor / 12;
                int cursor_col = mgr->keyboard_cursor % 12;
                bool is_sel = (cursor_row == r && cursor_col == c);
                
                // Base key background (dark grey)
                float base_r = 0.05f, base_g = 0.05f, base_b = 0.05f, base_a = 0.55f;
                
                // Draw key rectangle
                float rect[24] = {
                    x,           y - key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x + key_w,   y - key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x + key_w,   y + key_h, 0.0f, base_r, base_g, base_b, base_a,
                    x,           y + key_h, 0.0f, base_r, base_g, base_b, base_a
                };
                glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(rect), rect, GL_DYNAMIC_DRAW);
                glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(gl->corner_a_position);
                glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                glEnableVertexAttribArray(1);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                if (is_sel) {
                    // To avoid any translucent artifacts, draw selection with blending temporarily disabled
                    glDisable(GL_BLEND);
                    float sel_rcol = 0.20f, sel_gcol = 0.85f, sel_bcol = 0.30f, sel_acol = 1.0f;
                    float srect[24] = {
                        x,         y - key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x + key_w, y - key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x + key_w, y + key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol,
                        x,         y + key_h, 0.0f, sel_rcol, sel_gcol, sel_bcol, sel_acol
                    };
                    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof(srect), srect, GL_DYNAMIC_DRAW);
                    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                    glEnableVertexAttribArray(gl->corner_a_position);
                    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                    glEnableVertexAttribArray(1);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                    // Border
                    float bt = (key_w < key_h ? key_w : key_h) * 0.10f;
                    float br = 1.0f, bg = 1.0f, bb = 0.2f, ba = 1.0f;
                    float b_top[24] = {
                        x - bt,      y + key_h + bt, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h + bt, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h,    0.0f, br, bg, bb, ba,
                        x - bt,      y + key_h,      0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_top), b_top, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_bot[24] = {
                        x - bt,      y - key_h,      0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h,   0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h - bt, 0.0f, br, bg, bb, ba,
                        x - bt,      y - key_h - bt, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_bot), b_bot, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_left[24] = {
                        x - bt, y - key_h, 0.0f, br, bg, bb, ba,
                        x,      y - key_h, 0.0f, br, bg, bb, ba,
                        x,      y + key_h, 0.0f, br, bg, bb, ba,
                        x - bt, y + key_h, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_left), b_left, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    float b_right[24] = {
                        x + key_w, y - key_h, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y - key_h, 0.0f, br, bg, bb, ba,
                        x + key_w + bt, y + key_h, 0.0f, br, bg, bb, ba,
                        x + key_w, y + key_h, 0.0f, br, bg, bb, ba
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(b_right), b_right, GL_DYNAMIC_DRAW);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                    glEnable(GL_BLEND);
                }

                // Draw label
                char label[8] = {0};
                char k = mgr->keyboard_layout[r][c];
                if (k == '_') { label[0] = 'S'; label[1] = 'P'; }
                else if (k == '<') { label[0] = 'B'; label[1] = 'K'; label[2] = 'S'; label[3] = 'P'; }
                else if (k == '>') { label[0] = 'O'; label[1] = 'K'; }
                else if (k == '!') {
                    if (mgr->show_password) { label[0] = 'H'; label[1] = 'I'; label[2] = 'D'; label[3] = 'E'; }
                    else { label[0] = 'S'; label[1] = 'H'; label[2] = 'O'; label[3] = 'W'; }
                }
                else label[0] = k;
                int label_len = (int)strlen(label);
                float max_fit_size = (key_w * 0.80f) / (1.2f * (label_len > 0 ? label_len : 1));
                float label_size = max_fit_size;
                if (label_size > 0.040f) label_size = 0.040f;
                if (label_size < 0.022f) label_size = 0.022f;
                float total_w = (label_len ? label_len : 1) * label_size * 1.2f;
                float tx = x + (key_w - total_w) * 0.5f;
                float ty = y + label_size * 0.2f; // better vertical centering for our font

                float text_buf[512];
                int vcount = 0;
                // Slightly increase label size when selected
                float eff_label_size = is_sel ? (label_size * 1.1f) : label_size;
                draw_text_simple(text_buf, &vcount, label, tx, ty, eff_label_size);
                if (vcount > 0) {
                    float colored[4096];
                    int cc = 0;
                    for (int i = 0; i < vcount && (cc + 6) < 4096; ++i) {
                        colored[cc++] = text_buf[i * 2 + 0];
                        colored[cc++] = text_buf[i * 2 + 1];
                        // Invert label color when selected (black on bright key)
                        float tr = is_sel ? 0.0f : 1.0f;
                        float tg = is_sel ? 0.0f : 1.0f;
                        float tb = is_sel ? 0.0f : 1.0f;
                        colored[cc++] = tr;
                        colored[cc++] = tg;
                        colored[cc++] = tb;
                        colored[cc++] = 1.0f;
                    }
                    glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
                    glBufferData(GL_ARRAY_BUFFER, cc * sizeof(float), colored, GL_DYNAMIC_DRAW);
                    glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
                    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
                    glEnableVertexAttribArray(gl->corner_a_position);
                    glEnableVertexAttribArray(1);
                    int pixels = vcount / 4;
                    for (int p = 0; p < pixels; ++p) glDrawArrays(GL_TRIANGLE_FAN, p * 4, 4);
                }
            }
        }

        // (Selection already drawn per-key before labels)
    } else if (mgr->state == WIFI_STATE_SCANNING) {
        snprintf(wifi_text, sizeof(wifi_text),
                 "WiFi\n\nScanning for networks...\n\nPlease wait");
    } else {
        // Fallback: show nothing if in other states
        snprintf(wifi_text, sizeof(wifi_text), "WiFi\n");
    }
    
    // DEBUG: Log the text being rendered
    #ifndef NDEBUG
    static int wifi_text_render_count = 0;
    if (wifi_text_render_count++ % 120 == 0) {
        LOG_TRACE("WIFI", "Text rendered - selected_index=%d, First line: %.50s",
               mgr->selected_index, wifi_text);
    }
    #endif
    
    // Render text using same approach as help overlay
    float text_vertices[30000];
    int text_vertex_count = 0;
    draw_text_simple(text_vertices, &text_vertex_count, wifi_text, -0.8f, 0.5f, 0.025f);
    
    #ifndef NDEBUG
    static int detailed_log_count = 0;
    bool should_log_detailed = (detailed_log_count++ % 120 == 0);
    #else
    bool should_log_detailed = false;
    #endif
    
    if (should_log_detailed) {
        LOG_TRACE("WIFI", "Render - idx=%d, count=%d", mgr->selected_index, text_vertex_count);
        // Print line by line to see the markers clearly
        LOG_TRACE("WIFI", "=== Full WiFi Text ===\n%s", wifi_text);
        LOG_TRACE("WIFI", "====== End Text ======");
    }
    
    if (text_vertex_count > 0) {
        // Create colored vertices for text - YELLOW to be clearly visible
        float colored_vertices[180000];
        int colored_vertex_count = 0;
        for (int i = 0; i < text_vertex_count && i * 6 < 180000; i++) {
            colored_vertices[colored_vertex_count++] = text_vertices[i * 2 + 0]; // x
            colored_vertices[colored_vertex_count++] = text_vertices[i * 2 + 1]; // y
            colored_vertices[colored_vertex_count++] = 1.0f; // r - YELLOW
            colored_vertices[colored_vertex_count++] = 1.0f; // g - YELLOW
            colored_vertices[colored_vertex_count++] = 0.0f; // b - no blue
            colored_vertices[colored_vertex_count++] = 1.0f; // a
        }
        
        // Upload text with colors
        glBindBuffer(GL_ARRAY_BUFFER, gl->corner_vbo);
        glBufferData(GL_ARRAY_BUFFER, colored_vertex_count * sizeof(float), colored_vertices, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(gl->corner_a_position, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(gl->corner_a_position);
        glEnableVertexAttribArray(1);
        
        int text_pixels = text_vertex_count / 4;
        
        if (should_log_detailed) {
            LOG_TRACE("WIFI", "  Rendering %d text pixels", text_pixels);
            LOG_TRACE("WIFI", "  text_vertex_count=%d, colored_vertex_count=%d", text_vertex_count, colored_vertex_count);
        }

        // Draw all text pixels (each character pixel is a 4-vertex rectangle)
        int draw_calls = 0;
        for (int i = 0; i < text_pixels; i++) {
            glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
            draw_calls++;
        }

        if (should_log_detailed) {
            LOG_TRACE("WIFI", "  Completed %d draw calls", draw_calls);
        }
    }
    
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDisableVertexAttribArray(gl->corner_a_position);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
#endif

// DMA buffer zero-copy rendering
// Imports hardware-decoded frame via DMA buffer file descriptor
// Uses eglCreateImageKHR and glEGLImageTargetTexture2DOES for zero-copy binding
void gl_render_frame_dma(gl_context_t *gl, int dma_fd, int width, int height,
                        int plane_offsets[3], int plane_pitches[3],
                        struct display_ctx *drm, keystone_context_t *keystone, bool clear_screen, int video_index) {
    // PRODUCTION: Validate EGL context before rendering
    if (!validate_egl_context()) {
        LOG_ERROR("GL", "Cannot render DMA - EGL context lost");
        return;
    }

    if (!gl || dma_fd < 0) {
        LOG_ERROR("DMA", "Invalid arguments (gl=%p, fd=%d)", (void*)gl, dma_fd);
        return;
    }

    if (!gl->supports_egl_image) {
        LOG_ERROR("DMA", "EGL image not supported");
        return;
    }

    // DEBUG: Log DMA rendering
    static int dma_render_count[2] = {0, 0};
    if (dma_render_count[video_index] < 3) {
        LOG_DEBUG("DMA", "DMA render - Video %d: fd=%d, size=%dx%d, clear=%d",
               video_index, dma_fd, width, height, clear_screen);
        dma_render_count[video_index]++;
    }

    // Select appropriate texture set based on video index
    GLuint tex_y = (video_index == 0) ? gl->texture_y : gl->texture_y2;
    GLuint tex_u = (video_index == 0) ? gl->texture_u : gl->texture_u2;
    GLuint tex_v = (video_index == 0) ? gl->texture_v : gl->texture_v2;

    // CRITICAL: Clear any previous EGL image bindings before rebinding
    // This prevents GL_INVALID_OPERATION errors when reusing textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }

    // Set up rendering state
    glViewport(0, 0, drm->mode.hdisplay, drm->mode.vdisplay);
    if (clear_screen && video_index == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // OPTIMIZATION: Only set up GL state once per frame (for video 0)
    // Video 1 reuses the same state - only textures and matrices change
    if (video_index == 0) {
        glUseProgram(gl->program);
        glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);

        // Position attribute
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        // Texture coordinate attribute
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // Disable blending and depth test
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
    }

    // Set MVP matrix with aspect ratio preservation (recalculate for each video!)
    float mvp_matrix[16];
    calculate_aspect_ratio_matrix(mvp_matrix, width, height, drm->mode.hdisplay, drm->mode.vdisplay);
    glUniformMatrix4fv(gl->u_mvp_matrix, 1, GL_FALSE, mvp_matrix);

    if (gl->u_use_nv12 >= 0) {
        glUniform1i(gl->u_use_nv12, 0);
    }
    
    // YUV420P format via DMA buffer (3 separate planes)
    // Use actual video dimensions, not buffer dimensions (hardware may pad buffers)
    int uv_width = width / 2;   // UV is half resolution of Y
    int uv_height = height / 2;
    
    struct timespec dma_start, dma_end;
    clock_gettime(CLOCK_MONOTONIC, &dma_start);
    
    if (!eglCreateImageKHR) {
        LOG_ERROR("DMA", "eglCreateImageKHR not loaded");
        return;
    }
    
    // Create Y plane EGL image using actual hardware layout
    EGLint y_attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane_offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, plane_pitches[0],
        EGL_NONE
    };

    EGLImage y_image = eglCreateImageKHR(gl->egl_display, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, y_attrs);
    EGLint egl_err = eglGetError();
    if (y_image == EGL_NO_IMAGE || egl_err != EGL_SUCCESS) {
        static int err_count[2] = {0, 0};
        if (err_count[video_index] < 3) {
            LOG_ERROR("DMA", "Video %d Y plane import failed: 0x%x (fd=%d, %dx%d, offset=%d, pitch=%d)",
                    video_index, egl_err, dma_fd, width, height, plane_offsets[0], plane_pitches[0]);
            err_count[video_index]++;
        }
        return;
    }
    
    // Create U plane EGL image using actual hardware layout
    EGLint u_attrs[] = {
        EGL_WIDTH, uv_width,
        EGL_HEIGHT, uv_height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane_offsets[1],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, plane_pitches[1],
        EGL_NONE
    };
    
    EGLImage u_image = eglCreateImageKHR(gl->egl_display, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, u_attrs);
    egl_err = eglGetError();
    if (u_image == EGL_NO_IMAGE || egl_err != EGL_SUCCESS) {
        static int err_count = 0;
        if (err_count < 3) {
            LOG_ERROR("DMA", "U plane import failed: 0x%x", egl_err);
            err_count++;
        }
        if (eglDestroyImageKHR) {
            (*eglDestroyImageKHR)(gl->egl_display, y_image);
        }
        return;
    }
    
    // Create V plane EGL image using actual hardware layout
    EGLint v_attrs[] = {
        EGL_WIDTH, uv_width,
        EGL_HEIGHT, uv_height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane_offsets[2],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, plane_pitches[2],
        EGL_NONE
    };
    
    EGLImage v_image = eglCreateImageKHR(gl->egl_display, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, v_attrs);
    egl_err = eglGetError();
    if (v_image == EGL_NO_IMAGE || egl_err != EGL_SUCCESS) {
        static int err_count = 0;
        if (err_count < 3) {
            LOG_ERROR("DMA", "V plane import failed: 0x%x", egl_err);
            err_count++;
        }
        if (eglDestroyImageKHR) {
            (*eglDestroyImageKHR)(gl->egl_display, y_image);
            (*eglDestroyImageKHR)(gl->egl_display, u_image);
        }
        return;
    }
    
    // Bind Y plane to texture unit 0 (use selected texture based on video_index)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)y_image);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOG_ERROR("DMA", "Y plane bind error: 0x%x", err);
        }
    } else {
        LOG_ERROR("DMA", "glEGLImageTargetTexture2DOES not loaded");
        goto cleanup_dma;
    }
    // Set texture parameters for completeness
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(gl->u_texture_y, 0);

    // Bind U plane to texture unit 1 (use selected texture based on video_index)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u);
    (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)u_image);
    GLenum err_u = glGetError();
    if (err_u != GL_NO_ERROR) {
        LOG_ERROR("DMA", "U plane bind error: 0x%x", err_u);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(gl->u_texture_u, 1);

    // Bind V plane to texture unit 2 (use selected texture based on video_index)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v);
    (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)v_image);
    GLenum err_v = glGetError();
    if (err_v != GL_NO_ERROR) {
        LOG_ERROR("DMA", "V plane bind error: 0x%x", err_v);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(gl->u_texture_v, 2);
    
    // Set flip_y uniform (video is encoded upside down)
    glUniform1f(gl->u_flip_y, 1.0f);

    // Bind keystone matrix and render
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->u_keystone_matrix, 1, GL_FALSE, keystone_matrix);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    // Check for GL errors (only report first few)
    GLenum err = glGetError();
    static int gl_err_count = 0;
    if (err != GL_NO_ERROR && gl_err_count < 3) {
        LOG_ERROR("DMA", "GL error after draw: 0x%x", err);
        gl_err_count++;
    }

    clock_gettime(CLOCK_MONOTONIC, &dma_end);

cleanup_dma:
    // Flush GL commands to GPU (non-blocking)
    // The driver will ensure EGL images aren't freed until GPU is done with them
    glFlush();

    // CRITICAL: Unbind textures from EGL images before destroying
    // Bind NULL/0 to each texture to detach the EGL image
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_2D, (GLeglImageOES)0);
    }

    // Now destroy the EGL images
    if (eglDestroyImageKHR) {
        (*eglDestroyImageKHR)(gl->egl_display, y_image);
        (*eglDestroyImageKHR)(gl->egl_display, u_image);
        (*eglDestroyImageKHR)(gl->egl_display, v_image);
    }

    // Reset texture binding
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Multi-plane YUV EGLImage rendering with external texture (zero-copy)
// Imports DRM_PRIME buffer as single multi-plane EGLImage and renders via samplerExternalOES
// Returns true if render succeeded, false if EGLImage import failed (caller should use CPU fallback)
bool gl_render_frame_external(gl_context_t *gl, int dma_fd, int width, int height,
                              int plane_offsets[3], int plane_pitches[3],
                              struct display_ctx *drm, keystone_context_t *keystone,
                              bool clear_screen, int video_index) {
    // PRODUCTION: Validate EGL context before rendering
    if (!validate_egl_context()) {
        LOG_ERROR("GL", "Cannot render external - EGL context lost");
        return false;
    }

    if (!gl || dma_fd < 0 || !gl->supports_external_texture || !gl->external_program) {
        return false;
    }

    // Select texture and texture unit based on video index
    // CRITICAL: Each video must use its own texture unit to prevent cross-contamination
    GLuint tex_external = (video_index == 0) ? gl->texture_external : gl->texture_external2;
    GLenum texture_unit = (video_index == 0) ? GL_TEXTURE0 : GL_TEXTURE1;
    GLint sampler_unit = (video_index == 0) ? 0 : 1;

    // Set up rendering state
    glViewport(0, 0, drm->mode.hdisplay, drm->mode.vdisplay);
    if (clear_screen && video_index == 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Use external shader program
    glUseProgram(gl->external_program);

    // Set up vertex attributes (using existing VBO)
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    // Set MVP matrix with aspect ratio preservation
    float mvp_matrix[16];
    calculate_aspect_ratio_matrix(mvp_matrix, width, height, drm->mode.hdisplay, drm->mode.vdisplay);
    glUniformMatrix4fv(gl->ext_u_mvp_matrix, 1, GL_FALSE, mvp_matrix);

    // Set keystone matrix
    const float *keystone_matrix = keystone_get_matrix(keystone);
    glUniformMatrix4fv(gl->ext_u_keystone_matrix, 1, GL_FALSE, keystone_matrix);

    // Flip Y coordinate for video
    glUniform1f(gl->ext_u_flip_y, 1.0f);

    // Create multi-plane YUV EGLImage using DRM_FORMAT_YUV420
    // All three planes share the same DMA FD with different offsets
    // Colorspace hints ensure correct BT.709 TV-range conversion (matching SW path)
    EGLint attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
        // Colorspace hints for accurate YUV→RGB conversion (BT.709 TV-range to match SW path)
        EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,        // Use BT.709 color matrix
        EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,     // Use TV range (16-235 luma, 16-240 chroma)
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, plane_offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, plane_pitches[0],
        EGL_DMA_BUF_PLANE1_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, plane_offsets[1],
        EGL_DMA_BUF_PLANE1_PITCH_EXT, plane_pitches[1],
        EGL_DMA_BUF_PLANE2_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, plane_offsets[2],
        EGL_DMA_BUF_PLANE2_PITCH_EXT, plane_pitches[2],
        EGL_NONE
    };

    // DEEP BUFFER EGLImages: Keep 12 frames in flight to avoid DMA buffer contention
    // In dual-hw mode, both V4L2 decoders share the same hardware, causing contention
    // when GPU tries to access a buffer that V4L2 hasn't fully released yet.
    // 12 frames = 200ms at 60fps - by then the GPU is definitely done with the buffer.
    // NO FENCE SYNC - just rely on deep buffering to avoid races.
    #define EGLIMAGE_BUFFER_DEPTH 12
    static EGLImage image_ring[2][EGLIMAGE_BUFFER_DEPTH] = {
        {EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, 
         EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE,
         EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE},
        {EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE,
         EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE,
         EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE, EGL_NO_IMAGE}
    };
    static int ring_index[2] = {0, 0};  // Next slot to write to
    static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;

    EGLImage yuv_image = eglCreateImageKHR(gl->egl_display, EGL_NO_CONTEXT,
                                           EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
    EGLint egl_err = eglGetError();

    // DUAL-HW FIX: If EGLImage creation fails, reuse the most recent valid image
    // This prevents flickering when DMA buffer is temporarily unavailable
    bool using_fallback = false;
    if (yuv_image == EGL_NO_IMAGE || egl_err != EGL_SUCCESS) {
        pthread_mutex_lock(&ring_mutex);
        // Find the most recent valid image (search backwards from current position)
        for (int i = EGLIMAGE_BUFFER_DEPTH - 1; i >= 0; i--) {
            int check_idx = (ring_index[video_index] - 1 - i + EGLIMAGE_BUFFER_DEPTH) % EGLIMAGE_BUFFER_DEPTH;
            if (image_ring[video_index][check_idx] != EGL_NO_IMAGE) {
                yuv_image = image_ring[video_index][check_idx];
                using_fallback = true;
                static int fallback_count[2] = {0, 0};
                if (fallback_count[video_index] < 5) {
                    LOG_DEBUG("EXT", "V%d EGLImage import failed (0x%x), reusing previous frame",
                             video_index + 1, egl_err);
                    fallback_count[video_index]++;
                }
                break;
            }
        }
        pthread_mutex_unlock(&ring_mutex);
        
        if (!using_fallback) {
            // No previous image to fall back to - this is first frame failure
            static int err_count[2] = {0, 0};
            if (err_count[video_index] < 5) {
                LOG_WARN("EXT", "V%d EGLImage import failed (0x%x), no fallback available",
                         video_index + 1, egl_err);
                err_count[video_index]++;
            }
            return false;  // Signal caller to use CPU fallback (if available)
        }
    }

    // Bind to GL_TEXTURE_EXTERNAL_OES using video-specific texture unit
    glActiveTexture(texture_unit);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_external);
    if (glEGLImageTargetTexture2DOES) {
        (*glEGLImageTargetTexture2DOES)(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)yuv_image);
        // Clear any GL error - some drivers report spurious errors on first few frames
        glGetError();
    }

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Set sampler uniform to the video-specific texture unit
    glUniform1i(gl->ext_u_texture_external, sampler_unit);

    // Draw
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    // Deep-buffer EGLImage management - simple ring buffer, no fence sync
    // Only update ring if we created a NEW image (not reusing fallback)
    if (!using_fallback) {
        pthread_mutex_lock(&ring_mutex);
        
        int oldest_idx = ring_index[video_index];
        
        // Destroy the oldest EGLImage (it's been 12 frames, GPU is done)
        if (image_ring[video_index][oldest_idx] != EGL_NO_IMAGE && eglDestroyImageKHR) {
            (*eglDestroyImageKHR)(gl->egl_display, image_ring[video_index][oldest_idx]);
        }
        
        // Store new image in the ring and advance index
        image_ring[video_index][oldest_idx] = yuv_image;
        ring_index[video_index] = (oldest_idx + 1) % EGLIMAGE_BUFFER_DEPTH;
        
        pthread_mutex_unlock(&ring_mutex);
    }
    // else: Reusing previous image due to EGLImage creation failure - don't modify ring

    // Log first successful render
    static bool logged = false;
    if (!logged) {
        LOG_INFO("GL", "Zero-copy YUV420 render via external texture");
        logged = true;
    }

    return true;  // Successfully rendered via zero-copy
}

void gl_cleanup(gl_context_t *gl) {
    // Clean up YUV textures (video 1)
    if (gl->texture_y) glDeleteTextures(1, &gl->texture_y);
    if (gl->texture_u) glDeleteTextures(1, &gl->texture_u);
    if (gl->texture_v) glDeleteTextures(1, &gl->texture_v);
    if (gl->texture_nv12) glDeleteTextures(1, &gl->texture_nv12);
    if (gl->texture_nv12_2) glDeleteTextures(1, &gl->texture_nv12_2);
    
    // Clean up YUV textures (video 2)
    if (gl->texture_y2) glDeleteTextures(1, &gl->texture_y2);
    if (gl->texture_u2) glDeleteTextures(1, &gl->texture_u2);
    if (gl->texture_v2) glDeleteTextures(1, &gl->texture_v2);

    if (gl->vbo) glDeleteBuffers(1, &gl->vbo);
    if (gl->ebo) glDeleteBuffers(1, &gl->ebo);
    if (gl->corner_vbo) glDeleteBuffers(1, &gl->corner_vbo);
    if (gl->border_vbo) glDeleteBuffers(1, &gl->border_vbo);
    if (gl->help_vbo) glDeleteBuffers(1, &gl->help_vbo);
    if (gl->program) glDeleteProgram(gl->program);
    if (gl->corner_program) glDeleteProgram(gl->corner_program);
    if (gl->external_program) glDeleteProgram(gl->external_program);
    if (gl->vertex_shader) glDeleteShader(gl->vertex_shader);
    if (gl->fragment_shader) glDeleteShader(gl->fragment_shader);

    // Clean up external textures
    if (gl->texture_external) glDeleteTextures(1, &gl->texture_external);
    if (gl->texture_external2) glDeleteTextures(1, &gl->texture_external2);

    // Clean up pre-allocated YUV buffers
    free_yuv_buffers();
    
    if (gl->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(gl->egl_display, gl->egl_surface);
    }
    if (gl->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(gl->egl_display, gl->egl_context);
    }
    if (gl->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(gl->egl_display);
    }
}