#define _GNU_SOURCE  // For pthread_timedjoin_np
#define _POSIX_C_SOURCE 199309L
// KMS video overlay plane implementation for hardware zero-copy video playback
// This bypasses OpenGL/EGL entirely and uses DRM/KMS direct scanout
#include "drm_display.h"
#include "video_decoder.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <drm_fourcc.h>
#include <time.h>

// Worker thread function for non-blocking plane updates
static void* plane_worker_thread_func(void* arg) {
    display_ctx_t *drm = (display_ctx_t*)arg;
    
    pthread_mutex_lock(&drm->plane_mutex);
    
    while (!drm->plane_worker_shutdown) {
        // Wait for a plane update request
        while (!drm->plane_update.pending && !drm->plane_worker_shutdown) {
            pthread_cond_wait(&drm->plane_cond, &drm->plane_mutex);
        }
        
        if (drm->plane_worker_shutdown) {
            break;
        }
        
        // Get the pending update
        uint32_t fb_id = drm->plane_update.fb_id;
        uint32_t x = drm->plane_update.x;
        uint32_t y = drm->plane_update.y;
        uint32_t width = drm->plane_update.width;
        uint32_t height = drm->plane_update.height;
        drm->plane_update.pending = false;
        
        // Unlock while doing the blocking call
        pthread_mutex_unlock(&drm->plane_mutex);
        
        // Perform the blocking plane update
        struct timespec plane_t1, plane_t2;
        clock_gettime(CLOCK_MONOTONIC, &plane_t1);
        int ret = drmModeSetPlane(drm->drm_fd, drm->video_plane_id, drm->crtc_id,
                                 fb_id, 0,
                                 x, y, width, height,
                                 0 << 16, 0 << 16,
                                 width << 16, height << 16);
        clock_gettime(CLOCK_MONOTONIC, &plane_t2);
        double plane_ms = (plane_t2.tv_sec - plane_t1.tv_sec) * 1000.0 +
                          (plane_t2.tv_nsec - plane_t1.tv_nsec) / 1000000.0;

        // Frame timing tracking (disabled unless debugging jitter)
        // static struct timespec last_present_time = {0, 0};
        // if (last_present_time.tv_sec != 0) {
        //     double frame_interval_ms = (plane_t2.tv_sec - last_present_time.tv_sec) * 1000.0 +
        //                                (plane_t2.tv_nsec - last_present_time.tv_nsec) / 1000000.0;
        //     if (frame_interval_ms > 20.0 || frame_interval_ms < 13.0) {
        //         printf("[JITTER] Frame interval: %.1fms (vsync wait: %.1fms)\n",
        //                frame_interval_ms, plane_ms);
        //     }
        // }
        // last_present_time = plane_t2;

        if (hw_debug_enabled && plane_ms > 5.0) {
            LOG_DEBUG("KMS-WORKER", "drmModeSetPlane took %.1fms (plane=%u, crtc=%u, fb=%u)",
                   plane_ms, drm->video_plane_id, drm->crtc_id, fb_id);
        }
        
        // Reacquire lock before accessing shared state
        pthread_mutex_lock(&drm->plane_mutex);
        
        if (ret < 0) {
            static int err_count = 0;
            if (err_count < 5 && hw_debug_enabled) {
                LOG_WARN("KMS-WORKER", "drmModeSetPlane failed: %s", strerror(errno));
                err_count++;
            }
        } else {
            // Update tracking (now protected by mutex)
            drm->prev_video_fb_id = drm->video_fb_id;
            drm->video_fb_id = fb_id;
        }
    }

    pthread_mutex_unlock(&drm->plane_mutex);
    return NULL;
}

