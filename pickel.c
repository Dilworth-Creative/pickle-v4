#include "video_player.h"
#include "input_handler.h"
#include "version.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

// Global app context for signal handlers
static app_context_t *g_app = NULL;

// PRODUCTION: Global quit flag for async-signal-safe shutdown
// Set by signal handler, checked in main loop
volatile sig_atomic_t g_quit_requested = 0;

static void signal_handler(int sig) {
    // CRITICAL: Only async-signal-safe operations here!
    // Do NOT call printf, malloc, free, pthread functions, etc.
    (void)sig;  // Mark unused to avoid warning
    
    // PRODUCTION FIX: Set atomic flag only - main loop checks this
    g_quit_requested = 1;
    
    // Do NOT touch g_app or any other data structures here!
    // Let main loop handle graceful shutdown
}

// PRODUCTION FIX: Crash signal handler - minimal recovery, immediate exit
// For SIGSEGV/SIGBUS/SIGABRT, memory may be corrupted - don't attempt full cleanup
static void crash_signal_handler(int sig) {
    // Restore terminal first (critical for SSH sessions)
    input_restore_terminal_global();
    
    // Write directly to stderr (write() is async-signal-safe)
    const char *msg = "\nCRASH: ";
    (void)write(STDERR_FILENO, msg, 8);
    
    const char *signame;
    switch(sig) {
        case SIGSEGV: signame = "SIGSEGV\n"; break;
        case SIGBUS:  signame = "SIGBUS\n"; break;
        case SIGABRT: signame = "SIGABRT\n"; break;
        default:      signame = "UNKNOWN\n"; break;
    }
    (void)write(STDERR_FILENO, signame, strlen(signame));
    
    // Use _exit() - does NOT run atexit handlers (which could corrupt further)
    _exit(128 + sig);
}

static void cleanup_on_exit(void) {
    LOG_INFO("MAIN", "Restoring terminal state...");
    fflush(stdout);
    
    input_restore_terminal_global();
    
    if (g_app) {
        app_cleanup(g_app);
        g_app = NULL;
    }
    
    if (g_quit_requested) {
        LOG_INFO("MAIN", "Exiting after signal");
        fflush(stdout);
    }
}

static void setup_signal_handlers(void) {
    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination signal
    signal(SIGHUP, signal_handler);   // Hangup (SSH disconnect)
    
    // PRODUCTION FIX: Crash handlers - minimal recovery, no full cleanup
    // These indicate memory corruption, so atexit handlers could make things worse
    signal(SIGSEGV, crash_signal_handler);  // Segmentation fault
    signal(SIGBUS, crash_signal_handler);   // Bus error
    signal(SIGABRT, crash_signal_handler);  // Abort signal
    
    // Register atexit handler for safe cleanup (only runs on normal exit)
    atexit(cleanup_on_exit);
}

