#define _GNU_SOURCE
#include "drm_display.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <stdint.h>

// Forward declarations
static void drm_handle_pending_flips(display_ctx_t *drm);

static const char *drm_device_paths[] = {
    "/dev/dri/card1",       // Primary display on this Pi 4 (tested working)
    "/dev/dri/card0",       // Secondary/alternate display
    "/dev/dri/renderD128",  // Render node (compute only, no display output)
    NULL
};

static int find_drm_device(void) {
    for (int i = 0; drm_device_paths[i]; i++) {
        int fd = open(drm_device_paths[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            LOG_WARN("DRM", "⊗ %s: Cannot open (%s)", drm_device_paths[i], strerror(errno));
            continue;
        }
        
        LOG_INFO("DRM", "✓ %s: Opened successfully, checking resources...", drm_device_paths[i]);
        
        // Test if this device actually works (has display resources)
        drmModeRes *resources = drmModeGetResources(fd);
        if (resources) {
            LOG_INFO("DRM", "✓ Found working DRM device: %s", drm_device_paths[i]);
            LOG_INFO("DRM", "  - Connectors: %d", resources->count_connectors);
            LOG_INFO("DRM", "  - Encoders: %d", resources->count_encoders);
            LOG_INFO("DRM", "  - CRTCs: %d", resources->count_crtcs);
            drmModeFreeResources(resources);
            return fd;
        }
        
        // This device didn't work, get more details
        LOG_WARN("DRM", "⊗ %s opened but drmModeGetResources failed", drm_device_paths[i]);
        
        // Try to get driver info
        drmVersion *version = drmGetVersion(fd);
        if (version) {
            LOG_INFO("DRM", "  Driver: %s (version %d.%d.%d)", 
                   version->name, version->version_major, 
                   version->version_minor, version->version_patchlevel);
            drmFreeVersion(version);
        }
        
        // Check capabilities
        uint64_t cap_dumb = 0;
        drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap_dumb);
        LOG_INFO("DRM", "  DRM_CAP_DUMB_BUFFER: %s", cap_dumb ? "yes" : "no");
        
        uint64_t cap_prime = 0;
        drmGetCap(fd, DRM_CAP_PRIME, &cap_prime);
        LOG_INFO("DRM", "  DRM_CAP_PRIME: %s", cap_prime ? "yes" : "no");
        
        close(fd);
    }
    
    LOG_ERROR("DRM", "\nTroubleshooting:");
    LOG_ERROR("DRM", "1. Make sure you're in the 'render' group: groups | grep render");
    LOG_ERROR("DRM", "2. If not, run: sudo usermod -a -G render $USER && logout");
    LOG_ERROR("DRM", "3. Or try with sudo: sudo ./pickel <video>");
    LOG_ERROR("DRM", "\nDevice details (try manually):");
    LOG_ERROR("DRM", "  modetest -c");
    LOG_ERROR("DRM", "  lspci | grep VGA");
    LOG_ERROR("DRM", "  dmesg | grep -i drm");
    return -1;
}
static drmModeConnector* find_connector(display_ctx_t *drm) {
    drmModeRes *resources = drmModeGetResources(drm->drm_fd);
    if (!resources) {
        LOG_ERROR("DRM", "Failed to get DRM resources");
        LOG_ERROR("DRM", "This usually means:");
        LOG_ERROR("DRM", "  1. No GPU/display driver loaded");
        LOG_ERROR("DRM", "  2. Running in SSH without display");
        LOG_ERROR("DRM", "  3. Need to run on the Pi's console directly");
        return NULL;
    }

    drmModeConnector *connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(drm->drm_fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED) {
            drm->connector_id = connector->connector_id;
            break;
        }
        if (connector) {
            drmModeFreeConnector(connector);
            connector = NULL;
        }
    }

    drmModeFreeResources(resources);
    return connector;
}

static drmModeEncoder* find_encoder(display_ctx_t *drm) {
    drmModeEncoder *encoder = drmModeGetEncoder(drm->drm_fd, drm->connector->encoder_id);
    if (encoder) {
        drm->encoder_id = encoder->encoder_id;
        drm->crtc_id = encoder->crtc_id;
    }
    return encoder;
}