// Initialize and find an available overlay plane for video
int drm_init_video_plane(display_ctx_t *drm) {
    if (!drm || drm->drm_fd < 0) {
        LOG_ERROR("KMS", "Invalid DRM context");
        return -1;
    }
    
    // Find CRTC index by looking through resources
    drmModeResPtr resources = drmModeGetResources(drm->drm_fd);
    if (!resources) {
        LOG_ERROR("KMS", "Failed to get DRM resources");
        return -1;
    }
    
    int crtc_index = -1;
    for (int i = 0; i < resources->count_crtcs; i++) {
        if (resources->crtcs[i] == drm->crtc_id) {
            crtc_index = i;
            break;
        }
    }
    drmModeFreeResources(resources);
    
    if (crtc_index < 0) {
        LOG_ERROR("KMS", "Could not find CRTC index for ID %u", drm->crtc_id);
        return -1;
    }
    
    if (hw_debug_enabled) {
        LOG_DEBUG("KMS", "CRTC ID %u is at index %d", drm->crtc_id, crtc_index);
    }
    
    // Get all available planes
    drmModePlaneResPtr planes = drmModeGetPlaneResources(drm->drm_fd);
    if (!planes) {
        LOG_ERROR("KMS", "Failed to get plane resources: %s", strerror(errno));
        return -1;
    }
    
    if (hw_debug_enabled) {
        LOG_DEBUG("KMS", "Found %d planes total", planes->count_planes);
    }
    
    // Look for an overlay plane that supports YUV420 and our CRTC
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(drm->drm_fd, planes->planes[i]);
        if (!plane) continue;
        
        // Check if this plane can be used with our CRTC
        if (!(plane->possible_crtcs & (1 << crtc_index))) {
            drmModeFreePlane(plane);
            continue;
        }
        
        // Check if plane supports YU12/I420 format
        bool supports_yuv420 = false;
        for (uint32_t j = 0; j < plane->count_formats; j++) {
            if (plane->formats[j] == DRM_FORMAT_YUV420) {
                supports_yuv420 = true;
                break;
            }
        }
        
        if (!supports_yuv420) {
            drmModeFreePlane(plane);
            continue;
        }
        
        // Get plane properties to check type
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
            drm->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        
            if (props) {
            bool is_overlay = false;

            // Check plane type and cache common atomic property IDs for faster commits
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyPtr prop = drmModeGetProperty(drm->drm_fd, props->props[j]);
                if (prop) {
                    if (strcmp(prop->name, "type") == 0) {
                        uint64_t type = props->prop_values[j];
                        // DRM_PLANE_TYPE_OVERLAY = 0, PRIMARY = 1, CURSOR = 2
                        if (type == DRM_PLANE_TYPE_OVERLAY) {
                            is_overlay = true;
                        }
                    }

                    // Cache property IDs by name for atomic commits
                    if (strcmp(prop->name, "FB_ID") == 0) drm->video_plane_prop_fb_id = prop->prop_id;
                    else if (strcmp(prop->name, "CRTC_ID") == 0) drm->video_plane_prop_crtc_id = prop->prop_id;
                    else if (strcmp(prop->name, "SRC_X") == 0) drm->video_plane_prop_src_x = prop->prop_id;
                    else if (strcmp(prop->name, "SRC_Y") == 0) drm->video_plane_prop_src_y = prop->prop_id;
                    else if (strcmp(prop->name, "SRC_W") == 0) drm->video_plane_prop_src_w = prop->prop_id;
                    else if (strcmp(prop->name, "SRC_H") == 0) drm->video_plane_prop_src_h = prop->prop_id;
                    else if (strcmp(prop->name, "CRTC_X") == 0) drm->video_plane_prop_crtc_x = prop->prop_id;
                    else if (strcmp(prop->name, "CRTC_Y") == 0) drm->video_plane_prop_crtc_y = prop->prop_id;
                    else if (strcmp(prop->name, "CRTC_W") == 0) drm->video_plane_prop_crtc_w = prop->prop_id;
                    else if (strcmp(prop->name, "CRTC_H") == 0) drm->video_plane_prop_crtc_h = prop->prop_id;

                    drmModeFreeProperty(prop);
                }
            }

            drmModeFreeObjectProperties(props);

            // Found a suitable overlay plane
            if (is_overlay && plane->crtc_id == 0) {  // Not currently in use
                if (hw_debug_enabled) {
                    LOG_DEBUG("KMS", "✓ Found available overlay plane: %u (supports YUV420, compatible with CRTC %d)", 
                           plane->plane_id, crtc_index);
                }
                drm->video_plane_id = plane->plane_id;
                drm->video_plane_available = true;
                drm->video_fb_id = 0;
                drm->prev_video_fb_id = 0;
                
                // Initialize worker thread for non-blocking plane updates
                pthread_mutex_init(&drm->plane_mutex, NULL);
                pthread_cond_init(&drm->plane_cond, NULL);
                drm->plane_update.pending = false;
                drm->plane_worker_shutdown = false;
                drm->plane_worker_running = false;
                
                if (pthread_create(&drm->plane_worker_thread, NULL, plane_worker_thread_func, drm) == 0) {
                    drm->plane_worker_running = true;
                    if (hw_debug_enabled) {
                        LOG_DEBUG("KMS", "Worker thread started for non-blocking plane updates");
                    }
                } else {
                    LOG_WARN("KMS", "Failed to create worker thread, will use blocking updates");
                }
                
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(planes);
                return 0;
            }
        }
        
        drmModeFreePlane(plane);
    }
    
    drmModeFreePlaneResources(planes);
    LOG_ERROR("KMS", "No available overlay plane found");
    return -1;
}

