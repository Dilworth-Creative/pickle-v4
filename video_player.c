#define _GNU_SOURCE  // For pthread_timedjoin_np
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE
#include "video_player.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sched.h>  // For CPU affinity
#include <linux/input.h>
#include <sys/stat.h>
#include <signal.h>
#include "production_config.h"

// Global quit flag set by async-signal-safe signal handler
extern volatile sig_atomic_t g_quit_requested;

static double timespec_diff_seconds(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0.0;
    }
    double sec = (double)(end->tv_sec - start->tv_sec);
    double nsec = (double)(end->tv_nsec - start->tv_nsec) / 1e9;
    return sec + nsec;
}

static void format_metric_ms(double seconds, char *buffer, size_t length) {
    if (!buffer || length == 0) {
        return;
    }
    if (seconds < 0.0) {
        snprintf(buffer, length, "--");
    } else {
        snprintf(buffer, length, "%.2f", seconds * 1000.0);
    }
}

// Validate video file before processing
static int validate_video_file(const char *filename) {
    if (!filename) {
        LOG_ERROR("VIDEO", "No video file specified");
        return -1;
    }
    
    // Check if file exists and get size
    struct stat st;
    if (stat(filename, &st) != 0) {
        LOG_ERROR("VIDEO", "Cannot access video file: %s", filename);
        return -1;
    }
    
    // Check file size limits
    if ((unsigned long long)st.st_size > MAX_VIDEO_FILE_SIZE) {
        LOG_ERROR("VIDEO", "Video file too large (%lld bytes, limit: %lld bytes)",
                (long long)st.st_size, (long long)MAX_VIDEO_FILE_SIZE);
        return -1;
    }
    
    if (st.st_size < 1024) {  // Minimum 1KB for valid video
        LOG_ERROR("VIDEO", "Video file too small (%lld bytes)", (long long)st.st_size);
        return -1;
    }
    
    LOG_INFO("VIDEO", "Video file validation passed: %s (%lld bytes)", filename, (long long)st.st_size);
    return 0;
}

// Show a notification message overlay for a specified duration
static void show_notification(app_context_t *app, const char *message, double duration) {
    if (!app || !message) return;

    strncpy(app->notification_message, message, sizeof(app->notification_message) - 1);
    app->notification_message[sizeof(app->notification_message) - 1] = '\0';

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    app->notification_start_time = ts.tv_sec + ts.tv_nsec / 1e9;
    app->notification_duration = duration;
    app->notification_active = true;
}

static bool process_keystone_movement(app_context_t *app, double delta_time, double target_frame_time) {
    if (!app || !app->input) {
        return false;
    }
    
    // Get active keystone
    keystone_context_t *active_ks = (app->active_keystone == 1 && app->keystone2) ? app->keystone2 : app->keystone;
    if (!active_ks) {
        return false;
    }
    


    bool left = input_is_key_pressed(app->input, KEY_LEFT);
    bool right = input_is_key_pressed(app->input, KEY_RIGHT);
    bool up = input_is_key_pressed(app->input, KEY_UP);
    bool down = input_is_key_pressed(app->input, KEY_DOWN);

    if (app->input->use_stdin_fallback) {
        left = left || input_is_key_just_pressed(app->input, KEY_LEFT);
        right = right || input_is_key_just_pressed(app->input, KEY_RIGHT);
        up = up || input_is_key_just_pressed(app->input, KEY_UP);
        down = down || input_is_key_just_pressed(app->input, KEY_DOWN);
    }
    
    // Add gamepad input (D-pad and left analog stick)
    if (app->input->gamepad_enabled) {
        // D-pad input
        if (app->input->gamepad_dpad_x < 0) left = true;
        if (app->input->gamepad_dpad_x > 0) right = true;
        if (app->input->gamepad_dpad_y < 0) up = true;
        if (app->input->gamepad_dpad_y > 0) down = true;
        
        // Analog stick input (with deadzone)
        const int16_t deadzone = 8000; // ~25% deadzone
        if (app->input->gamepad_axis_x < -deadzone) left = true;
        if (app->input->gamepad_axis_x > deadzone) right = true;
        if (app->input->gamepad_axis_y < -deadzone) up = true;
        if (app->input->gamepad_axis_y > deadzone) down = true;
    }

    float move_x = 0.0f;
    float move_y = 0.0f;

    if (left != right) {
        move_x = left ? -1.0f : 1.0f;
    }
    if (up != down) {
        move_y = up ? 1.0f : -1.0f;
    }

    if (active_ks->selected_corner < 0 || (move_x == 0.0f && move_y == 0.0f)) {
        return false;
    }
    
    // Only allow movement if border or corners are visible
    if (!active_ks->show_border && !active_ks->show_corners) {
        return false;
    }

    float speed_scale = 1.0f;
    if (target_frame_time > 0.0 && delta_time > 0.0) {
        speed_scale = (float)(delta_time / target_frame_time);
        if (speed_scale < 0.25f) speed_scale = 0.25f;
        if (speed_scale > 3.0f) speed_scale = 3.0f;
    }

    keystone_move_corner(active_ks, move_x * speed_scale, move_y * speed_scale);
    app->needs_redraw = true;
    return true;
}

// PRODUCTION FIX: Protect next_cpu_core with mutex to prevent race on multi-thread startup
static pthread_mutex_t cpu_core_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_cpu_core = 2;  // Start from core 2

// Async decode thread function for video 2
static void* async_decode_thread(void *arg) {
    async_decode_t *decoder = (async_decode_t *)arg;

    // PRODUCTION FIX: Disable thread cancellation to prevent use-after-free
    // pthread_cancel during FFmpeg decode causes memory corruption
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    // OPTIMIZATION: Pin decode threads to specific CPU cores for better cache utilization
    // RPi4 has 4 cores (0-3). Pin background decode threads to cores 2-3
    pthread_mutex_lock(&cpu_core_mutex);
    int assigned_core = next_cpu_core;
    next_cpu_core = (next_cpu_core == 2) ? 3 : 2;  // Alternate between cores 2 and 3
    pthread_mutex_unlock(&cpu_core_mutex);
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(assigned_core, &cpuset);

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        LOG_DEBUG("ASYNC", "Decode thread pinned to CPU core %d", assigned_core);
    }

    while (!decoder->should_exit) {
        pthread_mutex_lock(&decoder->mutex);
        
        // Wait for signal to decode
        while (!decoder->decoding && !decoder->should_exit) {
            pthread_cond_wait(&decoder->cond, &decoder->mutex);
        }
        
        if (decoder->should_exit) {
            pthread_mutex_unlock(&decoder->mutex);
            break;
        }
        
        decoder->decoding = false;
        pthread_mutex_unlock(&decoder->mutex);
        
        // Decode frame (outside lock to allow main thread to continue)
        int result = video_decode_frame(decoder->video);
        
        pthread_mutex_lock(&decoder->mutex);
        if (result == 0) {
            decoder->frame_ready = true;
            // Signal that frame is ready (for async_decode_wait_frame)
            pthread_cond_signal(&decoder->cond);
        }
        pthread_mutex_unlock(&decoder->mutex);
    }
    
    return NULL;
}

// Create async decoder for video 2
async_decode_t* async_decode_create(video_context_t *video) {
    async_decode_t *decoder = (async_decode_t *)malloc(sizeof(async_decode_t));
    if (!decoder) {
        LOG_ERROR("ASYNC", "Failed to allocate async decoder");
        return NULL;
    }
    
    decoder->video = video;
    decoder->frame_ready = false;
    decoder->decoding = false;
    decoder->should_exit = false;
    decoder->running = false;
    
    if (pthread_mutex_init(&decoder->mutex, NULL) != 0) {
        LOG_ERROR("ASYNC", "Failed to initialize decoder mutex");
        free(decoder);
        return NULL;
    }
    
    if (pthread_cond_init(&decoder->cond, NULL) != 0) {
        LOG_ERROR("ASYNC", "Failed to initialize decoder condition variable");
        pthread_mutex_destroy(&decoder->mutex);
        free(decoder);
        return NULL;
    }
    
    // Start decode thread
    if (pthread_create(&decoder->thread, NULL, async_decode_thread, decoder) != 0) {
        LOG_ERROR("ASYNC", "Failed to create async decode thread");
        pthread_cond_destroy(&decoder->cond);
        pthread_mutex_destroy(&decoder->mutex);
        free(decoder);
        return NULL;
    }
    
    decoder->running = true;
    return decoder;
}

// Destroy async decoder
void async_decode_destroy(async_decode_t *decoder) {
    if (!decoder) return;

    // Signal thread to exit
    int lock_result = pthread_mutex_lock(&decoder->mutex);
    if (lock_result != 0) {
        LOG_WARN("ASYNC", "mutex lock failed in destroy: %d", lock_result);
        // Try to proceed anyway for cleanup
    }
    decoder->should_exit = true;
    pthread_cond_broadcast(&decoder->cond);  // Use broadcast to wake all waiters
    if (lock_result == 0) {
        pthread_mutex_unlock(&decoder->mutex);
    }

    // PRODUCTION FIX: Use longer timeout (1 second) to allow FFmpeg decode to complete
    // DO NOT use pthread_cancel - it causes use-after-free when FFmpeg is mid-decode
    // FFmpeg decode can take up to ~50ms for 1080p, but add margin for slow frames
    if (decoder->running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;  // 1 second timeout (was 200ms - too short for FFmpeg)

        int result = pthread_timedjoin_np(decoder->thread, NULL, &timeout);
        if (result == ETIMEDOUT) {
            // Thread did not exit in time - this is a critical error
            // DO NOT cancel - would cause use-after-free in FFmpeg
            LOG_ERROR("ASYNC", "Thread did not exit within 1 second - possible deadlock");
            LOG_ERROR("ASYNC", "Leaking decoder struct to prevent use-after-free corruption");
            // PRODUCTION FIX: Do NOT free decoder - thread may still be accessing it!
            // Accept the memory leak (small) rather than risk use-after-free (crash)
            // The thread will eventually exit when the process terminates
            decoder->running = false;
            // Do NOT free(decoder) - thread may still reference it
            // Do NOT destroy mutex/cond - thread may still be waiting on them
            return;
        } else if (result != 0) {
            LOG_WARN("ASYNC", "pthread_timedjoin_np failed: %d", result);
        }
        decoder->running = false;
    }

    // PRODUCTION FIX: Never destroy a locked mutex (POSIX undefined behavior)
    // Wait for mutex to be released with timeout before destroying
    int max_wait_attempts = 20;  // 20 * 10ms = 200ms max wait
    int trylock_result;
    for (int attempt = 0; attempt < max_wait_attempts; attempt++) {
        trylock_result = pthread_mutex_trylock(&decoder->mutex);
        if (trylock_result == 0) {
            // Successfully acquired - unlock and we're ready to destroy
            pthread_mutex_unlock(&decoder->mutex);
            break;
        } else if (trylock_result == EBUSY) {
            // Mutex is locked by another thread - wait briefly
            if (attempt == 0) {
                LOG_DEBUG("ASYNC", "Waiting for mutex release during cleanup...");
            }
            usleep(10000);  // 10ms wait
        } else {
            // Other error (EINVAL = already destroyed, etc)
            LOG_WARN("ASYNC", "Unexpected trylock result: %d", trylock_result);
            break;
        }
    }
    if (trylock_result == EBUSY) {
        LOG_ERROR("ASYNC", "Mutex still locked after 200ms - cleanup may leak resources");
        // Don't destroy - would be undefined behavior. Accept the leak.
        free(decoder);
        return;
    }
    
    // Safe cleanup of synchronization primitives
    int mutex_result = pthread_mutex_destroy(&decoder->mutex);
    if (mutex_result != 0 && mutex_result != EINVAL) {
        LOG_WARN("ASYNC", "pthread_mutex_destroy failed: %d", mutex_result);
    }
    
    int cond_result = pthread_cond_destroy(&decoder->cond);
    if (cond_result != 0 && cond_result != EINVAL) {
        LOG_WARN("ASYNC", "pthread_cond_destroy failed: %d", cond_result);
    }

    free(decoder);
}