int drm_init(display_ctx_t *drm) {
    memset(drm, 0, sizeof(*drm));

    // Check if we're running under X11 or Wayland (early detection)
    const char *display = getenv("DISPLAY");
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    
    // Only block if we're clearly under a graphical session
    if ((display && strlen(display) > 0) || (wayland_display && strlen(wayland_display) > 0)) {
        LOG_ERROR("DRM", "\n=== Cannot Initialize DRM ===");
        LOG_ERROR("DRM", "Running under a display server (X11/Wayland).");
        LOG_ERROR("DRM", "DISPLAY=%s", display ? display : "(not set)");
        LOG_ERROR("DRM", "WAYLAND_DISPLAY=%s", wayland_display ? wayland_display : "(not set)");
        LOG_ERROR("DRM", "DRM/KMS requires direct console access.\n");
        LOG_ERROR("DRM", "Quick fix:");
        LOG_ERROR("DRM", "  1. Switch to console: Ctrl+Alt+F1 (or F2-F6)");
        LOG_ERROR("DRM", "  2. Login and run: sudo ./pickle <video>");
        LOG_ERROR("DRM", "\nPermanent fix:");
        LOG_ERROR("DRM", "  sudo systemctl set-default multi-user.target");
        LOG_ERROR("DRM", "  sudo reboot");
        LOG_ERROR("DRM", "================================\n");
        return -1;
    }

    // Open DRM device
    drm->drm_fd = find_drm_device();
    if (drm->drm_fd < 0) {
        LOG_ERROR("DRM", "Failed to open DRM device");
        LOG_ERROR("DRM", "Hint: Try running with 'sudo ./pickel <video>' for hardware access");
        LOG_ERROR("DRM", "Or make sure you're in the 'video' group: sudo usermod -a -G video $USER");
        return -1;
    }

    // Try to become DRM master
    int master_ret = drmSetMaster(drm->drm_fd);
    if (master_ret != 0) {
        LOG_WARN("DRM", "Failed to become DRM master: %s", strerror(errno));
        LOG_WARN("DRM", "Another process may be controlling the display");
        // Continue anyway - might work with render nodes
    } else {
        LOG_INFO("DRM", "Successfully became DRM master");
    }

    // Find connected display
    drm->connector = find_connector(drm);
    if (!drm->connector) {
        LOG_ERROR("DRM", "No connected display found");
        LOG_ERROR("DRM", "\nDebugging information:");
        LOG_ERROR("DRM", "- Current TTY: %s", ttyname(STDIN_FILENO) ? ttyname(STDIN_FILENO) : "unknown");
        LOG_ERROR("DRM", "- Session type: %s", getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "unknown");
        LOG_ERROR("DRM", "- Running via SSH: %s", getenv("SSH_CLIENT") ? "yes" : "no");
        LOG_ERROR("DRM", "\nPossible solutions:");
        LOG_ERROR("DRM", "1. Run directly on Pi console (not SSH): sudo ./pickel <video>");
        LOG_ERROR("DRM", "2. Stop desktop environment: sudo systemctl stop lightdm");
        LOG_ERROR("DRM", "3. Switch to console: Ctrl+Alt+F1, then run with sudo");
        close(drm->drm_fd);
        drm->drm_fd = -1;
        return -1;
    }

    // Get display mode
    if (drm->connector->count_modes == 0) {
        LOG_ERROR("DRM", "No display modes available");
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
        close(drm->drm_fd);
        drm->drm_fd = -1;
        return -1;
    }
    
    drm->mode = drm->connector->modes[0]; // Use first mode (usually preferred)
    drm->width = drm->mode.hdisplay;
    drm->height = drm->mode.vdisplay;
    drm->refresh_rate = drm->mode.vrefresh;
    
    LOG_INFO("DRM", "Display mode: %dx%d @ %dHz", 
             drm->width, drm->height, drm->refresh_rate);

    // Find encoder
    drm->encoder = find_encoder(drm);
    if (!drm->encoder) {
        LOG_ERROR("DRM", "Failed to find encoder");
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
        close(drm->drm_fd);
        drm->drm_fd = -1;
        return -1;
    }

    // Get CRTC
    drm->crtc = drmModeGetCrtc(drm->drm_fd, drm->encoder->crtc_id);
    if (!drm->crtc) {
        LOG_ERROR("DRM", "Failed to get CRTC");
        close(drm->drm_fd);
        drm->drm_fd = -1;
        drm->connector = NULL;
        drm->encoder = NULL;
        return -1;
    }
    
    // PRODUCTION: Save original CRTC state for restoration on cleanup
    drm->saved_crtc = drmModeGetCrtc(drm->drm_fd, drm->encoder->crtc_id);
    if (!drm->saved_crtc) {
        LOG_WARN("DRM", "Failed to save original CRTC state");
    }
    
    LOG_INFO("DRM", "Using CRTC %d, Encoder %d, Connector %d", 
           drm->crtc_id, drm->encoder_id, drm->connector_id);
    
    // Initialize state
    drm->current_bo = NULL;
    drm->next_bo = NULL;
    drm->current_fb_id = 0;
    drm->next_fb_id = 0;
    drm->waiting_for_flip = false;
    drm->mode_set_done = false;

    // Initialize GBM
    drm->gbm_device = gbm_create_device(drm->drm_fd);
    if (!drm->gbm_device) {
        LOG_ERROR("DRM", "Failed to create GBM device");
        drmModeFreeCrtc(drm->crtc);
        drm->crtc = NULL;
        drmModeFreeEncoder(drm->encoder);
        drm->encoder = NULL;
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
        close(drm->drm_fd);
        drm->drm_fd = -1;
        return -1;
    }

    // Create GBM surface
    drm->gbm_surface = gbm_surface_create(drm->gbm_device,
                                          drm->width, drm->height,
                                          GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!drm->gbm_surface) {
        LOG_ERROR("DRM", "Failed to create GBM surface");
        gbm_device_destroy(drm->gbm_device);
        drm->gbm_device = NULL;
        drmModeFreeCrtc(drm->crtc);
        drm->crtc = NULL;
        drmModeFreeEncoder(drm->encoder);
        drm->encoder = NULL;
        drmModeFreeConnector(drm->connector);
        drm->connector = NULL;
        close(drm->drm_fd);
        drm->drm_fd = -1;
        return -1;
    }

    // DRM initialization complete
    return 0;
}