int main(int argc, char *argv[]) {
    // Initialize logging system first
    log_init();

    LOG_INFO("MAIN", "Starting %s", VERSION_FULL);
    
    bool loop_playback = false;
    bool show_timing = false;
    bool debug_gamepad = false;
    bool advanced_diagnostics = false;
    bool enable_hardware_decode = false;  // Changed: now defaults to software
    bool dual_hw_decode = false;  // Both videos use HW decode (experimental)
    char *video_file = NULL;
    char *video_file2 = NULL;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            loop_playback = true;
        } else if (strcmp(argv[i], "--timing") == 0) {
            show_timing = true;
        } else if (strcmp(argv[i], "--debug-gamepad") == 0) {
            debug_gamepad = true;
        } else if (strcmp(argv[i], "--hw-debug") == 0) {
            advanced_diagnostics = true;
            LOG_INFO("MAIN", "Advanced hardware decoder diagnostics enabled");
        } else if (strcmp(argv[i], "--hw") == 0) {
            enable_hardware_decode = true;
            LOG_INFO("MAIN", "Hardware decode enabled (--hw flag set)");
        } else if (strcmp(argv[i], "--dual-hw") == 0) {
            enable_hardware_decode = true;
            dual_hw_decode = true;
            LOG_INFO("MAIN", "Dual hardware decode enabled (both videos use V4L2 M2M)");
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            LOG_INFO("MAIN", "%s", VERSION_FULL);
            LOG_INFO("MAIN", "Semantic versioning: %d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
            LOG_INFO("MAIN", "\nFeatures:");
            LOG_INFO("MAIN", "  - Dual video playback with independent keystone correction");
            LOG_INFO("MAIN", "  - Hardware-accelerated decode (--hw flag)");
            LOG_INFO("MAIN", "  - DRM/KMS direct scanout with OpenGL ES 3.1");
            LOG_INFO("MAIN", "  - Gamepad and keyboard input support");
            LOG_INFO("MAIN", "  - Real-time performance profiling (--timing flag)");
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            LOG_INFO("MAIN", "Usage: %s [options] <video_file1.mp4> [video_file2.mp4]", argv[0]);
            LOG_INFO("MAIN", "\nOptions:");
            LOG_INFO("MAIN", "  -l               Loop video playback");
            LOG_INFO("MAIN", "  --timing         Show frame timing information");
            LOG_INFO("MAIN", "  --debug-gamepad  Log gamepad button presses");
            LOG_INFO("MAIN", "  --hw-debug       Enable detailed hardware decoder diagnostics");
            LOG_INFO("MAIN", "  --hw             Enable hardware decode (default: software)");
            LOG_INFO("MAIN", "  --dual-hw        Enable HW decode for both videos (experimental)");
            LOG_INFO("MAIN", "  -v, --version    Show version information");
            LOG_INFO("MAIN", "  -h, --help       Show this help message");
            LOG_INFO("MAIN", "\nKeyboard Controls:");
            LOG_INFO("MAIN", "  q/ESC    Quit");
            LOG_INFO("MAIN", "  h        Toggle help overlay");
            LOG_INFO("MAIN", "  1-4      Select keystone corners (video 1)");
            LOG_INFO("MAIN", "  5-8      Select keystone corners (video 2)");
            LOG_INFO("MAIN", "  arrows   Move selected corner");
            LOG_INFO("MAIN", "  r        Reset keystone");
            LOG_INFO("MAIN", "  s        Save keystone settings");
            LOG_INFO("MAIN", "  c        Toggle corner markers");
            LOG_INFO("MAIN", "  b        Toggle border outline");
            LOG_INFO("MAIN", "\nGamepad Controls (8BitDo Zero 2):");
            LOG_INFO("MAIN", "  X        Cycle through corners");
            LOG_INFO("MAIN", "  D-pad/Stick  Move selected corner");
            LOG_INFO("MAIN", "  L1/R1    Decrease/Increase step size");
            LOG_INFO("MAIN", "  SELECT   Reset keystone");
            LOG_INFO("MAIN", "  START    Toggle corner markers");
            LOG_INFO("MAIN", "  B        Toggle help overlay");
            LOG_INFO("MAIN", "  HOME     Toggle border outline");
            LOG_INFO("MAIN", "  START+SELECT (2s)  Quit");
            return 0;
        } else if (argv[i][0] != '-') {
            if (!video_file) {
                video_file = argv[i];
            } else if (!video_file2) {
                video_file2 = argv[i];
            } else {
                LOG_ERROR("MAIN", "Too many video files specified");
                return 1;
            }
        } else {
            LOG_ERROR("MAIN", "Unknown option: %s", argv[i]);
            LOG_ERROR("MAIN", "Use -h or --help for usage information");
            return 1;
        }
    }
    
    if (!video_file) {
        LOG_ERROR("MAIN", "No video file specified");
        LOG_ERROR("MAIN", "Usage: %s [options] <video_file1.mp4> [video_file2.mp4]", argv[0]);
        LOG_ERROR("MAIN", "Use -h or --help for more information");
        return 1;
    }

    LOG_INFO("MAIN", "Setting up application context");

    app_context_t app = {0};
    g_app = &app;  // Set global reference for signal handlers

    LOG_INFO("MAIN", "Setting up signal handlers");
    
    // Set up signal handlers for clean exit
    setup_signal_handlers();
    
    // Initialize and run the video player
    if (app_init(&app, video_file, video_file2, loop_playback, show_timing, debug_gamepad, advanced_diagnostics, enable_hardware_decode, dual_hw_decode) != 0) {
        LOG_ERROR("MAIN", "Failed to initialize application");
        g_app = NULL;  // Clear global reference
        return 1;
    }
    
    LOG_INFO("MAIN", "Starting main application loop");
    
    app_run(&app);
    app_cleanup(&app);
    
    g_app = NULL;  // Clear global reference
    
    return 0;
}