// Request decode of next frame
void async_decode_request_frame(async_decode_t *decoder) {
    if (!decoder) return;
    
    int lock_result = pthread_mutex_lock(&decoder->mutex);
    if (lock_result != 0) {
        LOG_WARN("ASYNC", "mutex lock failed in request: %d", lock_result);
        return;
    }
    
    decoder->frame_ready = false;
    decoder->decoding = true;
    pthread_cond_signal(&decoder->cond);
    pthread_mutex_unlock(&decoder->mutex);
}

// Check if frame is ready
bool async_decode_frame_ready(async_decode_t *decoder) {
    if (!decoder) return false;
    
    int lock_result = pthread_mutex_lock(&decoder->mutex);
    if (lock_result != 0) {
        LOG_WARN("ASYNC", "mutex lock failed in frame_ready: %d", lock_result);
        return false;
    }
    
    bool ready = decoder->frame_ready;
    pthread_mutex_unlock(&decoder->mutex);
    
    return ready;
}

// Wait for frame to be ready (with timeout in milliseconds)
bool async_decode_wait_frame(async_decode_t *decoder, int timeout_ms) {
    if (!decoder) return false;
    
    int lock_result = pthread_mutex_lock(&decoder->mutex);
    if (lock_result != 0) {
        LOG_WARN("ASYNC", "mutex lock failed in wait_frame: %d", lock_result);
        return false;
    }
    
    // If already ready, return immediately
    if (decoder->frame_ready) {
        pthread_mutex_unlock(&decoder->mutex);
        return true;
    }
    
    // Wait with timeout using CLOCK_MONOTONIC (not affected by NTP adjustments)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    
    while (!decoder->frame_ready && !decoder->should_exit) {
        if (pthread_cond_timedwait(&decoder->cond, &decoder->mutex, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&decoder->mutex);
            return false;
        }
    }
    
    bool ready = decoder->frame_ready;
    pthread_mutex_unlock(&decoder->mutex);
    return ready;
}

int app_init(app_context_t *app, const char *video_file, const char *video_file2, bool loop_playback,
            bool show_timing, bool debug_gamepad, bool advanced_diagnostics, bool enable_hardware_decode, bool dual_hw_decode) {
    LOG_INFO("APP", "Starting initialization...");
    fflush(stdout);
    
    // Validate video file before proceeding
    if (validate_video_file(video_file) != 0) {
        LOG_ERROR("APP", "Failed to validate video file");
        return -1;
    }
    
    // Validate second video file if provided
    if (video_file2 && validate_video_file(video_file2) != 0) {
        LOG_ERROR("APP", "Failed to validate second video file");
        return -1;
    }
    
    memset(app, 0, sizeof(*app));
    
    // Set all flags
    app->video_file = video_file;
    app->video_file2 = video_file2;
    app->running = true;
    app->loop_playback = loop_playback;
    app->show_timing = show_timing;
    app->debug_gamepad = debug_gamepad;
    app->advanced_diagnostics = advanced_diagnostics;
    app->active_keystone = 0;  // Start with first keystone active
    app->gamepad_corner_cycle_index = -1;  // Reset corner cycle

    // Allocate contexts
    app->drm = calloc(1, sizeof(display_ctx_t));
    app->gl = calloc(1, sizeof(gl_context_t));
    app->video = calloc(1, sizeof(video_context_t));
    app->keystone = calloc(1, sizeof(keystone_context_t));
    app->input = calloc(1, sizeof(input_context_t));
    
    // Allocate second video and keystone if second file provided
    if (video_file2) {
        app->video2 = calloc(1, sizeof(video_context_t));
        app->keystone2 = calloc(1, sizeof(keystone_context_t));
        if (!app->video2 || !app->keystone2) {
            LOG_ERROR("APP", "Failed to allocate second video/keystone contexts");
            app_cleanup(app);
            return -1;
        }
    }

    if (!app->drm || !app->gl || !app->video || !app->keystone || !app->input) {
        LOG_ERROR("APP", "Failed to allocate contexts");
        app_cleanup(app);
        return -1;
    }

    LOG_INFO("APP", "Initializing DRM display...");
    fflush(stdout);
    
    // Initialize DRM display
    if (drm_init(app->drm) != 0) {
        LOG_ERROR("APP", "Failed to initialize DRM display");
        app_cleanup(app);
        return -1;
    }
    
    // Initialize KMS video overlay plane (optional, for hardware zero-copy)
    if (drm_init_video_plane(app->drm) == 0) {
        LOG_INFO("KMS", "Video overlay plane initialized successfully");
    } else {
        LOG_INFO("KMS", "Video overlay plane not available (will use OpenGL fallback)");
    }

    // Initialize OpenGL context
    if (gl_init(app->gl, app->drm) != 0) {
        LOG_ERROR("APP", "Failed to initialize OpenGL context");
        app_cleanup(app);
        return -1;
    }

    // Initialize video decoder
    if (video_init(app->video, video_file, app->advanced_diagnostics, enable_hardware_decode) != 0) {
        LOG_ERROR("APP", "Failed to initialize video decoder");
        app_cleanup(app);
        return -1;
    }

    // Enable pure hardware path if external texture is supported (multi-plane YUV EGLImage)
    // This uses GL_OES_EGL_image_external for true zero-copy rendering
    if (app->video->use_hardware_decode && app->gl->supports_external_texture) {
        app->video->skip_sw_transfer = true;
        LOG_INFO("ZERO-COPY", "Pure hardware path enabled (external texture)");
    } else if (app->video->use_hardware_decode) {
        app->video->skip_sw_transfer = false;  // Fallback to CPU transfer
        LOG_INFO("HW_DECODE", "Using hardware decode with CPU transfer (V4L2 M2M)");
    }

    // Validate video dimensions after decoder opens file
    if (app->video->width > MAX_VIDEO_WIDTH || app->video->height > MAX_VIDEO_HEIGHT) {
        LOG_ERROR("APP", "Video dimensions %dx%d exceed limits (%dx%d max)",
                app->video->width, app->video->height, MAX_VIDEO_WIDTH, MAX_VIDEO_HEIGHT);
        app_cleanup(app);
        return -1;
    }
    
    LOG_INFO("APP", "Video 1 dimensions: %dx%d (within limits)", app->video->width, app->video->height);
    
    // Set loop playback if requested
    video_set_loop(app->video, loop_playback);

    // Create async decoder for primary video (hardware path benefits too)
    bool force_sync_hw = false;
    const char *force_sync_env = getenv("PICKLE_FORCE_SYNC_HW");
    if (force_sync_env && (strcmp(force_sync_env, "1") == 0 ||
                           strcmp(force_sync_env, "true") == 0 ||
                           strcmp(force_sync_env, "yes") == 0)) {
        force_sync_hw = true;
    }

    bool allow_async_primary = true;
    if (app->video->use_hardware_decode && force_sync_hw) {
        allow_async_primary = false;
        LOG_INFO("APP", "PICKLE_FORCE_SYNC_HW=1 -> forcing hardware decode on main thread");
    }

    if (allow_async_primary) {
        app->async_decoder_primary = async_decode_create(app->video);
        if (!app->async_decoder_primary) {
            LOG_ERROR("APP", "Failed to create async decoder for video 1");
            app_cleanup(app);
            return -1;
        }
        LOG_INFO("APP", "Async decoder created for video 1 (%s path)",
               app->video->use_hardware_decode ? "hardware" : "software");
    }

    // Initialize second video decoder if provided
    // HYBRID MODE: Video 2 uses SOFTWARE decode by default to avoid V4L2 M2M contention
    // Dual HW decode tested: works but Video 1 slows down (5ms vs 1ms) due to resource sharing
    // Use --dual-hw to enable dual hardware decode (experimental)
    if (video_file2) {
        bool video2_hw_decode = dual_hw_decode;  // Use HW only with --dual-hw flag
        if (dual_hw_decode) {
            LOG_INFO("HYBRID", "Video 2 using HARDWARE decode (--dual-hw experimental mode)");
        } else if (enable_hardware_decode) {
            LOG_INFO("HYBRID", "Video 2 using software decode (default hybrid mode)");
        }

        if (video_init(app->video2, video_file2, app->advanced_diagnostics, video2_hw_decode) != 0) {
            LOG_ERROR("APP", "Failed to initialize second video decoder");
            app_cleanup(app);
            return -1;
        }

        if (app->video2->width > MAX_VIDEO_WIDTH || app->video2->height > MAX_VIDEO_HEIGHT) {
            LOG_ERROR("APP", "Video 2 dimensions %dx%d exceed limits (%dx%d max)",
                    app->video2->width, app->video2->height, MAX_VIDEO_WIDTH, MAX_VIDEO_HEIGHT);
            app_cleanup(app);
            return -1;
        }

        LOG_INFO("APP", "Video 2 dimensions: %dx%d (within limits)", app->video2->width, app->video2->height);
        video_set_loop(app->video2, loop_playback);

        // Enable pure hardware path for V2 if using dual-hw and external texture is supported
        // This enables zero-copy DMA buffer import for V2 (same as V1)
        if (app->video2->use_hardware_decode && app->gl->supports_external_texture) {
            app->video2->skip_sw_transfer = true;
            LOG_INFO("ZERO-COPY", "Pure hardware path enabled for Video 2 (external texture)");
        } else if (app->video2->use_hardware_decode) {
            app->video2->skip_sw_transfer = false;
            LOG_INFO("HW_DECODE", "Video 2 using hardware decode with CPU transfer");
        }

        // OPTIMIZATION: Enable async decode for Video 2 to overlap decode with rendering
        // This hides the ~7-9ms software decode time, improving performance
        app->async_decoder_secondary = async_decode_create(app->video2);
        if (!app->async_decoder_secondary) {
            LOG_WARN("APP", "Failed to create async decoder for Video 2, using sync decode");
        } else {
            LOG_INFO("APP", "Async decoder created for video 2 (%s path)",
                   app->video2->use_hardware_decode ? "hardware" : "software");
        }
    }

    // Initialize keystone correction
    if (keystone_init(app->keystone) != 0) {
        LOG_ERROR("APP", "Failed to initialize keystone correction");
        app_cleanup(app);
        return -1;
    }
    
    // Load saved keystone settings if available
    if (keystone_load_settings(app->keystone) == 0) {
        LOG_INFO("APP", "Loaded saved keystone settings from pickle_keystone.conf");
    } else {
        LOG_INFO("APP", "No saved keystone settings found, using defaults");
    }
    
    // Force show_corners and show_border to OFF at startup (user toggles with gamepad)
    app->keystone->show_corners = false;
    app->keystone->show_border = false;
    
    // CRITICAL FIX: Verify keystone corners are in correct physical positions
    // If they're inverted or scrambled, reset them to ensure selection matches visual corners
    // Check if corners are in correct order: TL should have Y > BR (Y positive = UP in normalized coords)
    float tl_y = app->keystone->corners[CORNER_TOP_LEFT].y;
    float br_y = app->keystone->corners[CORNER_BOTTOM_RIGHT].y;
    if (tl_y < br_y) {  // TOP_LEFT Y should be > BOTTOM_RIGHT Y (1.0 > -1.0)
        LOG_WARN("KEYSTONE", "Keystone 1 corners are inverted/scrambled! Resetting to defaults");
        LOG_DEBUG("KEYSTONE", "TL Y=%.2f, BR Y=%.2f (expected TL > BR)", tl_y, br_y);
        keystone_reset_corners(app->keystone);
        LOG_INFO("KEYSTONE", "Keystone 1 reset to correct defaults");
    }
    
    // Initialize second keystone if second video provided
    if (video_file2) {
        if (keystone_init(app->keystone2) != 0) {
            LOG_ERROR("APP", "Failed to initialize second keystone correction");
            app_cleanup(app);
            return -1;
        }

        // Try to load saved keystone2 settings
        if (keystone_load_from_file(app->keystone2, "pickle_keystone2.conf") == 0) {
            LOG_INFO("APP", "Loaded saved keystone2 settings from pickle_keystone2.conf");
            // Use saved settings as-is (borders off by default)
            app->keystone2->show_corners = false;
            app->keystone2->show_border = false;
        } else {
            // No keystone2 config found - create default dual-video setup
            LOG_INFO("APP", "No pickle_keystone2.conf found - creating default dual-video setup");

            // Reset keystone 1 to full screen defaults
            keystone_reset_corners(app->keystone);
            LOG_INFO("APP", "Keystone 1 reset to full screen");

            // Set keystone 2 to be inset inside keystone 1 with margin
            float margin = 0.3f;  // 30% margin on each side
            keystone_set_inset_corners(app->keystone2, margin);
            LOG_INFO("APP", "Keystone 2 positioned inside keystone 1 with %.0f%% margin", margin * 100);

            // Enable borders on BOTH keystones so user can see them
            app->keystone->show_border = true;
            app->keystone2->show_border = true;

            // Disable corners and help overlay on both keystones
            app->keystone->show_corners = false;
            app->keystone->show_help = false;
            app->keystone2->show_corners = false;
            app->keystone2->show_help = false;

            LOG_INFO("APP", "Borders enabled on both keystones for visibility");

            // Save both configs
            if (keystone_save_to_file(app->keystone, "pickle_keystone.conf") == 0) {
                LOG_INFO("APP", "Saved default keystone 1 to pickle_keystone.conf");
            }
            if (keystone_save_to_file(app->keystone2, "pickle_keystone2.conf") == 0) {
                LOG_INFO("APP", "Created pickle_keystone2.conf with default inset position");
            }
        }
    }

    // Initialize input handler
    if (input_init(app->input) != 0) {
        LOG_ERROR("APP", "Failed to initialize input handler");
        app_cleanup(app);
        return -1;
    }
    
    // Pass debug flag to input handler
    app->input->debug_gamepad = app->debug_gamepad;

    gl_setup_buffers(app->gl);

    // Application initialization complete
    return 0;
}