// Callback to destroy framebuffer when GBM buffer is destroyed
static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data) {
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    uint32_t fb_id = (uint32_t)(uintptr_t)data;
    
    if (fb_id) {
        drmModeRmFB(drm_fd, fb_id);
    }
}

uint32_t drm_get_fb_for_bo(display_ctx_t *drm, struct gbm_bo *bo) {
    uint32_t fb_id = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if (fb_id) {
        return fb_id;
    }
    
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    
    int ret = drmModeAddFB(drm->drm_fd, width, height, 24, 32,
                          stride, handle, &fb_id);
    if (ret) {
        LOG_ERROR("DRM", "Failed to create framebuffer: %s", strerror(errno));
        return 0;
    }
    
    // Register destroy callback to automatically free framebuffer when buffer is destroyed
    gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb_id, drm_fb_destroy_callback);
    return fb_id;
}

void drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, 
                          unsigned int usec, void *data) {
    (void)fd; (void)frame; (void)sec; (void)usec; // Suppress unused warnings
    display_ctx_t *drm = (display_ctx_t *)data;
    
    // Page flip completed - now safe to release the previous buffer
    if (drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->current_bo);
    }
    
    // Update current buffer to the one that just got displayed
    drm->current_bo = drm->next_bo;
    drm->current_fb_id = drm->next_fb_id;
    
    drm->waiting_for_flip = false;
}

int drm_set_mode(display_ctx_t *drm, uint32_t fb_id) {
    static bool error_shown = false;
    
    int ret = drmModeSetCrtc(drm->drm_fd, drm->crtc_id, fb_id,
                             0, 0, // x, y offset
                             &drm->connector_id, 1,
                             &drm->mode);
    if (ret != 0) {
        // Only show detailed error once
        if (!error_shown) {
            LOG_ERROR("DRM", "Failed to set CRTC mode: %s", strerror(errno));
            
            // Provide helpful diagnostics
            if (errno == EACCES || errno == EPERM) {
                LOG_ERROR("DRM", "\n=== DRM Permission Error ===");
                LOG_ERROR("DRM", "Another process may be using the display (X11, Wayland, etc.)");
                LOG_ERROR("DRM", "Solutions:");
                LOG_ERROR("DRM", "  1. Stop display manager: sudo systemctl stop lightdm");
                LOG_ERROR("DRM", "  2. Run with sudo: sudo ./pickle <video>");
                LOG_ERROR("DRM", "  3. Add to groups: sudo usermod -a -G video,render $USER");
                LOG_ERROR("DRM", "     (then logout/login)");
                LOG_ERROR("DRM", "============================\n");
            }
            error_shown = true;
        }
        return -1;
    }
    
    drm->mode_set_done = true;
    return 0;
}

// Handle any pending page flip completions without blocking
static void drm_handle_pending_flips(display_ctx_t *drm) {
    if (!drm->waiting_for_flip) {
        return; // No pending flips
    }
    
    // Non-blocking check for pending events
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(drm->drm_fd, &fds);
    
    struct timeval timeout = {0, 0}; // Non-blocking (immediate return)
    
    drmEventContext evctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = drm_page_flip_handler,
    };
    
    int ret = select(drm->drm_fd + 1, &fds, NULL, NULL, &timeout);
    if (ret > 0 && FD_ISSET(drm->drm_fd, &fds)) {
        drmHandleEvent(drm->drm_fd, &evctx);
    }
}