// Create a DRM framebuffer from a V4L2 DMA buffer (YU12/I420 format)
int drm_create_video_fb(display_ctx_t *drm, int dma_fd, uint32_t width, uint32_t height,
                        int plane_offsets[3], int plane_pitches[3], uint32_t *fb_id_out) {
    if (!drm || dma_fd < 0 || !fb_id_out) {
        LOG_ERROR("KMS", "Invalid parameters for framebuffer creation");
        return -1;
    }
    
    // Check cache first - reuse existing framebuffer if we've seen this DMA FD before
    for (int i = 0; i < drm->fb_cache_count; i++) {
        if (drm->fb_cache[i].dma_fd == dma_fd) {
            *fb_id_out = drm->fb_cache[i].fb_id;
            return 0;  // Cache hit - reuse existing FB
        }
    }
    
    // Cache miss - create new framebuffer
    // DRM framebuffer descriptor for YU12/I420 format (3 planes)
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    
    // Convert DMA-BUF FD to GEM handle
    struct drm_prime_handle prime_handle = {
        .fd = dma_fd,
        .flags = 0,
        .handle = 0
    };
    
    if (drmIoctl(drm->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime_handle) < 0) {
        LOG_ERROR("KMS", "Failed to import DMA-BUF: %s", strerror(errno));
        return -1;
    }

    // Measure import duration (helpful to detect blocking in PRIME import)
    // Note: we only have an end timestamp here, so the measurement is best-effort
    
    // YU12/I420: All 3 planes use the same GEM buffer with different offsets
    handles[0] = prime_handle.handle;  // Y plane
    handles[1] = prime_handle.handle;  // U plane
    handles[2] = prime_handle.handle;  // V plane
    
    pitches[0] = plane_pitches[0];
    pitches[1] = plane_pitches[1];
    pitches[2] = plane_pitches[2];
    
    offsets[0] = plane_offsets[0];
    offsets[1] = plane_offsets[1];
    offsets[2] = plane_offsets[2];
    
    // Create framebuffer with YU12 format
    uint32_t fb_id = 0;
    // Time the framebuffer creation call (this can block if GEM/driver is busy)
    struct timespec fb_t1, fb_t2;
    clock_gettime(CLOCK_MONOTONIC, &fb_t1);
    int ret = drmModeAddFB2(drm->drm_fd, width, height, DRM_FORMAT_YUV420,
                           handles, pitches, offsets, &fb_id, 0);
    clock_gettime(CLOCK_MONOTONIC, &fb_t2);
    double fb_ms = (fb_t2.tv_sec - fb_t1.tv_sec) * 1000.0 +
                   (fb_t2.tv_nsec - fb_t1.tv_nsec) / 1000000.0;
    if ((fb_ms > 5.0 && hw_debug_enabled) || hw_debug_enabled) {
        LOG_DEBUG("KMS", "drmModeAddFB2 took %.1fms (width=%u height=%u)", fb_ms, width, height);
    }
    
    if (ret < 0) {
        LOG_ERROR("KMS", "drmModeAddFB2 failed: %s", strerror(errno));
        LOG_ERROR("KMS", "Format: YUV420, Size: %dx%d", width, height);
        LOG_ERROR("KMS", "Pitches: %d, %d, %d", pitches[0], pitches[1], pitches[2]);
        LOG_ERROR("KMS", "Offsets: %d, %d, %d", offsets[0], offsets[1], offsets[2]);
        
        // Clean up GEM handle
        struct drm_gem_close gem_close = { .handle = prime_handle.handle };
        drmIoctl(drm->drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        return -1;
    }
    
    *fb_id_out = fb_id;
    
    // Add to cache for reuse
    if (drm->fb_cache_count < 8) {
        drm->fb_cache[drm->fb_cache_count].dma_fd = dma_fd;
        drm->fb_cache[drm->fb_cache_count].fb_id = fb_id;
        drm->fb_cache_count++;
    }
    
    // Only log first few framebuffer creations
    static int fb_create_count = 0;
    if (fb_create_count < 3 && hw_debug_enabled) {
        LOG_DEBUG("KMS", "✓ Created framebuffer %d from DMA-BUF (FD=%d, %dx%d) [cached]", 
               fb_id, dma_fd, width, height);
        fb_create_count++;
    }
    return 0;
}

// Display a video frame on the overlay plane
int drm_display_video_frame(display_ctx_t *drm, uint32_t fb_id, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height) {
    if (!drm || !drm->video_plane_available || fb_id == 0) {
        return -1;
    }
    
    // Get current mode to ensure we're within display bounds
    uint32_t crtc_w = drm->mode.hdisplay;
    uint32_t crtc_h = drm->mode.vdisplay;
    
    // Clamp video to display size if needed
    if (width > crtc_w) width = crtc_w;
    if (height > crtc_h) height = crtc_h;
    
    // If worker thread is running, submit update to the worker
    if (drm->plane_worker_running) {
        pthread_mutex_lock(&drm->plane_mutex);

        // Non-blocking: overwrite pending update if worker is still processing
        // This allows decode to run ahead of display, maintaining smooth 60fps
        // The single-mailbox design naturally rate-limits to vsync timing

        // Frame overwrite tracking (disabled - handled by frame pacing now)
        // static int overwrite_count = 0;
        // if (drm->plane_update.pending) {
        //     overwrite_count++;
        //     if (overwrite_count % 100 == 0) {
        //         printf("[WARN] %d frames overwritten\n", overwrite_count);
        //     }
        // }

        // Submit new plane update (overwrites pending if worker is busy)
        drm->plane_update.fb_id = fb_id;
        drm->plane_update.x = x;
        drm->plane_update.y = y;
        drm->plane_update.width = width;
        drm->plane_update.height = height;
        drm->plane_update.pending = true;

        // Signal the worker thread
        pthread_cond_signal(&drm->plane_cond);

        pthread_mutex_unlock(&drm->plane_mutex);
        return 0;
    }
    
    // Fallback: if no worker thread, do blocking update on main thread
    // (This preserves backward compatibility if thread creation fails)
    struct timespec plane_t1, plane_t2;
    clock_gettime(CLOCK_MONOTONIC, &plane_t1);
    int ret = drmModeSetPlane(drm->drm_fd, drm->video_plane_id, drm->crtc_id,
                             fb_id, 0,
                             x, y, width, height,
                             0 << 16, 0 << 16,
                             width << 16, height << 16);
    clock_gettime(CLOCK_MONOTONIC, &plane_t2);
    double plane_ms = (plane_t2.tv_sec - plane_t1.tv_sec) * 1000.0 +
                      (plane_t2.tv_nsec - plane_t1.tv_nsec) / 1000000.0;
    if ((plane_ms > 5.0 && hw_debug_enabled) || plane_ms > 20.0) {
        LOG_DEBUG("KMS", "drmModeSetPlane took %.1fms (plane=%u, crtc=%u, fb=%u)",
               plane_ms, drm->video_plane_id, drm->crtc_id, fb_id);
    }
    
    if (ret < 0) {
        static int err_count = 0;
        if (err_count < 5) {
            LOG_ERROR("KMS", "drmModeSetPlane failed: %s (plane=%u, crtc=%u, fb=%u, pos=%u,%u, size=%ux%u)", 
                   strerror(errno), drm->video_plane_id, drm->crtc_id, fb_id, x, y, width, height);
            err_count++;
        }
        return -1;
    }
    
    // Track current/previous for reference (but don't remove cached FBs)
    drm->prev_video_fb_id = drm->video_fb_id;
    drm->video_fb_id = fb_id;
    
    return 0;
}

// Hide the video overlay plane
void drm_hide_video_plane(display_ctx_t *drm) {
    if (!drm || !drm->video_plane_available) {
        return;
    }
    
    // Shutdown worker thread if running
    if (drm->plane_worker_running) {
        pthread_mutex_lock(&drm->plane_mutex);
        drm->plane_worker_shutdown = true;
        pthread_cond_signal(&drm->plane_cond);
        pthread_mutex_unlock(&drm->plane_mutex);

        // PRODUCTION: Use timed join with 100ms timeout for fast shutdown
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 100000000;  // 100ms
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }

        int result = pthread_timedjoin_np(drm->plane_worker_thread, NULL, &timeout);
        if (result != 0) {
            // Thread didn't exit in time - cancel it forcefully
            pthread_cancel(drm->plane_worker_thread);
            pthread_join(drm->plane_worker_thread, NULL);
        }

        pthread_mutex_destroy(&drm->plane_mutex);
        pthread_cond_destroy(&drm->plane_cond);
        drm->plane_worker_running = false;

        if (hw_debug_enabled) {
            LOG_DEBUG("KMS", "Worker thread stopped");
        }
    }
    
    // Disable the plane by setting fb_id to 0
    drmModeSetPlane(drm->drm_fd, drm->video_plane_id, 0, 0, 0, 
                   0, 0, 0, 0, 0, 0, 0, 0);
    
    // Clean up all cached framebuffers
    for (int i = 0; i < drm->fb_cache_count; i++) {
        if (drm->fb_cache[i].fb_id != 0) {
            drmModeRmFB(drm->drm_fd, drm->fb_cache[i].fb_id);
        }
    }
    drm->fb_cache_count = 0;
    
    drm->video_fb_id = 0;
    drm->prev_video_fb_id = 0;
}