void app_run(app_context_t *app) {
    // OPTIMIZATION: Pin main render thread to CPU core 0 for better cache separation
    // Decode threads use cores 2-3, keeping main thread isolated prevents cache thrashing
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        LOG_DEBUG("THREAD", "Main render thread pinned to CPU core 0");
    }

    struct timespec current_time, last_time;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    // Print app configuration to verify settings
    LOG_INFO("APP", "App configuration - Loop: %d, Show timing: %d, Debug gamepad: %d",
           app->loop_playback ? 1 : 0, 
           app->show_timing ? 1 : 0, 
           app->debug_gamepad ? 1 : 0);
    
    // Force timing display for debugging if needed
    if (!app->show_timing) {
        char *env_timing = getenv("PICKLE_SHOW_TIMING");
        if (env_timing && (strcmp(env_timing, "1") == 0 || 
                          strcmp(env_timing, "yes") == 0 || 
                          strcmp(env_timing, "true") == 0)) {
            app->show_timing = true;
            LOG_INFO("APP", "Timing display enabled via PICKLE_SHOW_TIMING environment variable");
        }
    }

    // Derive target frame time from video FPS (no hardcoded values)
    // FIXED: Each video has independent frame timing for correct playback speed
    double target_frame_time;
    double target_frame_time2 = 0.0;  // Video 2's independent frame time

    if (app->video && app->video->fps > 0) {
        target_frame_time = 1.0 / app->video->fps;
        LOG_INFO("APP", "Video 1 FPS: %.2f, Target frame time: %.3fms",
               app->video->fps, target_frame_time * 1000);
    } else {
        // Fallback: use display refresh rate (typically 60Hz) only if video FPS unknown
        target_frame_time = 1.0 / 60.0;
        LOG_WARN("APP", "Video 1 FPS unknown, using display refresh rate (60Hz fallback)");
    }

    // Video 2: Calculate independent frame timing
    if (app->video2 && app->video2->fps > 0) {
        target_frame_time2 = 1.0 / app->video2->fps;
        LOG_INFO("APP", "Video 2 FPS: %.2f, Target frame time: %.3fms",
               app->video2->fps, target_frame_time2 * 1000);
    } else if (app->video2) {
        // Fallback for video 2: use display refresh rate
        target_frame_time2 = 1.0 / 60.0;
        LOG_WARN("APP", "Video 2 FPS unknown, using display refresh rate (60Hz fallback)");
    }
    
    // FIXED: VSync handles frame timing - no manual budgets needed

    // Video 1 frame presentation timing for pacing
    // At 60Hz display with 60fps video, VSync handles pacing naturally
    // At higher refresh rates (120Hz+), this ensures we don't consume faster than video FPS
    double last_frame_present_time = 0.0;

    // Video 2 frame presentation timing for independent pacing
    // Each video plays at its own native FPS, no synchronization needed
    double last_frame_present_time2 = 0.0;

    // Frame timing diagnostics
    // Timing measurement variables (render_start/render_end now local to render section)
    struct timespec decode_start, decode_end;
    double total_decode_time = 0, total_render_time = 0;
    double decode0_time = 0.0, decode1_time = 0.0;  // Track both video streams separately
    int diagnostic_frame_count = 0;  // Counts successful decodes
    int render_frame_count = 0;       // Counts every frame rendered (for accurate avg)
    double nv12_interval_sum[2] = {0.0, 0.0};
    double nv12_interval_min[2] = {1e9, 1e9};
    double nv12_interval_max[2] = {0.0, 0.0};
    int nv12_interval_count[2] = {0, 0};
    double gl_upload_interval_sum[2] = {0.0, 0.0};
    double gl_upload_interval_min[2] = {1e9, 1e9};
    double gl_upload_interval_max[2] = {0.0, 0.0};
    int gl_upload_interval_count[2] = {0, 0};
    
    // Frame timing analysis buffers
    // PRODUCTION: Define buffer size as constant for maintainability
    #define TIMING_BUFFER_FRAMES 300
    double *decode_times = malloc(TIMING_BUFFER_FRAMES * sizeof(double));
    double *render_times = malloc(TIMING_BUFFER_FRAMES * sizeof(double));
    
    // PRODUCTION FIX: Handle partial allocation properly - if one succeeds and other fails,
    // we must free the successful one before setting both to NULL
    if (!decode_times || !render_times) {
        LOG_ERROR("APP", "Failed to allocate timing buffers (decode=%p, render=%p)",
                  (void*)decode_times, (void*)render_times);
        if (decode_times) { free(decode_times); decode_times = NULL; }
        if (render_times) { free(render_times); render_times = NULL; }
    }
    
    int timing_buffer_idx = 0;
    int timing_samples = 0;
    
    // Add a timer to ensure we print timing data periodically regardless of frame counts
    struct timespec last_timing_report;
    clock_gettime(CLOCK_MONOTONIC, &last_timing_report);
    
    // Initialize timing metrics display
    if (app->show_timing) {
        LOG_INFO("TIMING", "Timing display is enabled. Will show metrics every 30 frames");
        LOG_INFO("TIMING", "Video FPS: %.2f, Target frame time: %.3fms", 
               app->video->fps, target_frame_time * 1000);
        LOG_INFO("TIMING", "Hardware decode: %s, Resolution: %dx%d", 
               video_is_hardware_decoded(app->video) ? "YES" : "NO",
               app->video->width, app->video->height);
               
        // Start a background timer to ensure timing info is displayed
        // regardless of frame decode success
        LOG_DEBUG("TIMING", "Starting timing display timer");
        
        // Write directly to a file as a backup method for timing data
        FILE *timing_log = fopen("timing_log.txt", "w");
        if (timing_log) {
            fprintf(timing_log, "Pickle timing log started\n");
            fprintf(timing_log, "Video FPS: %.2f, Target frame time: %.3fms\n", 
                   app->video->fps, target_frame_time * 1000);
            fprintf(timing_log, "Hardware decode: %s, Resolution: %dx%d\n", 
                   video_is_hardware_decoded(app->video) ? "YES" : "NO",
                   app->video->width, app->video->height);
            fclose(timing_log);
        }
    }

    // Main loop starting
    double startup_time = (double)last_time.tv_sec + last_time.tv_nsec / 1e9;
    bool first_decode_attempted = false;
    bool primary_async_requested = false;
    bool primary_async_request_pending = false;
    bool secondary_async_requested = false;  // Track async decode requests for Video 2
    
    while (app->running && !g_quit_requested) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // Calculate frame time
        double delta_time = (current_time.tv_sec - last_time.tv_sec) +
                           (current_time.tv_nsec - last_time.tv_nsec) / 1e9;
        
        double current_total_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
        double decode_time = 0.0, render_time = 0.0;
        decode0_time = 0.0;  // Reset decode0 timing for this frame
        decode1_time = 0.0;  // Reset decode1 timing for this frame
        double nv12_frame_time[2] = {-1.0, -1.0};
        double upload_frame_time[2] = {-1.0, -1.0};
        
        // Handle input regardless of video state
        input_update(app->input);
        
        // Check for quit
        if (input_should_quit(app->input)) {
            LOG_INFO("APP", "Quit requested by user");
            app->running = false;
            break;
        }

        // Handle keystone corner selection - standard clockwise numbering from top-left
        // Keys 1-4 select corners for first keystone (video 1)
        // Keys 5-8 select corners for second keystone (video 2)
        // Standard layout: 1=top-left, 2=top-right, 3=bottom-right, 4=bottom-left (clockwise)
        if (input_is_key_just_pressed(app->input, KEY_1)) {
            keystone_select_corner(app->keystone, CORNER_TOP_LEFT);      // Key "1" = TL
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_2)) {
            keystone_select_corner(app->keystone, CORNER_TOP_RIGHT);     // Key "2" = TR
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_3)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_RIGHT);  // Key "3" = BR
            app->active_keystone = 0;
        } else if (input_is_key_just_pressed(app->input, KEY_4)) {
            keystone_select_corner(app->keystone, CORNER_BOTTOM_LEFT);   // Key "4" = BL
            app->active_keystone = 0;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_5)) {
            keystone_select_corner(app->keystone2, CORNER_TOP_LEFT);     // Key "5" = TL
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_6)) {
            keystone_select_corner(app->keystone2, CORNER_TOP_RIGHT);    // Key "6" = TR
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_7)) {
            keystone_select_corner(app->keystone2, CORNER_BOTTOM_RIGHT); // Key "7" = BR
            app->active_keystone = 1;
        } else if (app->keystone2 && input_is_key_just_pressed(app->input, KEY_8)) {
            keystone_select_corner(app->keystone2, CORNER_BOTTOM_LEFT);  // Key "8" = BL
            app->active_keystone = 1;
        }
        
        // Check for arrow key input (simply helps the corner movement by triggering input_is_key_pressed)

        // Get active keystone
        keystone_context_t *active_ks = (app->active_keystone == 1 && app->keystone2) ? app->keystone2 : app->keystone;
        
        // Reset keystone to defaults
        if (input_is_key_just_pressed(app->input, KEY_R)) {
            // Reset keystone 1 to full screen
            keystone_reset_corners(app->keystone);
            LOG_INFO("KEYSTONE", "Keystone 1 reset to defaults");
            
            // If second video exists, reset keystone 2 to a smaller inset area
            if (app->keystone2) {
                // Reset to defaults first
                keystone_reset_corners(app->keystone2);
                
                // Then adjust to be slightly inset (10% margin on each side)
                float inset = 0.10f; // 10% inset from edges
                app->keystone2->corners[CORNER_TOP_LEFT].x = -1.0f + inset;      // TL X
                app->keystone2->corners[CORNER_TOP_LEFT].y = 1.0f - inset;       // TL Y
                app->keystone2->corners[CORNER_TOP_RIGHT].x = 1.0f - inset;      // TR X
                app->keystone2->corners[CORNER_TOP_RIGHT].y = 1.0f - inset;      // TR Y
                app->keystone2->corners[CORNER_BOTTOM_RIGHT].x = 1.0f - inset;   // BR X
                app->keystone2->corners[CORNER_BOTTOM_RIGHT].y = -1.0f + inset;  // BR Y
                app->keystone2->corners[CORNER_BOTTOM_LEFT].x = -1.0f + inset;   // BL X
                app->keystone2->corners[CORNER_BOTTOM_LEFT].y = -1.0f + inset;   // BL Y
                
                keystone_calculate_matrix(app->keystone2);
                LOG_INFO("KEYSTONE", "Keystone 2 reset to inset defaults (visible inside keystone 1)");
            }
        }
        
        // Save keystone settings (S key or P key for compatibility)
        if (app->input->save_keystone) {
            // Save both keystones
            bool saved1 = false, saved2 = false;

            if (keystone_save_settings(app->keystone) == 0) {
                LOG_INFO("KEYSTONE", "Keystone 1 settings saved to pickle_keystone.conf");
                saved1 = true;
            } else {
                LOG_ERROR("KEYSTONE", "Failed to save keystone 1 settings");
            }

            if (app->keystone2) {
                if (keystone_save_to_file(app->keystone2, "pickle_keystone2.conf") == 0) {
                    LOG_INFO("KEYSTONE", "Keystone 2 settings saved to pickle_keystone2.conf");
                    saved2 = true;
                } else {
                    LOG_ERROR("KEYSTONE", "Failed to save keystone 2 settings");
                }
            }

            // Show notification overlay
            if (saved1 && saved2) {
                LOG_INFO("KEYSTONE", "Both keystone configurations saved successfully");
                show_notification(app, "Settings Saved!", 3.0);
            } else if (saved1) {
                show_notification(app, "Keystone 1 Saved!", 3.0);
            } else if (saved2) {
                show_notification(app, "Keystone 2 Saved!", 3.0);
            } else {
                show_notification(app, "Save Failed!", 3.0);
            }

            app->input->save_keystone = false; // Reset flag
        }
        
        // Toggle corner visibility
        if (app->input->toggle_corners) {
            if (app->keystone2) {
                // Two videos: toggle BOTH keystones
                app->keystone->show_corners = !app->keystone->show_corners;
                app->keystone2->show_corners = !app->keystone2->show_corners;
                LOG_INFO("TOGGLE", "Corners: %s (both keystone 1 & 2)", 
                       app->keystone->show_corners ? "ON" : "OFF");
            } else {
                // Single video: toggle only keystone 1
                app->keystone->show_corners = !app->keystone->show_corners;
                LOG_INFO("TOGGLE", "Corners: %s (keystone 1 only)", 
                       app->keystone->show_corners ? "ON" : "OFF");
            }
            app->input->toggle_corners = false;
        }
        
        // Toggle border visibility
        if (app->input->toggle_border) {
            if (app->keystone2) {
                // Two videos: toggle BOTH keystones
                app->keystone->show_border = !app->keystone->show_border;
                app->keystone2->show_border = !app->keystone2->show_border;
            } else {
                // Single video: toggle only keystone 1
                app->keystone->show_border = !app->keystone->show_border;
            }
            app->input->toggle_border = false;
        }
        
        // Toggle help overlay visibility
        if (app->input->toggle_help) {
            keystone_toggle_help(active_ks);
            app->input->toggle_help = false; // Reset flag
        }
        
        // Gamepad-specific actions
        if (app->input->gamepad_enabled) {
            // X button: Cycle through corners
            if (app->input->gamepad_cycle_corner) {
                // If video 2 EXISTS, cycle through all 8 corners (both keystones)
                // Otherwise, cycle through only the 4 corners of the active keystone
                
                if (app->keystone2) {
                    // Dual video mode - cycle through all 8 corners (1-8 mode)
                    
                    // Initialize index if first press
                    if (app->gamepad_corner_cycle_index < 0 || app->gamepad_corner_cycle_index >= 8) {
                        app->gamepad_corner_cycle_index = 0;
                    }
                    
                    // Determine which keystone and corner to select based on current index
                    int next_index = app->gamepad_corner_cycle_index;  // Cache the index we're about to use
                    keystone_context_t *target_keystone;
                    int target_corner;
                    
                    if (next_index < 4) {
                        // Corners 0-3: Video 1 (corners TL, TR, BR, BL)
                        target_keystone = app->keystone;
                        target_corner = next_index;  // 0, 1, 2, or 3
                        app->active_keystone = 0;
                    } else {
                        // Corners 4-7: Video 2 (corners TL, TR, BR, BL)
                        target_keystone = app->keystone2;
                        target_corner = next_index - 4;  // 0, 1, 2, or 3
                        app->active_keystone = 1;
                    }

                    // Select the corner
                    keystone_select_corner(target_keystone, target_corner);
                    target_keystone->show_corners = true;
                    
                    // Increment to next position for next button press
                    app->gamepad_corner_cycle_index = (next_index + 1) % 8;
                } else {
                    // Single video mode - cycle through current keystone's 4 corners
                    int current = active_ks->selected_corner;
                    
                    // If no corner selected, start with top-left
                    if (current < 0 || current > 3) {
                        current = CORNER_TOP_LEFT;
                        keystone_select_corner(active_ks, current);
                    } else {
                        // Cycle clockwise: TOP_LEFT(0) -> TOP_RIGHT(1) -> BOTTOM_RIGHT(2) -> BOTTOM_LEFT(3) -> TOP_LEFT
                        int next;
                        switch (current) {
                            case CORNER_TOP_LEFT:     next = CORNER_TOP_RIGHT; break;
                            case CORNER_TOP_RIGHT:    next = CORNER_BOTTOM_RIGHT; break;
                            case CORNER_BOTTOM_RIGHT: next = CORNER_BOTTOM_LEFT; break;
                            case CORNER_BOTTOM_LEFT:  next = CORNER_TOP_LEFT; break;
                            default:                  next = CORNER_TOP_LEFT; break;
                        }
                        keystone_select_corner(active_ks, next);
                    }
                }
                app->input->gamepad_cycle_corner = false;
            }
            
            // L1/R1: Decrease/increase step size
            if (app->input->gamepad_decrease_step) {
                keystone_decrease_step_size(active_ks);
                LOG_INFO("GAMEPAD", "R1 - Step size decreased to %.6f (keystone %d)", active_ks->move_step, app->active_keystone + 1);
                app->input->gamepad_decrease_step = false;
            }
            if (app->input->gamepad_increase_step) {
                keystone_increase_step_size(active_ks);
                LOG_INFO("GAMEPAD", "L1 - Step size increased to %.6f (keystone %d)", active_ks->move_step, app->active_keystone + 1);
                app->input->gamepad_increase_step = false;
            }
            
            // SELECT: Reset keystone
            if (app->input->gamepad_reset_keystone) {
                // Reset keystone 1 to full screen
                keystone_reset_corners(app->keystone);
                LOG_INFO("KEYSTONE", "Keystone 1 reset to defaults (gamepad)");
                
                // If second video exists, reset keystone 2 to a smaller inset area
                if (app->keystone2) {
                    // Reset to defaults first
                    keystone_reset_corners(app->keystone2);
                    
                    // Then adjust to be slightly inset (10% margin on each side)
                    float inset = 0.10f; // 10% inset from edges
                    app->keystone2->corners[CORNER_TOP_LEFT].x = -1.0f + inset;      // TL X
                    app->keystone2->corners[CORNER_TOP_LEFT].y = 1.0f - inset;       // TL Y
                    app->keystone2->corners[CORNER_TOP_RIGHT].x = 1.0f - inset;      // TR X
                    app->keystone2->corners[CORNER_TOP_RIGHT].y = 1.0f - inset;      // TR Y
                    app->keystone2->corners[CORNER_BOTTOM_RIGHT].x = 1.0f - inset;   // BR X
                    app->keystone2->corners[CORNER_BOTTOM_RIGHT].y = -1.0f + inset;  // BR Y
                    app->keystone2->corners[CORNER_BOTTOM_LEFT].x = -1.0f + inset;   // BL X
                    app->keystone2->corners[CORNER_BOTTOM_LEFT].y = -1.0f + inset;   // BL Y
                    
                    keystone_calculate_matrix(app->keystone2);
                    LOG_INFO("KEYSTONE", "Keystone 2 reset to inset defaults (gamepad)");
                }
                
                app->input->gamepad_reset_keystone = false;
            }
            
            // START: Toggle keystone mode (corners visibility)
            if (app->input->gamepad_toggle_mode) {
                if (app->keystone2) {
                    // Two videos: toggle BOTH keystones
                    keystone_toggle_corners(app->keystone);
                    keystone_toggle_corners(app->keystone2);
                } else {
                    // Single video: toggle only keystone 1
                    keystone_toggle_corners(app->keystone);
                }
                app->input->gamepad_toggle_mode = false;
            }
            
            // B button: Toggle both corners and borders on all keystones
            if (app->input->gamepad_toggle_corner_border) {
                if (app->keystone2) {
                    app->keystone->show_corners = !app->keystone->show_corners;
                    app->keystone->show_border = !app->keystone->show_border;
                    app->keystone2->show_corners = !app->keystone2->show_corners;
                    app->keystone2->show_border = !app->keystone2->show_border;
                } else {
                    app->keystone->show_corners = !app->keystone->show_corners;
                    app->keystone->show_border = !app->keystone->show_border;
                }
                app->input->gamepad_toggle_corner_border = false;
            }
        }

        // OPTIMIZATION: Pre-decode next frame while rendering current frame
        // This hides decode latency by overlapping decode with render/swap
        static bool next_frame_ready = false;
        static bool next_frame_ready2 = false;  // Pre-decode flag for Video 2
        static bool first_frame_decoded = false;
        static bool first_frame_decoded2 = false;
        const bool using_async_primary = (app->async_decoder_primary != NULL);
        
        // Decode video frame continuously - vsync will handle timing
        uint8_t *video_data = NULL;
        // PRODUCTION FIX: Make video2 YUV pointers static to prevent flickering
        // These hold the last valid frame data even when async decoder isn't ready
        static uint8_t *y_data2 = NULL, *u_data2 = NULL, *v_data2 = NULL;
        static int y_stride2 = 0, u_stride2 = 0, v_stride2 = 0;
        static int frame_count = 0;
        static int frame_count2 = 0;
        static uint8_t *last_video_data = NULL;
        bool new_primary_frame_ready = false;
        bool new_secondary_frame_ready = false;

        // FIXED: Always decode/render, let vsync handle frame timing
        // Old buggy logic: if (delta_time >= frame_budget) caused stuttering
        {
            // Update timing for diagnostics
            last_time = current_time;

            // Frame pacing: Check if enough time has passed since last frame consumption
            // This ensures correct playback speed at any display refresh rate
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            double current_total_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
            
            // Video 1: Should we consume the next frame?
            bool should_consume_v1 = (current_total_time - last_frame_present_time >= target_frame_time);

            if (using_async_primary) {
                decode_time = 0.0; // Decode work happens on background thread
                if (frame_count == 0 && !first_decode_attempted) {
                    LOG_INFO("DECODE", "Attempting first frame decode (async)...");
                    first_decode_attempted = true;
                }

                int wait_timeout_ms = first_frame_decoded ? 0 : 100;

                // Always request decode if none pending - keeps decoder thread busy
                if (!primary_async_requested) {
                    async_decode_request_frame(app->async_decoder_primary);
                    primary_async_requested = true;
                }

                // Only try to consume if timing says we need a new frame
                // This prevents consuming frames faster than the video's native FPS
                if (should_consume_v1 && async_decode_wait_frame(app->async_decoder_primary, wait_timeout_ms)) {
                    // Check for new frame: either YUV data (SW/fallback) or DMA buffer (pure HW)
                    bool frame_available = false;

                    if (app->video->skip_sw_transfer && video_has_dma_buffer(app->video)) {
                        // Pure hardware path: DMA buffer is the frame
                        frame_available = true;
                    } else {
                        // Software/fallback path: check YUV data
                        uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
                        int y_stride = 0, u_stride = 0, v_stride = 0;
                        video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
                        if (y_data != NULL) {
                            video_data = y_data;
                            frame_available = true;
                        }
                    }

                    if (frame_available) {
                        last_video_data = video_data;
                        frame_count++;
                        
                        // Update last_frame_present_time when we consume a frame
                        // This ensures correct timing at any display refresh rate
                        last_frame_present_time = current_total_time;
                        
                        // Mark frame ready for render
                        new_primary_frame_ready = true;

                        if (!first_frame_decoded) {
                            LOG_INFO("DECODE", "First frame decoded successfully (async)");
                            first_frame_decoded = true;
                        }

                        diagnostic_frame_count++;
                    }

                    primary_async_requested = false;
                    primary_async_request_pending = !video_is_eof(app->video);
                }
                // Note: No yield here - V2 decode runs on separate thread (cores 2-3)
                // Main thread should process V2 frames as fast as possible

                if (video_is_eof(app->video)) {
                    if (app->loop_playback) {
                        LOG_INFO("APP", "End of video reached - restarting playback (loop mode)");

                        // CRITICAL: Wait for any pending async decode to complete before seeking
                        // This prevents V4L2 M2M decoder from getting into a bad state
                        if (primary_async_requested && app->async_decoder_primary) {
                            async_decode_wait_frame(app->async_decoder_primary, 100);
                        }

                        // Reset async decoder state before seeking
                        primary_async_requested = false;
                        primary_async_request_pending = false;

                        // Now safe to seek
                        // video_seek() now resets frame_count to 0, ensuring MAX_PACKETS_INITIAL
                        // is used for re-priming the V4L2 M2M decoder (50 packets vs 10)
                        video_seek(app->video, 0);

                        // Reset frame state
                        next_frame_ready = false;
                        first_frame_decoded = false;
                        first_decode_attempted = false;
                        frame_count = 0;

                        // Update timing
                        clock_gettime(CLOCK_MONOTONIC, &current_time);
                        startup_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
                        last_frame_present_time = current_total_time;  // Reset timing for loop
                    } else {
                        LOG_INFO("APP", "Playback finished");
                        app->running = false;
                        break;
                    }
                }
            } else {
                if (frame_count == 0 && !first_decode_attempted) {
                    LOG_INFO("DECODE", "Attempting first frame decode...");
                    first_decode_attempted = true;
                    
                    // Hardware decode: Pre-buffer 2 frames to prime the decoder pipeline
                    // More buffering causes systematic lag (playback appears slower)
                    if (app->video && video_is_hardware_decoded(app->video)) {
                        LOG_INFO("HW_DECODE", "Priming decoder pipeline...");
                        for (int prebuf = 0; prebuf < 2; prebuf++) {
                            int result = video_decode_frame(app->video);
                            if (result != 0) break;
                        }
                        LOG_INFO("HW_DECODE", "Decoder ready, starting playback");
                    }
                }

                // Add timeout for first decode to prevent hanging
                if (frame_count == 0 && (current_total_time - startup_time) > 5.0) {
                    LOG_WARN("DECODE", "Video decode timeout after 5 seconds, continuing without video...");
                    frame_count = 1; // Skip further decode attempts
                } else {
                    // OPTIMIZATION: Use pre-decoded frame if available, then decode next
                    if (next_frame_ready && first_frame_decoded) {
                        // Current frame was already decoded - just use it (zero decode time!)
                        decode_time = 0.0;
                        
                        // Get YUV data from already-decoded frame
                        uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
                        int y_stride = 0, u_stride = 0, v_stride = 0;
                        video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
                        
                        if (y_data != NULL) {
                            video_data = y_data;
                            last_video_data = video_data;
                            frame_count++;
                            new_primary_frame_ready = true;
                            
                            total_decode_time += decode_time;
                            diagnostic_frame_count++;
                            
                            if (app->show_timing && (frame_count % 10 == 0 || frame_count < 10)) {
                                fflush(stdout);
                            }
                            
                            // Now decode the NEXT frame (during rendering of current frame)
                            // This will be ready for next iteration
                            next_frame_ready = false;
                            // Actual decode happens after rendering (see below)
                        }
                    } else {
                        // Normal decode path (first frame or fallback)
                        clock_gettime(CLOCK_MONOTONIC, &decode_start);
                        int decode_result = video_decode_frame(app->video);
                        clock_gettime(CLOCK_MONOTONIC, &decode_end);
                        
                        decode_time = (decode_end.tv_sec - decode_start.tv_sec) +
                                    (decode_end.tv_nsec - decode_start.tv_nsec) / 1e9;
                        decode0_time = decode_time;  // Track decode0 timing
                        
                        if (decode_result == 0) {
                            if (frame_count == 0) {
                                LOG_INFO("DECODE", "First frame decoded successfully");
                                first_frame_decoded = true;
                            }
                            
                            uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
                            int y_stride = 0, u_stride = 0, v_stride = 0;
                            video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
                            
                            if (y_data != NULL) {
                                video_data = y_data;
                                last_video_data = video_data;
                                frame_count++;
                                new_primary_frame_ready = true;
                                
                                total_decode_time += decode_time;
                                diagnostic_frame_count++;
                                
                                if (app->show_timing && (frame_count % 10 == 0 || frame_count < 10)) {
                                    fflush(stdout);
                                }
                                
                                // Mark that we should pre-decode next time
                                next_frame_ready = true;
                            }
                        } else if (video_is_eof(app->video)) {
                            if (app->loop_playback) {
                                LOG_INFO("APP", "End of video reached - restarting playback (loop mode)");
                                video_seek(app->video, 0);
                                next_frame_ready = false;
                                first_frame_decoded = false;
                                first_decode_attempted = false;  // Reset to allow "Attempting first frame" message
                                frame_count = 0;
                                // Update startup_time to reset the 5-second timeout
                                clock_gettime(CLOCK_MONOTONIC, &current_time);
                                startup_time = (double)current_time.tv_sec + current_time.tv_nsec / 1e9;
                            } else {
                                LOG_INFO("APP", "Playback finished");
                                app->running = false;
                                break;
                            }
                        } else {
                            if (frame_count < 10) {
                                LOG_WARN("DECODE", "Video decode failed: %d", decode_result);
                            }
                            video_data = last_video_data;
                            next_frame_ready = false;
                        }
                    }
                }
            }
        } // End of main decode/render block

        // Decode second video if available
        // ASYNC OPTIMIZATION: Use async decoder to overlap decode with rendering
        if (app->video2) {
            // Video 2: Should we consume the next frame? (independent timing from V1)
            // Dual-HW mode: Use 1.0 threshold (zero-copy, no GL overhead)
            // Software mode: Use 0.90 threshold to compensate for 12ms GL upload overhead
            double timing_threshold = app->video2->use_hardware_decode ? 1.0 : 0.90;
            bool should_consume_v2 = (current_total_time - last_frame_present_time2 >= target_frame_time2 * timing_threshold);

            if (app->async_decoder_secondary) {
                // AGGRESSIVE PRE-DECODE: Always keep decode thread busy by requesting next frame
                // This ensures frames are ready immediately when timing allows consumption
                if (!secondary_async_requested) {
                    async_decode_request_frame(app->async_decoder_secondary);
                    secondary_async_requested = true;
                }

                // ASYNC PATH: Check if frame is ready from background thread
                // Hardware decode: Short wait for V4L2 M2M (prevents DMA buffer staleness)
                // Software decode: Non-blocking after first frame (decoder is fast)
                int wait_timeout_ms = first_frame_decoded2 ?
                                     (app->video2->use_hardware_decode ? 5 : 0) : 100;

                // Check if frame is available (non-blocking after first frame)
                if (async_decode_wait_frame(app->async_decoder_secondary, wait_timeout_ms)) {
                    // Frame is ready - decide whether to consume based on timing
                    bool frame_available2 = false;

                    if (app->video2->skip_sw_transfer && video_has_dma_buffer(app->video2)) {
                        // Pure hardware path: DMA buffer is the frame
                        frame_available2 = true;
                    } else {
                        // Software/fallback path: check YUV data
                        video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2,
                                           &y_stride2, &u_stride2, &v_stride2);
                        if (y_data2 != NULL) {
                            frame_available2 = true;
                        }
                    }

                    if (frame_available2) {
                        // Frame is available - only consume if timing allows
                        if (should_consume_v2) {
                            frame_count2++;

                            // Update last_frame_present_time2 when we consume a frame
                            // This ensures correct timing at any display refresh rate
                            last_frame_present_time2 = current_total_time;

                            // Mark frame ready for render
                            new_secondary_frame_ready = true;

                            if (!first_frame_decoded2) {
                                LOG_INFO("DECODE", "First frame of video 2 decoded successfully (async)");
                                first_frame_decoded2 = true;
                            }

                            // Request next frame immediately to keep decoder ahead
                            secondary_async_requested = false;
                        }
                        // else: Frame is ready but timing says don't consume yet - hold it for next loop
                    } else {
                        // No frame available, request one
                        secondary_async_requested = false;
                    }
                }

                if (video_is_eof(app->video2)) {
                    if (app->loop_playback) {
                        LOG_INFO("APP", "End of video 2 reached - restarting playback (loop mode)");

                        // CRITICAL: Wait for any pending async decode to complete before seeking
                        if (secondary_async_requested && app->async_decoder_secondary) {
                            async_decode_wait_frame(app->async_decoder_secondary, 100);
                        }

                        // Reset async decoder state before seeking
                        secondary_async_requested = false;

                        // Now safe to seek
                        video_seek(app->video2, 0);

                        // Reset frame state
                        first_frame_decoded2 = false;
                        frame_count2 = 0;

                        // Reset timing for loop
                        last_frame_present_time2 = current_total_time;
                    }
                }
            } else {
                // SYNC FALLBACK: If async decoder creation failed, use synchronous decode
                // Only consume frames according to timing to prevent slow-motion playback
                if (should_consume_v2) {
                    if (next_frame_ready2 && first_frame_decoded2) {
                        video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2,
                                           &y_stride2, &u_stride2, &v_stride2);
                        if (y_data2 != NULL) {
                            frame_count2++;
                            last_frame_present_time2 = current_total_time;
                            new_secondary_frame_ready = true;
                        }
                        next_frame_ready2 = false;
                    } else {
                        int decode_result = video_decode_frame(app->video2);
                        if (decode_result == 0) {
                            video_get_yuv_data(app->video2, &y_data2, &u_data2, &v_data2,
                                               &y_stride2, &u_stride2, &v_stride2);
                            if (y_data2 != NULL) {
                                frame_count2++;
                                last_frame_present_time2 = current_total_time;
                                if (!first_frame_decoded2) {
                                    LOG_INFO("DECODE", "First frame of video 2 decoded successfully (sync fallback)");
                                    first_frame_decoded2 = true;
                                }
                                new_secondary_frame_ready = true;
                                next_frame_ready2 = true;
                            }
                        } else if (video_is_eof(app->video2)) {
                            if (app->loop_playback) {
                                LOG_INFO("APP", "End of video 2 reached - restarting playback (loop mode, sync)");
                                video_seek(app->video2, 0);
                                first_frame_decoded2 = false;
                                next_frame_ready2 = false;
                                frame_count2 = 0;
                                last_frame_present_time2 = current_total_time;  // Reset timing for loop
                            }
                        }
                    }
                }
            }
        }

        if (app->keystone->selected_corner < 0) {
            // No corner selected, clear key states to prevent movement later
            app->input->keys_pressed[KEY_UP] = false;
            app->input->keys_pressed[KEY_DOWN] = false;
            app->input->keys_pressed[KEY_LEFT] = false;
            app->input->keys_pressed[KEY_RIGHT] = false;
        }

        // Get video dimensions
        int video_width = 0, video_height = 0;
        video_get_dimensions(app->video, &video_width, &video_height);
        if (video_width == 0) {
            video_width = 256;
            video_height = 256;
        }
        
        // Get YUV data for GPU rendering (only needed for SW decode path)
        uint8_t *y_data = NULL, *u_data = NULL, *v_data = NULL;
        int y_stride = 0, u_stride = 0, v_stride = 0;
        if (!app->video->skip_sw_transfer) {
            video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
        }
        
        // CRITICAL: Start render timing RIGHT BEFORE actual GL operations
        // Don't include async decode prep, input handling, or other non-render work
        struct timespec render_start, render_end;
        clock_gettime(CLOCK_MONOTONIC, &render_start);
        struct timespec gl_render_start, gl_render_end, overlay_start, overlay_end, swap_start, swap_end;
        
        // Time the main GL rendering
        clock_gettime(CLOCK_MONOTONIC, &gl_render_start);

        // Initialize overlay visibility flags (needed for goto skip path)
        bool any_overlay_visible = (app->keystone && (app->keystone->show_corners ||
                                                     app->keystone->show_border));
        bool any_overlay_visible2 = (app->keystone2 && (app->keystone2->show_corners ||
                                                       app->keystone2->show_border));

        // OPTIMIZATION: Blank video when help overlay is displayed (cleaner, faster, no flicker)
        bool help_visible = (app->keystone && app->keystone->show_help) ||
                           (app->keystone2 && app->keystone2->show_help);

        if (help_visible) {
            // Just render black screen when help is up
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            // Skip video rendering entirely - will render help overlay below
            goto skip_video_render;
        }

        // Check if DMA buffer is available for zero-copy rendering
        bool has_dma = video_has_dma_buffer(app->video);
        bool use_hw_decode = app->video->use_hardware_decode;

        bool rendered = false;
        struct timespec gl_upload_start = {0, 0}, gl_upload_end = {0, 0};

        // PURE HARDWARE PATH: Zero-copy via external texture (multi-plane YUV EGLImage)
        // GPU imports DMA buffer directly - no CPU copy at all
        if (has_dma && use_hw_decode && new_primary_frame_ready && app->gl->supports_external_texture) {
            int dma_fd = video_get_dma_fd(app->video);
            if (dma_fd >= 0) {
                static bool egl_dma_logged = false;
                if (!egl_dma_logged) {
                    LOG_INFO("Render", "Using external texture zero-copy path (pure hardware)");
                    egl_dma_logged = true;
                }

                int plane_offsets[3] = {0, 0, 0};
                int plane_pitches[3] = {0, 0, 0};
                video_get_dma_plane_layout(app->video, plane_offsets, plane_pitches);

                if (app->show_timing) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_start);
                }
                // Try zero-copy render - if it fails, fallback paths below will handle it
                rendered = gl_render_frame_external(app->gl, dma_fd, video_width, video_height,
                                        plane_offsets, plane_pitches,
                                        app->drm, app->keystone, true, 0);
                if (app->show_timing && rendered) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_end);
                    upload_frame_time[0] = timespec_diff_seconds(&gl_upload_start, &gl_upload_end);
                }
            }
        }

        // SOFTWARE DECODE PATH: Direct YUV420P upload (no conversion)
        if (!rendered && !use_hw_decode && new_primary_frame_ready) {
            static bool sw_path_logged = false;
            if (!sw_path_logged) {
                LOG_INFO("Render", "Using CPU upload path (software decode)");
                sw_path_logged = true;
            }

            video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
            if (y_data && u_data && v_data) {
                if (app->show_timing) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_start);
                }
                gl_render_frame(app->gl, y_data, u_data, v_data, video_width, video_height,
                              y_stride, u_stride, v_stride, app->drm, app->keystone, true, 0);
                if (app->show_timing) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_end);
                    upload_frame_time[0] = timespec_diff_seconds(&gl_upload_start, &gl_upload_end);
                }
                rendered = true;
            }
        }

        // FALLBACK: HW decode without DMA support - use direct YUV420P upload
        // The sw_frame is already in YUV420P format, no need for NV12 conversion
        if (!rendered && use_hw_decode && new_primary_frame_ready) {
            static bool fallback_logged = false;
            if (!fallback_logged) {
                LOG_INFO("Render", "Using direct YUV420P path (HW decode, CPU upload)");
                fallback_logged = true;
            }

            // Get YUV data directly - no NV12 conversion needed!
            video_get_yuv_data(app->video, &y_data, &u_data, &v_data, &y_stride, &u_stride, &v_stride);
            if (y_data && u_data && v_data) {
                if (app->show_timing) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_start);
                }
                gl_render_frame(app->gl, y_data, u_data, v_data, video_width, video_height,
                              y_stride, u_stride, v_stride, app->drm, app->keystone, true, 0);
                if (app->show_timing) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_end);
                    upload_frame_time[0] = timespec_diff_seconds(&gl_upload_start, &gl_upload_end);
                }
                rendered = true;
            }
        }

        // When no new frame is ready, skip rendering - keep previous frame on screen
        // Don't render black (causes flashing). Previous frame stays in GPU texture.
        
        // Issue deferred async decode request once we finish uploading the frame
        if (using_async_primary && primary_async_request_pending && !primary_async_requested) {
            async_decode_request_frame(app->async_decoder_primary);
            primary_async_requested = true;
            primary_async_request_pending = false;
        }

        // Render second video with second keystone (don't clear screen)
        if (app->video2 && app->keystone2) {
            // Get dimensions (safe to call anytime)
            int video_width2 = app->video2->width;
            int video_height2 = app->video2->height;
            struct timespec gl_upload_start2 = {0, 0}, gl_upload_end2 = {0, 0};

            bool rendered2 = false;
            bool has_dma2 = video_has_dma_buffer(app->video2);
            bool use_hw_decode2 = app->video2->use_hardware_decode;

            // ZERO-COPY PATH for V2: Use external texture if DMA buffer available
            // This is the fast path (~1ms) - GPU imports DMA buffer directly
            // CRITICAL: Always render if we've had at least one frame (prevents flickering)
            // Only create new EGLImage when new_secondary_frame_ready, else reuse last frame
            if (has_dma2 && use_hw_decode2 && first_frame_decoded2 &&
                app->video2->skip_sw_transfer && app->gl->supports_external_texture) {
                int dma_fd2 = video_get_dma_fd(app->video2);
                if (dma_fd2 >= 0) {
                    static bool egl_dma_logged2 = false;
                    if (!egl_dma_logged2) {
                        LOG_INFO("Render", "V2 using external texture zero-copy path (pure hardware)");
                        egl_dma_logged2 = true;
                    }

                    int plane_offsets2[3] = {0, 0, 0};
                    int plane_pitches2[3] = {0, 0, 0};
                    video_get_dma_plane_layout(app->video2, plane_offsets2, plane_pitches2);

                    if (app->show_timing) {
                        clock_gettime(CLOCK_MONOTONIC, &gl_upload_start2);
                    }
                    // Try zero-copy render - if it fails, CPU fallback will be used below
                    bool zero_copy_ok = gl_render_frame_external(app->gl, dma_fd2, video_width2, video_height2,
                                            plane_offsets2, plane_pitches2,
                                            app->drm, app->keystone2, false, 1);
                    if (app->show_timing && zero_copy_ok) {
                        clock_gettime(CLOCK_MONOTONIC, &gl_upload_end2);
                        upload_frame_time[1] = timespec_diff_seconds(&gl_upload_start2, &gl_upload_end2);
                    }
                    rendered2 = zero_copy_ok;  // Only mark as rendered if zero-copy succeeded
                }
            }

            // CPU UPLOAD PATH for V2: Fallback when zero-copy not available
            // This is the slow path (~10ms) - CPU uploads YUV textures
            if (!rendered2 && first_frame_decoded2 && y_data2 && u_data2 && v_data2) {
                // Only upload if we have a new frame
                uint8_t *p_y = new_secondary_frame_ready ? y_data2 : NULL;
                uint8_t *p_u = new_secondary_frame_ready ? u_data2 : NULL;
                uint8_t *p_v = new_secondary_frame_ready ? v_data2 : NULL;

                if (app->show_timing && new_secondary_frame_ready) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_start2);
                }
                
                // Pass original strides to gl_render_frame
                gl_render_frame(app->gl, p_y, p_u, p_v, video_width2, video_height2,
                              y_stride2, u_stride2, v_stride2, app->drm, app->keystone2, false, 1);
                              
                if (app->show_timing && new_secondary_frame_ready) {
                    clock_gettime(CLOCK_MONOTONIC, &gl_upload_end2);
                    upload_frame_time[1] = timespec_diff_seconds(&gl_upload_start2, &gl_upload_end2);
                }
            }
        }
        
        clock_gettime(CLOCK_MONOTONIC, &gl_render_end);

        // Aggregate timing for both videos (AFTER both videos rendered)
        if (app->show_timing) {
            for (int i = 0; i < 2; ++i) {
                if (nv12_frame_time[i] >= 0.0) {
                    nv12_interval_sum[i] += nv12_frame_time[i];
                    if (nv12_frame_time[i] < nv12_interval_min[i]) {
                        nv12_interval_min[i] = nv12_frame_time[i];
                    }
                    if (nv12_frame_time[i] > nv12_interval_max[i]) {
                        nv12_interval_max[i] = nv12_frame_time[i];
                    }
                    nv12_interval_count[i]++;
                }

                if (upload_frame_time[i] >= 0.0) {
                    gl_upload_interval_sum[i] += upload_frame_time[i];
                    if (upload_frame_time[i] < gl_upload_interval_min[i]) {
                        gl_upload_interval_min[i] = upload_frame_time[i];
                    }
                    if (upload_frame_time[i] > gl_upload_interval_max[i]) {
                        gl_upload_interval_max[i] = upload_frame_time[i];
                    }
                    gl_upload_interval_count[i]++;
                }
            }
        }

