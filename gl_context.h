#ifndef GL_CONTEXT_H
#define GL_CONTEXT_H

#include <EGL/egl.h>
#include <GLES3/gl31.h>  // OpenGL ES 3.1 for modern features
#include <gbm.h>
#include <stdbool.h>
#include "drm_display.h"
#include "keystone.h"

// Forward declarations
struct display_ctx;

typedef struct {
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    
    GLuint program;
    GLuint vertex_shader;
    GLuint fragment_shader;
    
    // YUV textures for hardware decoded video (video 1)
    GLuint texture_y;     // Y plane texture
    GLuint texture_u;     // U plane texture  
    GLuint texture_v;     // V plane texture
    GLuint texture_nv12;  // NV12 UV interleaved texture (video 1)
    GLuint texture_nv12_2;// NV12 UV interleaved texture (video 2)
    
    // YUV textures for second video (video 2)
    GLuint texture_y2;    // Y plane texture (video 2)
    GLuint texture_u2;    // U plane texture (video 2)
    GLuint texture_v2;    // V plane texture (video 2)

    GLuint vbo;
    GLuint ebo;
    GLuint vao;
    
    // Modern ES 3.1 Uniform Buffer Objects
    GLuint transform_ubo;      // TransformMatrices UBO
    GLuint video_settings_ubo; // VideoSettings UBO
    
    // UBO binding points
    GLuint transform_binding_point;
    GLuint video_settings_binding_point;
    
    // Legacy uniform locations (fallback)
    GLint u_mvp_matrix;
    GLint u_texture_y;    // Y plane sampler
    GLint u_texture_u;    // U plane sampler
    GLint u_texture_v;    // V plane sampler
    GLint u_texture_nv12; // NV12 UV sampler
    GLint u_use_nv12;     // Toggle for NV12 path
    GLint u_keystone_matrix;
    GLint u_flip_y;       // Flip texture Y coordinate (for upside-down videos)
    
    // Vertex attributes
    GLint a_position;
    GLint a_texcoord;
    
    // Corner highlighting
    GLuint corner_program;
    GLuint corner_vbo;
    GLint corner_a_position;
    GLint corner_u_mvp_matrix;
    GLint corner_u_color;
    
    // Border rendering
    GLuint border_vbo;
    
    // Help overlay rendering
    GLuint help_vbo;
    
    // GPU sync objects for pipelining
    GLsync gpu_fence;
    
    // EGL DMA buffer zero-copy support
    bool supports_egl_image;         // True if EGL_EXT_image_dma_buf_import supported
    bool supports_external_texture;  // True if GL_OES_EGL_image_external supported
    EGLImage egl_image_y;            // EGL image for Y plane (DMA-backed)
    EGLImage egl_image_uv;           // EGL image for UV plane (DMA-backed) - NV12 packed
    EGLImage egl_image_y2;           // EGL image for Y plane (video 2)
    EGLImage egl_image_uv2;          // EGL image for UV plane (video 2)

    // External texture program (for zero-copy YUV import)
    GLuint external_program;         // Shader program using samplerExternalOES
    GLint ext_u_mvp_matrix;          // MVP matrix uniform
    GLint ext_u_keystone_matrix;     // Keystone matrix uniform
    GLint ext_u_flip_y;              // Flip Y uniform
    GLint ext_u_texture_external;    // External texture sampler uniform
    GLuint texture_external;         // External texture for YUV EGLImage
    GLuint texture_external2;        // External texture for video 2
    EGLImage egl_image_yuv;          // Multi-plane YUV EGLImage (video 1)
    EGLImage egl_image_yuv2;         // Multi-plane YUV EGLImage (video 2)
} gl_context_t;

// OpenGL ES functions
int gl_init(gl_context_t *gl, display_ctx_t *drm);
void gl_cleanup(gl_context_t *gl);
int gl_create_shaders(gl_context_t *gl);
void gl_setup_buffers(gl_context_t *gl);
void gl_render_frame(gl_context_t *gl, uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, 
                    int width, int height, int y_stride, int u_stride, int v_stride,
                    struct display_ctx *drm, keystone_context_t *keystone, bool clear_screen, int video_index);
void gl_render_nv12(gl_context_t *gl, uint8_t *nv12_data, int width, int height, int stride,
                    struct display_ctx *drm, keystone_context_t *keystone, bool clear_screen, int video_index);
void gl_render_corners(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_border(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_display_boundary(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_help_overlay(gl_context_t *gl, keystone_context_t *keystone);
void gl_render_notification_overlay(gl_context_t *gl, const char *message);
void gl_swap_buffers(gl_context_t *gl, struct display_ctx *drm);

// DMA buffer zero-copy rendering (NV12 format) - legacy separate planes
void gl_render_frame_dma(gl_context_t *gl, int dma_fd, int width, int height,
                        int plane_offsets[3], int plane_pitches[3],
                        struct display_ctx *drm, keystone_context_t *keystone, bool clear_screen, int video_index);

// DMA buffer zero-copy rendering (multi-plane YUV EGLImage with external texture)
// Returns true if successful, false if EGLImage import failed (use CPU fallback)
bool gl_render_frame_external(gl_context_t *gl, int dma_fd, int width, int height,
                              int plane_offsets[3], int plane_pitches[3],
                              struct display_ctx *drm, keystone_context_t *keystone, bool clear_screen, int video_index);

// Shader source code
extern const char *vertex_shader_source;
extern const char *fragment_shader_source;

#endif // GL_CONTEXT_H