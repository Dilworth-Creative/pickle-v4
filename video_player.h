#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Include the actual struct definitions so we can use them
#include "drm_display.h"
#include "gl_context.h"
#include "video_decoder.h"
#include "keystone.h"
#include "input_handler.h"

// Async decode context for video 2
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool running;
    bool should_exit;
    video_context_t *video;
    bool frame_ready;
    bool decoding;
} async_decode_t;

// Main application context
typedef struct {
    video_context_t *video;
    video_context_t *video2;     // Second video decoder
    async_decode_t *async_decoder_primary;   // Async decode thread for video 1 (software path)
    async_decode_t *async_decoder_secondary; // Async decode thread for video 2
    display_ctx_t *drm;
    gl_context_t *gl;
    input_context_t *input;
    keystone_context_t *keystone;
    keystone_context_t *keystone2; // Second keystone for second video
    bool running;
    bool loop_playback;
    const char *video_file;
    const char *video_file2;     // Second video file path
    bool needs_redraw;       // Flag to indicate frame needs redrawing
    bool show_timing;        // Flag to show frame timing information
    bool debug_gamepad;      // Flag to log gamepad button presses
    bool advanced_diagnostics; // Flag to enable detailed hardware decoder diagnostics
    int active_keystone;     // 0 for first keystone, 1 for second
    int gamepad_corner_cycle_index; // Track position in corner cycling (1-8 mode)

    // Notification message overlay
    char notification_message[256];  // Message to display
    double notification_start_time;  // When message was shown (monotonic time)
    double notification_duration;    // How long to show (seconds)
    bool notification_active;        // Whether message is currently showing
} app_context_t;

// Main application functions
int app_init(app_context_t *app, const char *video_file, const char *video_file2, bool loop_playback, bool show_timing, bool debug_gamepad, bool advanced_diagnostics, bool enable_hardware_decode, bool dual_hw_decode);
void app_run(app_context_t *app);
void app_cleanup(app_context_t *app);

// Async decode functions
async_decode_t* async_decode_create(video_context_t *video);
void async_decode_destroy(async_decode_t *decoder);
void async_decode_request_frame(async_decode_t *decoder);
bool async_decode_frame_ready(async_decode_t *decoder);
bool async_decode_wait_frame(async_decode_t *decoder, int timeout_ms);

#endif // VIDEO_PLAYER_H