skip_video_render:  // Jump here when help is visible to skip video rendering

        // Time overlay rendering
        clock_gettime(CLOCK_MONOTONIC, &overlay_start);
        
        // OPTIMIZED: Only render overlays when actually needed
        // Render overlays for first keystone
        // NOTE: Overlay visibility was already checked above to determine rendering path
        if (any_overlay_visible) {
            // No special GL setup needed - overlays render on top of video
            // (we already switched to OpenGL path if overlays are visible)

            // PRODUCTION FIX: Only deselect keystone1 if keystone2 is the active one
            // This ensures only the ACTIVE keystone's corner is highlighted
            int saved_selected_corner1 = -1;
            if (app->keystone2 && app->active_keystone == 1 && app->keystone2->selected_corner >= 0) {
                // Keystone 2 is active, temporarily hide keystone 1's selection
                saved_selected_corner1 = app->keystone->selected_corner;
                app->keystone->selected_corner = -1;
            }

            // Only render the overlays that are actually visible
            if (app->keystone->show_corners) {
                gl_render_corners(app->gl, app->keystone);
            }
            if (app->keystone->show_border) {
                gl_render_border(app->gl, app->keystone);
                gl_render_display_boundary(app->gl, app->keystone);
            }
            if (app->keystone->show_help) {
                gl_render_help_overlay(app->gl, app->keystone);
            }

            // Restore keystone1's selection if it was hidden
            if (saved_selected_corner1 >= 0) {
                app->keystone->selected_corner = saved_selected_corner1;
            }
            
            // NOTE: State restoration is now handled by gl_render_frame()
            // The main video rendering function detects and fixes state corruption
            
            // Clear any OpenGL errors from overlay rendering
            while (glGetError() != GL_NO_ERROR) {
                // Clear error queue
            }
        }

        // ALWAYS render help overlay if visible (even when other overlays aren't)
        // This allows help to display on blank screen when video is hidden
        if (app->keystone && app->keystone->show_help) {
            gl_render_help_overlay(app->gl, app->keystone);
        }
        if (app->keystone2 && app->keystone2->show_help) {
            gl_render_help_overlay(app->gl, app->keystone2);
        }

        // Render overlays for second keystone
        // PRODUCTION FIX: Only render keystone2 overlays when video2 has valid frame data
        // This prevents flickering during initial startup before first frame is decoded
        // NOTE: Use first_frame_decoded2 instead of y_data2 for HW decode (y_data2 is NULL in HW path)
        if (any_overlay_visible2 && first_frame_decoded2) {
            // PRODUCTION FIX: Only deselect keystone2 if keystone1 is the active one
            // This ensures only the ACTIVE keystone's corner is highlighted
            int saved_selected_corner = -1;
            if (app->active_keystone == 0 && app->keystone->selected_corner >= 0) {
                // Keystone 1 is active, temporarily hide keystone 2's selection
                saved_selected_corner = app->keystone2->selected_corner;
                app->keystone2->selected_corner = -1;
            }

            if (app->keystone2->show_corners) {
                gl_render_corners(app->gl, app->keystone2);
            }
            if (app->keystone2->show_border) {
                gl_render_border(app->gl, app->keystone2);
                gl_render_display_boundary(app->gl, app->keystone2);
            }
            if (app->keystone2->show_help) {
                gl_render_help_overlay(app->gl, app->keystone2);
            }

            // Restore the selection
            if (saved_selected_corner >= 0) {
                app->keystone2->selected_corner = saved_selected_corner;
            }

            while (glGetError() != GL_NO_ERROR) {
                // Clear error queue
            }
        }
        
        // Render notification overlay if active
        if (app->notification_active) {
            struct timespec current_ts;
            clock_gettime(CLOCK_MONOTONIC, &current_ts);
            double current_time = current_ts.tv_sec + current_ts.tv_nsec / 1e9;
            double elapsed = current_time - app->notification_start_time;

            if (elapsed < app->notification_duration) {
                // Notification is still active - render it with text
                gl_render_notification_overlay(app->gl, app->notification_message);
            } else {
                // Notification expired
                app->notification_active = false;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &overlay_end);

        // Time buffer swapping
        clock_gettime(CLOCK_MONOTONIC, &swap_start);

        // Swap buffers to display the rendered frame
        gl_swap_buffers(app->gl, app->drm);

        clock_gettime(CLOCK_MONOTONIC, &swap_end);

        // Capture render_end after swap completes
        clock_gettime(CLOCK_MONOTONIC, &render_end);

        // OPTIMIZATION: Pre-decode next frame DURING vsync wait (for KMS path)
        // For KMS overlay, the vsync wait happens in drm_display_video_frame (line 1104)
        // We can overlap the next frame's decode with the current frame's display
        if (!using_async_primary && first_frame_decoded && !next_frame_ready && frame_count > 0) {
            struct timespec predecode_start, predecode_end;
            clock_gettime(CLOCK_MONOTONIC, &predecode_start);

            int predecode_result = video_decode_frame(app->video);

            clock_gettime(CLOCK_MONOTONIC, &predecode_end);
            // Note: predecode timing not currently logged but available if needed

            if (predecode_result == 0) {
                next_frame_ready = true;
            } else if (video_is_eof(app->video)) {
                // EOF during pre-decode - will be handled in main decode section next iteration
                next_frame_ready = false;
            } else {
                next_frame_ready = false;
            }
        }
        
        // NOTE: Video 2 pre-decode removed - now handled by async decoder thread
        // The async decoder continuously decodes in background, no need for explicit pre-decode

        // Update render_time from actual local timestamps
        render_time = (render_end.tv_sec - render_start.tv_sec) +
                     (render_end.tv_nsec - render_start.tv_nsec) / 1e9;
        
        total_render_time += render_time;
        render_frame_count++;  // Increment frame counter for every rendered frame
        
        // FRAME DROP DETECTION: Track dropped frames for diagnostics
        static struct timespec last_frame_time = {0, 0};
        static int frame_drop_count = 0;
        static int frame_drop_reports = 0;

        struct timespec current_time_drop;
        clock_gettime(CLOCK_MONOTONIC, &current_time_drop);

        if (last_frame_time.tv_sec != 0) {
            double time_since_last = (current_time_drop.tv_sec - last_frame_time.tv_sec) +
                                   (current_time_drop.tv_nsec - last_frame_time.tv_nsec) / 1e9;

            // Detect frame drops using actual video FPS (not hardcoded 60fps)
            // If we see >1.5x expected frame time, we may have dropped a frame
            if (time_since_last > (target_frame_time * 1.5) && render_frame_count > 10) {
                frame_drop_count++;
                /* PRODUCTION: Only report first 5 drops, then summary every 100 frames */
                if (frame_drop_reports < 5) {
                    LOG_WARN("FRAME DROP", "Frame %d: %.1fms since last frame (expected ~%.1fms)",
                           render_frame_count, time_since_last * 1000, target_frame_time * 1000);
                    frame_drop_reports++;
                    if (frame_drop_reports == 5) {
                        LOG_INFO("FRAME DROP", "Further frame drops will be summarized periodically");
                    }
                } else if (render_frame_count % 100 == 0) {
                    // Periodic summary every 100 frames
                    LOG_INFO("FRAME DROP", "Summary: %d dropped frames in last 100 (total: %d)",
                           frame_drop_count - (frame_drop_reports - 5), frame_drop_count);
                }
            }
        }
        last_frame_time = current_time_drop;

        // FRAME DROP DETECTION FOR VIDEO 2: Track dropped frames independently
        if (app->video2) {
            static struct timespec last_frame_time2 = {0, 0};
            static int frame_drop_count2 = 0;
            static int frame_drop_reports2 = 0;

            struct timespec current_time_drop2;
            clock_gettime(CLOCK_MONOTONIC, &current_time_drop2);

            if (last_frame_time2.tv_sec != 0 && frame_count2 > 0) {
                double time_since_last2 = (current_time_drop2.tv_sec - last_frame_time2.tv_sec) +
                                         (current_time_drop2.tv_nsec - last_frame_time2.tv_nsec) / 1e9;

                // Detect frame drops for V2 using its own FPS (independent from V1)
                if (time_since_last2 > (target_frame_time2 * 1.5) && frame_count2 > 10) {
                    frame_drop_count2++;
                    /* PRODUCTION: Only report first 5 drops, then summary every 100 frames */
                    if (frame_drop_reports2 < 5) {
                        LOG_WARN("FRAME DROP V2", "Frame %d: %.1fms since last frame (expected ~%.1fms)",
                               frame_count2, time_since_last2 * 1000, target_frame_time2 * 1000);
                        frame_drop_reports2++;
                        if (frame_drop_reports2 == 5) {
                            LOG_INFO("FRAME DROP V2", "Further V2 frame drops will be summarized periodically");
                        }
                    } else if (frame_count2 % 100 == 0) {
                        // Periodic summary every 100 frames
                        LOG_INFO("FRAME DROP V2", "Summary: %d dropped frames in last 100 (total: %d)",
                               frame_drop_count2 - (frame_drop_reports2 - 5), frame_drop_count2);
                    }
                }
            }
            last_frame_time2 = current_time_drop2;
        }

        // Per-frame detailed timing breakdown
        double gl_render_time = (gl_render_end.tv_sec - gl_render_start.tv_sec) +
                                (gl_render_end.tv_nsec - gl_render_start.tv_nsec) / 1e9;
        double overlay_time = (overlay_end.tv_sec - overlay_start.tv_sec) +
                             (overlay_end.tv_nsec - overlay_start.tv_nsec) / 1e9;
        double swap_time = (swap_end.tv_sec - swap_start.tv_sec) +
                          (swap_end.tv_nsec - swap_start.tv_nsec) / 1e9;
        double upload_time = gl_render_time; // Upload happens during gl_render_frame calls
        double warp_draw_time = overlay_time; // Warp+draw is overlay rendering
        double total_stage_time = decode0_time + decode1_time + upload_time + warp_draw_time + swap_time;
        
        // Per-frame output (every 6 frames to reduce spam, ~10 fps @ 60fps capture)
        if (app->show_timing && frame_count > 0 && frame_count % 6 == 0) {
            LOG_DEBUG("PERF", "Frame %d: decode0=%.2fms decode1=%.2fms upload=%.2fms warp+draw=%.2fms swap=%.2fms total=%.2fms",
                   frame_count,
                   decode0_time * 1000,
                   decode1_time * 1000,
                   upload_time * 1000,
                   warp_draw_time * 1000,
                   swap_time * 1000,
                   total_stage_time * 1000);
             // Only log NV12 conversion time if using hardware decode
             if (app->video->use_hardware_decode || (app->video2 && app->video2->use_hardware_decode)) {
                 char nv12_ms[2][16];
                 char upload_ms[2][16];
                 format_metric_ms(nv12_frame_time[0], nv12_ms[0], sizeof(nv12_ms[0]));
                 format_metric_ms(nv12_frame_time[1], nv12_ms[1], sizeof(nv12_ms[1]));
                 format_metric_ms(upload_frame_time[0], upload_ms[0], sizeof(upload_ms[0]));
                 format_metric_ms(upload_frame_time[1], upload_ms[1], sizeof(upload_ms[1]));
                 LOG_DEBUG("PERF", "         nv12_cpu(ms)=%s/%s gl_upload(ms)=%s/%s",
                     nv12_ms[0], nv12_ms[1], upload_ms[0], upload_ms[1]);
             }
        }
        
        // Store timing samples for analysis (only if buffers allocated successfully)
        if (decode_times && render_times) {
            if (timing_samples < 300) {
                decode_times[timing_buffer_idx] = decode_time;
                render_times[timing_buffer_idx] = render_time;
                timing_buffer_idx = (timing_buffer_idx + 1) % 300;
                timing_samples++;
            } else {
                decode_times[timing_buffer_idx] = decode_time;
                render_times[timing_buffer_idx] = render_time;
                timing_buffer_idx = (timing_buffer_idx + 1) % 300;
            }
        }
        
        // Detailed frame timing analysis every 30 frames
        if (app->show_timing && diagnostic_frame_count % 30 == 0 && diagnostic_frame_count > 0 && decode_times && render_times) {
            static int last_reported_frame = -1;
            
            // Only print once per unique frame milestone
            if (diagnostic_frame_count != last_reported_frame) {
                last_reported_frame = diagnostic_frame_count;
                
                double avg_decode = 0, min_decode = 999, max_decode = 0;
                double avg_render = 0, min_render = 999, max_render = 0;
                int samples = timing_samples < 300 ? timing_samples : 300;
                
                for (int i = 0; i < samples; i++) {
                    avg_decode += decode_times[i];
                    avg_render += render_times[i];
                    if (decode_times[i] < min_decode) min_decode = decode_times[i];
                    if (decode_times[i] > max_decode) max_decode = decode_times[i];
                    if (render_times[i] < min_render) min_render = render_times[i];
                    if (render_times[i] > max_render) max_render = render_times[i];
                }
                
                avg_decode /= samples;
                avg_render /= samples;
                
                LOG_INFO("TIMING", "Analysis - V1 Frame %d%s", diagnostic_frame_count,
                       app->video2 ? "" : "");
                if (app->video2) {
                    LOG_INFO("TIMING", "           V2 Frame %d", frame_count2);
                }
                LOG_INFO("TIMING", "  DECODE:  Avg: %.3fms, Min: %.3fms, Max: %.3fms (samples: %d)",
                       avg_decode * 1000, min_decode * 1000, max_decode * 1000, samples);
                LOG_INFO("TIMING", "  RENDER:  Avg: %.3fms, Min: %.3fms, Max: %.3fms",
                       avg_render * 1000, min_render * 1000, max_render * 1000);
                LOG_INFO("TIMING", "  Target: V1=%.2fms%s", target_frame_time * 1000,
                       app->video2 ? "" : "");
                if (app->video2) {
                    LOG_INFO("TIMING", "          V2=%.2fms (FPS: %.1f)", target_frame_time2 * 1000, app->video2->fps);
                }
                LOG_INFO("TIMING", "  V1 HW decode: %s%s", 
                       video_is_hardware_decoded(app->video) ? "YES" : "NO",
                       app->video2 ? "" : "");
                if (app->video2) {
                    LOG_INFO("TIMING", "  V2 HW decode: %s", 
                           video_is_hardware_decoded(app->video2) ? "YES" : "NO");
                }
                LOG_INFO("TIMING", "  Total time: %.3fms (decode + render)", (avg_decode + avg_render) * 1000);
                LOG_DEBUG("TIMING", "  Note: Low decode times indicate async decode working");

                // Only report NV12 stats when hardware decode is enabled
                bool hw_v1 = video_is_hardware_decoded(app->video);
                bool hw_v2 = app->video2 && video_is_hardware_decoded(app->video2);

                // Always show GL Upload stats for both videos (useful for SW decode too)
                if (app->video2) {
                    for (int vid = 0; vid < 2; ++vid) {
                        if (gl_upload_interval_count[vid] == 0) {
                            gl_upload_interval_min[vid] = 0.0;
                        }
                    }

                    LOG_INFO("TIMING", "  GL Upload: V1 Avg: %.2fms (min: %.2fms, max: %.2fms, n=%d) | V2 Avg: %.2fms (min: %.2fms, max: %.2fms, n=%d)",
                           gl_upload_interval_count[0] ? (gl_upload_interval_sum[0] / gl_upload_interval_count[0]) * 1000.0 : 0.0,
                           gl_upload_interval_min[0] * 1000.0,
                           gl_upload_interval_max[0] * 1000.0,
                           gl_upload_interval_count[0],
                           gl_upload_interval_count[1] ? (gl_upload_interval_sum[1] / gl_upload_interval_count[1]) * 1000.0 : 0.0,
                           gl_upload_interval_min[1] * 1000.0,
                           gl_upload_interval_max[1] * 1000.0,
                           gl_upload_interval_count[1]);
                }

                // NV12 stats only for hardware decode
                if (hw_v1 || hw_v2) {
                    for (int vid = 0; vid < 2; ++vid) {
                        if (nv12_interval_count[vid] == 0) {
                            nv12_interval_min[vid] = 0.0;
                        }
                    }

                    LOG_INFO("TIMING", "  NV12 CPU:  V1 Avg: %.2fms (min: %.2fms, max: %.2fms, n=%d) | V2 Avg: %.2fms (min: %.2fms, max: %.2fms, n=%d)",
                           nv12_interval_count[0] ? (nv12_interval_sum[0] / nv12_interval_count[0]) * 1000.0 : 0.0,
                           nv12_interval_min[0] * 1000.0,
                           nv12_interval_max[0] * 1000.0,
                           nv12_interval_count[0],
                           nv12_interval_count[1] ? (nv12_interval_sum[1] / nv12_interval_count[1]) * 1000.0 : 0.0,
                           nv12_interval_min[1] * 1000.0,
                           nv12_interval_max[1] * 1000.0,
                           nv12_interval_count[1]);
                }
                
                if (avg_decode + avg_render > target_frame_time * 1.1) {
                    LOG_WARN("TIMING", "Frame taking %.0f%% of budget!", 
                           ((avg_decode + avg_render) / target_frame_time) * 100);
                }

                for (int vid = 0; vid < 2; ++vid) {
                    nv12_interval_sum[vid] = 0.0;
                    nv12_interval_min[vid] = 1e9;
                    nv12_interval_max[vid] = 0.0;
                    nv12_interval_count[vid] = 0;
                    gl_upload_interval_sum[vid] = 0.0;
                    gl_upload_interval_min[vid] = 1e9;
                    gl_upload_interval_max[vid] = 0.0;
                    gl_upload_interval_count[vid] = 0;
                }
            }
        }

        // Process keystone movement immediately after input update
        process_keystone_movement(app, delta_time, target_frame_time);

        // FIXED: For KMS overlay, drmModeSetPlane doesn't reliably wait for vsync on RPi
        // Must manually pace frames to avoid overwhelming the worker thread
        double total_frame_time = decode_time + render_time;
        double remaining_time = target_frame_time - total_frame_time;
        
        // PRODUCTION: PTS-based drift compensation to prevent timing skew over long playback
        // Check if current wall-clock time matches where video playback should be
        static double first_frame_wall_time = -1.0;
        static double first_frame_pts_time = -1.0;
        
        if (app->video && app->video->frame && app->video->frame->pts != AV_NOPTS_VALUE) {
            AVStream *stream = app->video->format_ctx->streams[app->video->video_stream_index];
            if (stream && stream->time_base.den > 0) {
                // Calculate video PTS in seconds
                double frame_pts_seconds = (double)app->video->frame->pts * stream->time_base.num / stream->time_base.den;
                
                // Initialize baseline on first frame
                if (first_frame_wall_time < 0) {
                    first_frame_wall_time = current_total_time;
                    first_frame_pts_time = frame_pts_seconds;
                }
                
                // Calculate how far into playback we should be
                double intended_wall_time = first_frame_wall_time + (frame_pts_seconds - first_frame_pts_time);
                double wall_drift = current_total_time - intended_wall_time;
                
                // If we're significantly ahead or behind, adjust sleep time
                // Max adjustment is +/- 20ms per frame to avoid jitter
                if (fabs(wall_drift) > 0.001) {  // More than 1ms drift
                    double drift_correction = wall_drift * 0.05;  // Smooth correction (5% per frame)
                    drift_correction = (drift_correction > 0.020) ? 0.020 : drift_correction;
                    drift_correction = (drift_correction < -0.020) ? -0.020 : drift_correction;
                    remaining_time -= drift_correction;
                    
                    if (app->advanced_diagnostics && fabs(wall_drift) > 0.050) {
                        LOG_DEBUG("TIMING", "Drift correction: %.1fms (total drift: %.1fms)",
                               drift_correction * 1000, wall_drift * 1000);
                    }
                }
            }
        }

        // OPTIMIZED: Absolute timing with clock_nanosleep to eliminate cumulative drift
        // Wake up 1.5ms early to allow vsync in eglSwapBuffers to handle final sync
        // This prevents nanosleep overshoot from causing frame drops
        static struct timespec next_frame_time = {0, 0};
        static bool timing_initialized = false;

        if (!timing_initialized) {
            clock_gettime(CLOCK_MONOTONIC, &next_frame_time);
            timing_initialized = true;
        }

        // Calculate when next frame should be displayed (absolute time)
        double target_seconds = target_frame_time;
        long target_nsec = (long)(target_seconds * 1000000000);
        next_frame_time.tv_nsec += target_nsec;
        // Handle nanosecond overflow
        if (next_frame_time.tv_nsec >= 1000000000) {
            next_frame_time.tv_sec += next_frame_time.tv_nsec / 1000000000;
            next_frame_time.tv_nsec %= 1000000000;
        }

        // Get current time
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // Calculate time until next frame
        double time_until_next = (next_frame_time.tv_sec - now.tv_sec) +
                                 (next_frame_time.tv_nsec - now.tv_nsec) / 1e9;

        // Sleep until 1.5ms before target, let vsync handle the rest
        // This reduces nanosleep jitter impact
        const double early_wakeup_margin = 0.0015;  // 1.5ms early wakeup

        if (time_until_next > early_wakeup_margin) {
            struct timespec wakeup_time = next_frame_time;
            long wakeup_ns_early = (long)(early_wakeup_margin * 1000000000);
            wakeup_time.tv_nsec -= wakeup_ns_early;
            if (wakeup_time.tv_nsec < 0) {
                wakeup_time.tv_sec -= 1;
                wakeup_time.tv_nsec += 1000000000;
            }

            // Use TIMER_ABSTIME for absolute timing (no cumulative drift)
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);
        }

        // If we're way behind (> 2 frames), reset timing to prevent infinite catchup
        if (time_until_next < -target_frame_time * 2.0) {
            clock_gettime(CLOCK_MONOTONIC, &next_frame_time);
            if (app->show_timing) {
                LOG_WARN("TIMING", "Timing reset - fell behind by %.1fms", -time_until_next * 1000);
            }
        }

        if (app->show_timing && total_frame_time > target_frame_time * 1.5) {
            LOG_WARN("TIMING", "Frame processing slow: %.1fms (target: %.1fms)",
                   total_frame_time * 1000, target_frame_time * 1000);
        }
    }  // End while (app->running)
    
    // Cleanup timing buffers
    if (decode_times) free(decode_times);
    if (render_times) free(render_times);
}