int drm_swap_buffers(display_ctx_t *drm) {
    // Get the front buffer from GBM surface
    drm->next_bo = gbm_surface_lock_front_buffer(drm->gbm_surface);
    if (!drm->next_bo) {
        LOG_ERROR("DRM", "Failed to lock front buffer");
        return -1;
    }
    // Get framebuffer ID for the buffer
    drm->next_fb_id = drm_get_fb_for_bo(drm, drm->next_bo);
    if (!drm->next_fb_id) {
        LOG_ERROR("DRM", "Failed to get framebuffer ID");
        gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
        return -1;
    }
    
    // First frame: set the mode
    if (!drm->mode_set_done) {
        LOG_INFO("DRM", "Setting display mode...");
        int ret = drm_set_mode(drm, drm->next_fb_id);
        if (ret) {
            // Only print error once, then fail gracefully
            static bool error_printed = false;
            if (!error_printed) {
                LOG_ERROR("DRM", "drm_set_mode failed with code %d", ret);
                error_printed = true;
            }
            gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
            return ret;
        }
        drm->current_bo = drm->next_bo;
        drm->current_fb_id = drm->next_fb_id;
        LOG_INFO("DRM", "Display initialized. Video should appear now.");
        return 0;
    }
    
    // Handle any pending page flip completions first (non-blocking)
    drm_handle_pending_flips(drm);
    
    // If we're still waiting for a previous flip, skip this frame to avoid stalling
    if (drm->waiting_for_flip) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
        return 0; // Not an error, just frame skip for performance
    }
    
    // Queue page flip (asynchronous)
    drm->waiting_for_flip = true;
    int ret = drmModePageFlip(drm->drm_fd, drm->crtc_id, drm->next_fb_id,
                             DRM_MODE_PAGE_FLIP_EVENT, drm);
    if (ret) {
        LOG_ERROR("DRM", "Failed to queue page flip: %s", strerror(errno));
        drm->waiting_for_flip = false;
        gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
        // PRODUCTION: Clean up framebuffer on error to prevent resource leak
        if (drm->next_fb_id) {
            drmModeRmFB(drm->drm_fd, drm->next_fb_id);
            drm->next_fb_id = 0;
        }
        return -1;
    }
    
    // Don't wait for completion - let it complete asynchronously
    // The page flip handler will update the buffers when it completes
    return 0;
}

void drm_cleanup(display_ctx_t *drm) {
    // PRODUCTION: Restore original CRTC state before cleanup
    if (drm->saved_crtc && drm->drm_fd >= 0) {
        int ret = drmModeSetCrtc(drm->drm_fd, 
                                 drm->saved_crtc->crtc_id,
                                 drm->saved_crtc->buffer_id,
                                 drm->saved_crtc->x, 
                                 drm->saved_crtc->y,
                                 &drm->connector_id, 
                                 1, 
                                 &drm->saved_crtc->mode);
        if (ret < 0) {
            LOG_WARN("DRM", "Failed to restore CRTC state: %d", ret);
        }
        drmModeFreeCrtc(drm->saved_crtc);
        drm->saved_crtc = NULL;
    }
    
    // Clean up framebuffers
    if (drm->current_fb_id) {
        drmModeRmFB(drm->drm_fd, drm->current_fb_id);
    }
    if (drm->next_fb_id && drm->next_fb_id != drm->current_fb_id) {
        drmModeRmFB(drm->drm_fd, drm->next_fb_id);
    }
    
    // Release buffers
    if (drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->current_bo);
    }
    if (drm->next_bo && drm->next_bo != drm->current_bo) {
        gbm_surface_release_buffer(drm->gbm_surface, drm->next_bo);
    }
    
    if (drm->gbm_surface) {
        gbm_surface_destroy(drm->gbm_surface);
    }
    if (drm->gbm_device) {
        gbm_device_destroy(drm->gbm_device);
    }
    if (drm->crtc) {
        drmModeFreeCrtc(drm->crtc);
    }
    if (drm->encoder) {
        drmModeFreeEncoder(drm->encoder);
    }
    if (drm->connector) {
        drmModeFreeConnector(drm->connector);
    }
    if (drm->drm_fd >= 0) {
        drmDropMaster(drm->drm_fd);
        close(drm->drm_fd);
    }
}