void app_cleanup(app_context_t *app) {
    if (!app) {
        return;
    }
    
    // Stop async decoders first before cleaning up videos
    if (app->async_decoder_primary) {
        async_decode_destroy(app->async_decoder_primary);
        app->async_decoder_primary = NULL;
    }
    if (app->async_decoder_secondary) {
        async_decode_destroy(app->async_decoder_secondary);
        app->async_decoder_secondary = NULL;
    }
    
    if (app->input) {
        input_cleanup(app->input);
        free(app->input);
        app->input = NULL;
    }
    if (app->keystone) {
        // Don't auto-save on exit - only save when user presses S key
        keystone_cleanup(app->keystone);
        free(app->keystone);
        app->keystone = NULL;
    }
    if (app->keystone2) {
        keystone_cleanup(app->keystone2);
        free(app->keystone2);
        app->keystone2 = NULL;
    }
    if (app->video) {
        video_cleanup(app->video);
        free(app->video);
        app->video = NULL;
    }
    if (app->video2) {
        video_cleanup(app->video2);
        free(app->video2);
        app->video2 = NULL;
    }
    if (app->gl) {
        gl_cleanup(app->gl);
        free(app->gl);
        app->gl = NULL;
    }
    if (app->drm) {
        // Clean up KMS video overlay before general DRM cleanup
        drm_hide_video_plane(app->drm);
        drm_cleanup(app->drm);
        free(app->drm);
        app->drm = NULL;
    }

    memset(app, 0, sizeof(*app));
    LOG_INFO("APP", "Application cleanup complete");
}
