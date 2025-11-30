#include "input_handler.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <dirent.h>
#include <termios.h>
#include <linux/kd.h>       // For console mode constants

// Bit manipulation macros for input device capabilities
#define NBITS(x) ((((x)-1)/8)+1)
#define OFF(x)  ((x)%8)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/8)
#define test_bit(bit, array) ((array[LONG(bit)] >> OFF(bit)) & 1)

// Global terminal state for emergency restoration
// PRODUCTION: Consolidated to single global (was duplicated as g_orig_termios and g_original_term)
static struct termios g_original_term;
static bool g_terminal_modified = false;
static int g_stdin_fd = -1;

// Comprehensive terminal restoration function
static void restore_terminal_state(void) {
	if (g_terminal_modified) {
		// Try to restore text console mode
		int console_fd = open("/dev/tty", O_RDWR);
		if (console_fd >= 0) {
			// Switch back to text mode
			ioctl(console_fd, KDSETMODE, KD_TEXT);
			close(console_fd);
		}
		
		// Restore original terminal attributes with immediate effect
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_original_term);
		
		// Additional terminal reset commands to ensure proper state
		printf("\033[?25h");    // Show cursor
		printf("\033[0m");      // Reset all attributes  
		// DON'T reset terminal - preserve output: printf("\033c");
		printf("\r\n");         // Carriage return + newline for clean prompt
		
		// Force output and ensure line discipline is restored
		fflush(stdout);
		fflush(stderr);
		
		// Extra safety: explicitly restore canonical and echo modes
		struct termios current;
		if (tcgetattr(STDIN_FILENO, &current) == 0) {
			current.c_lflag |= (ECHO | ICANON);  // Ensure echo and canonical mode
			current.c_cc[VMIN] = 1;              // Restore blocking read
			current.c_cc[VTIME] = 0;             // No timeout
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &current);
		}
		
		g_terminal_modified = false;
	}
}

static const char *input_device_paths[] = {
    "/dev/input/event0",
    "/dev/input/event1",
    "/dev/input/event2",
    "/dev/input/event3",
    "/dev/input/event4",
    NULL
};

void input_clear_keys(input_context_t *input) {
    // Clear all key states - useful for terminal mode
    memset(input->keys_pressed, false, sizeof(input->keys_pressed));
    input->toggle_corners = false;
    input->toggle_border = false;
    input->toggle_help = false;
    input->save_keystone = false;
}

static int find_keyboard_device(void) {
    // Try to find a keyboard device by checking input devices
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        LOG_ERROR("INPUT", "Failed to open /dev/input directory");
        return -1;
    }
    
    struct dirent *entry;
    char device_path[512];  // Increased buffer size to avoid truncation warnings
    int fd = -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) == 0) {
            // Safe string formatting with bounds checking
            int ret = snprintf(device_path, sizeof(device_path), "/dev/input/%s", entry->d_name);
            if (ret >= (int)sizeof(device_path)) {
                continue; // Skip if path would be truncated
            }
            
            fd = open(device_path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                // Test if this device can generate keyboard events
                char name[256] = "Unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                
                // Look for keyboard-related keywords in device name
                if (strstr(name, "keyboard") || strstr(name, "Keyboard") ||
                    strstr(name, "USB") || strstr(name, "AT")) {
                    LOG_INFO("INPUT", "Found keyboard device: %s (%s)", device_path, name);
                    break;
                }
                
                close(fd);
                fd = -1;
            }
        }
    }
    
    closedir(dir);
    
    // If we couldn't find a keyboard device, try the first available event device
    if (fd < 0) {
        for (int i = 0; input_device_paths[i]; i++) {
            fd = open(input_device_paths[i], O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                // Using input device
                break;
            }
        }
    }
    
    return fd;
}

static int find_gamepad_device(void) {
    // Try to open joystick device (simpler API than evdev)
    const char *device = "/dev/input/js0";
    int fd = open(device, O_RDONLY | O_NONBLOCK);
    
    if (fd < 0) {
        // No joystick found
        return -1;
    }
    
    // Get joystick name
    char name[256] = "Unknown";
    ioctl(fd, JSIOCGNAME(sizeof(name)), name);
    
    LOG_INFO("INPUT", "Found gamepad device: %s (%s)", device, name);
    return fd;
}

static int setup_terminal_input(input_context_t *input) {
    input->stdin_fd = STDIN_FILENO;
    
    // Check if stdin is a terminal (tty)
    if (!isatty(input->stdin_fd)) {
        LOG_INFO("INPUT", "Input is not a terminal, using simplified input mode");
        // For non-tty input (pipes, redirects), just use stdin directly
        return 0;
    }
    
    // Get current terminal attributes
    if (tcgetattr(input->stdin_fd, &input->orig_termios) != 0) {
        return -1;
    }
    
    // Save global copy for emergency restoration
    g_original_term = input->orig_termios;
    g_stdin_fd = input->stdin_fd;
    g_terminal_modified = true;
    
    // Set up non-canonical, non-echo mode for immediate input
    struct termios raw = input->orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);  // Disable echo and canonical mode
    raw.c_cc[VMIN] = 0;               // Non-blocking read
    raw.c_cc[VTIME] = 0;              // No timeout
    
    if (tcsetattr(input->stdin_fd, TCSAFLUSH, &raw) != 0) {
        return -1;
    }
    
    // Mark that we modified the terminal
    g_terminal_modified = true;
    
    LOG_INFO("INPUT", "Using terminal input mode (press keys directly)");
    LOG_INFO("INPUT", "Controls: 1-4=select corner, arrows=move, s=save, r=reset, c=corners, b=border, h=help, q=quit");
    fflush(stdout);
    return 0;
}

// Global terminal restoration function for emergencies
void input_restore_terminal_global(void) {
    restore_terminal_state();
    
    // Additional safety measures for terminal restoration
    LOG_INFO("INPUT", "Terminal restored");
    fflush(stdout);
    
    // PRODUCTION FIX: Use direct termios API instead of system() for security
    // system() is a security risk if running with elevated privileges
    // Just restore the original terminal settings we saved at startup
    if (g_terminal_modified) {
        struct termios sane_term = g_original_term;
        // Ensure canonical and echo modes are re-enabled
        sane_term.c_lflag |= (ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN);
        sane_term.c_iflag |= (ICRNL | IXON);
        sane_term.c_oflag |= (OPOST | ONLCR);
        sane_term.c_cc[VMIN] = 1;
        sane_term.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &sane_term);
    }
}

static void restore_terminal(input_context_t *input) {
    if (input->use_stdin_fallback && input->stdin_fd >= 0) {
        // Use the comprehensive restore function
        restore_terminal_state();
    }
}

int input_init(input_context_t *input) {
    memset(input, 0, sizeof(*input));
    
    // Initialize gamepad polling timer
    struct timeval tv;
    gettimeofday(&tv, NULL);
    input->last_gamepad_poll_time = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    
    // Check if we're in SSH or other terminal-only environment
    bool prefer_terminal = (getenv("SSH_CLIENT") != NULL) || 
                          (getenv("SSH_CONNECTION") != NULL) ||
                          (!isatty(STDIN_FILENO));
    
    if (prefer_terminal) {
        LOG_INFO("INPUT", "Terminal environment detected, using terminal input...");
        if (setup_terminal_input(input) == 0) {
            input->use_stdin_fallback = true;
            input->keyboard_fd = -1;
        } else {
            LOG_ERROR("INPUT", "Failed to set up terminal input");
            return -1;
        }
    } else {
        input->keyboard_fd = find_keyboard_device();
        if (input->keyboard_fd < 0) {
            LOG_INFO("INPUT", "Event device input not available, trying terminal input...");
            
            // Try terminal input as fallback
            if (setup_terminal_input(input) == 0) {
                input->use_stdin_fallback = true;
                input->keyboard_fd = -1; // Mark event device as unavailable
            } else {
                LOG_ERROR("INPUT", "Failed to set up any input method");
                return -1;
            }
        } else {
            LOG_INFO("INPUT", "Using event device input");
            input->use_stdin_fallback = false;
        }
    }
    
    input->should_quit = false;
    
    // Try to initialize gamepad (optional, doesn't fail if not found)
    input->gamepad_fd = find_gamepad_device();
    if (input->gamepad_fd >= 0) {
        input->gamepad_enabled = true;
        LOG_INFO("INPUT", "Gamepad input enabled");
    } else {
        input->gamepad_enabled = false;
        input->gamepad_fd = -1;
        LOG_INFO("INPUT", "No gamepad detected (keyboard/terminal input only)");
    }
    
    // Input handler initialization complete
    return 0;
}

// Attempt to connect to gamepad (used for periodic polling)
static bool try_connect_gamepad(input_context_t *input) {
    if (input->gamepad_fd >= 0) {
        return true; // Already connected
    }
    
    input->gamepad_fd = find_gamepad_device();
    if (input->gamepad_fd >= 0) {
        input->gamepad_enabled = true;
        LOG_INFO("INPUT", "Gamepad connected!");
        return true;
    }
    return false;
}

void input_cleanup(input_context_t *input) {
    if (input->keyboard_fd >= 0) {
        close(input->keyboard_fd);
    }
    
    if (input->gamepad_fd >= 0) {
        close(input->gamepad_fd);
    }
    
    // Restore terminal if we were using stdin fallback
    restore_terminal(input);
    
    memset(input, 0, sizeof(*input));
}

void input_update(input_context_t *input) {
    static bool prev_keys[256] = {false};
    
    // Periodically poll for gamepad connection (every 3 seconds)
    if (!input->gamepad_enabled) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t current_time = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        
        if (current_time - input->last_gamepad_poll_time > 3000) {
            input->last_gamepad_poll_time = current_time;
            try_connect_gamepad(input);
        }
    }
    
    if (input->use_stdin_fallback) {
        // Terminal mode has no key-up events; treat directional keys as one-shot
        input->keys_pressed[KEY_UP] = false;
        input->keys_pressed[KEY_DOWN] = false;
        input->keys_pressed[KEY_LEFT] = false;
        input->keys_pressed[KEY_RIGHT] = false;
    }

    // Store previous key states for edge detection
    memcpy(prev_keys, input->keys_pressed, sizeof(prev_keys));
    
    // Clear one-shot keys at start of each frame
    memset(input->keys_just_pressed, false, sizeof(input->keys_just_pressed));
    
    if (input->use_stdin_fallback) {
        // Handle terminal input
        char ch;
        int bytes_read = read(input->stdin_fd, &ch, 1);
        while (bytes_read == 1) {
            // Convert ASCII characters to our internal key codes
            switch (ch) {
                case 'q':
                case 'Q':
                    input->should_quit = true;
                    LOG_DEBUG("INPUT", "Quit requested");
                    break;
                case 27: // ESC - check for arrow key sequences
                    {
                        char seq[3];
                        // Try to read the escape sequence
                        if (read(input->stdin_fd, &seq[0], 1) == 1 && seq[0] == '[') {
                            if (read(input->stdin_fd, &seq[1], 1) == 1) {
                                switch (seq[1]) {
                                    case 'A': // Up arrow
                                        input->keys_just_pressed[KEY_UP] = true;
                                        input->keys_pressed[KEY_UP] = true;
                                        // Hold functionality removed
                                        break;
                                    case 'B': // Down arrow
                                        input->keys_just_pressed[KEY_DOWN] = true;
                                        input->keys_pressed[KEY_DOWN] = true;
                                        // Hold functionality removed
                                        break;
                                    case 'C': // Right arrow
                                        input->keys_just_pressed[KEY_RIGHT] = true;
                                        input->keys_pressed[KEY_RIGHT] = true;
                                        // Hold functionality removed
                                        break;
                                    case 'D': // Left arrow
                                        input->keys_just_pressed[KEY_LEFT] = true;
                                        input->keys_pressed[KEY_LEFT] = true;
                                        // Hold functionality removed
                                        break;
                                    default:
                                        // Unknown escape sequence, treat as quit
                                        input->should_quit = true;
                                        LOG_DEBUG("INPUT", "Quit requested (ESC)");
                                        break;
                                }
                            } else {
                                // Just ESC by itself
                                input->should_quit = true;
                                LOG_DEBUG("INPUT", "Quit requested (ESC)");
                            }
                        } else {
                            // Just ESC by itself
                            input->should_quit = true;
                            LOG_DEBUG("INPUT", "Quit requested (ESC)");
                        }
                    }
                    break;
                case '1':
                    input->keys_just_pressed[KEY_1] = true;
                    // printf("Key 1 pressed (select corner 1)\n");
                    break;
                case '2':
                    input->keys_just_pressed[KEY_2] = true;
                    // printf("Key 2 pressed (select corner 2)\n");
                    break;
                case '3':
                    input->keys_just_pressed[KEY_3] = true;
                    // printf("Key 3 pressed (select corner 3)\n");
                    break;
                case '4':
                    input->keys_just_pressed[KEY_4] = true;
                    // printf("Key 4 pressed (select corner 4)\n");
                    break;
                case '5':
                    input->keys_just_pressed[KEY_5] = true;
                    // printf("Key 5 pressed (select corner 5)\n");
                    break;
                case '6':
                    input->keys_just_pressed[KEY_6] = true;
                    // printf("Key 6 pressed (select corner 6)\n");
                    break;
                case '7':
                    input->keys_just_pressed[KEY_7] = true;
                    // printf("Key 7 pressed (select corner 7)\n");
                    break;
                case '8':
                    input->keys_just_pressed[KEY_8] = true;
                    // printf("Key 8 pressed (select corner 8)\n");
                    break;
                case 's':
                case 'S':
                    input->save_keystone = true;
                    // printf("S key pressed (save keystone)\n");
                    break;
                case 'c':
                case 'C':
                    input->toggle_corners = true;
                    // printf("Toggle corner highlights\n");
                    break;
                case 'b':
                case 'B':
                    input->toggle_border = true;
                    // printf("Toggle border highlights\n");
                    break;
                case 'h':
                case 'H':
                    input->toggle_help = true;
                    // printf("Toggle help overlay\n");
                    break;

                case 'r':
                case 'R':
                    input->keys_just_pressed[KEY_R] = true;
                    // printf("R key pressed (reset keystone)\n");
                    break;
                case 'p':
                case 'P':
                    input->save_keystone = true;
                    // printf("P key pressed (save keystone)\n");
                    break;
            }
            // Read next character
            bytes_read = read(input->stdin_fd, &ch, 1);
        }
        
    } else {
        // Handle event device input (original method)
        struct input_event ev;
        while (read(input->keyboard_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY) {
                if (ev.code < 256) {
                    // Update key state
                    input->keys_pressed[ev.code] = (ev.value != 0);
                    
                    // Handle specific keys
                    if (ev.value == 1) // Key press
                        switch (ev.code) {
                            case KEY_Q:
                            case KEY_ESC:
                                input->should_quit = true;
                                printf("Quit requested\n");
                                break;
                            case KEY_1:
                                // printf("Key 1 pressed (select corner 1)\n");
                                break;
                            case KEY_2:
                                // printf("Key 2 pressed (select corner 2)\n");
                                break;
                            case KEY_3:
                                // printf("Key 3 pressed (select corner 3)\n");
                                break;
                            case KEY_4:
                                // printf("Key 4 pressed (select corner 4)\n");
                                break;
                            case KEY_C:
                                input->toggle_corners = true;
                                // printf("Toggle corner highlights\n");
                                break;
                            case KEY_UP:
                                // printf("Up arrow pressed\n");
                                break;
                            case KEY_DOWN:
                                // printf("Down arrow pressed\n");
                                break;
                            case KEY_LEFT:
                                // printf("Left arrow pressed\n");
                                break;
                            case KEY_RIGHT:
                                // printf("Right arrow pressed\n");
                                break;
                            case KEY_R:
                                // printf("R pressed (reset keystone)\n");
                                break;
                        }
                }
            }
        }
    }
    
    // Process gamepad input (joystick API - can run alongside keyboard)
    if (input->gamepad_enabled && input->gamepad_fd >= 0) {
        struct js_event js;
        
        if (input->debug_gamepad) {
            static int debug_counter = 0;
            if ((debug_counter++ % 300) == 0) {  // Every ~10 seconds at 30fps
                printf("[GAMEPAD] Gamepad processing active (fd=%d, enabled=%d)\n", 
                       input->gamepad_fd, input->gamepad_enabled);
            }
        }
        
        // Clear one-shot gamepad button presses
        memset(input->gamepad_buttons_just_pressed, false, sizeof(input->gamepad_buttons_just_pressed));
        input->gamepad_cycle_corner = false;
        input->gamepad_decrease_step = false;
        input->gamepad_increase_step = false;
        input->gamepad_reset_keystone = false;
        input->gamepad_toggle_mode = false;
        
        // Read all pending joystick events
        int event_count = 0;
        ssize_t read_result;
        while ((read_result = read(input->gamepad_fd, &js, sizeof(js))) == sizeof(js)) {
            event_count++;
            if (input->debug_gamepad) {
                printf("[GAMEPAD] Event %d: type=%d number=%d value=%d\n", 
                       event_count, js.type, js.number, js.value);
            }
            
            // Skip initial state events sent when joystick is first opened
            if (js.type & JS_EVENT_INIT) {
                if (input->debug_gamepad) {
                    printf("[GAMEPAD] Skipping INIT event\n");
                }
                continue;
            }
            
            if (js.type == JS_EVENT_BUTTON) {
                // Button event
                if (js.number < 32) {
                    bool was_pressed = input->gamepad_buttons[js.number];
                    input->gamepad_buttons[js.number] = (js.value != 0);
                    
                    // Detect button press (rising edge)
                    if (!was_pressed && input->gamepad_buttons[js.number]) {
                        input->gamepad_buttons_just_pressed[js.number] = true;
                        
                        // Debug logging with CORRECTED button names
                        if (input->debug_gamepad) {
                            const char *btn_names[] = {
                                "A",       // 0
                                "B",       // 1
                                "?",       // 2
                                "X",       // 3
                                "Y",       // 4
                                "?",       // 5
                                "L1",      // 6
                                "R1",      // 7
                                "HOME",    // 8
                                "?",       // 9
                                "SELECT",  // 10
                                "START"    // 11
                            };
                            const char *btn_name = (js.number < 12) ? btn_names[js.number] : "UNKNOWN";
                            printf("[GAMEPAD] Button pressed: %s (button %d)\n", btn_name, js.number);
                        }
                        
                        // Map specific buttons to actions
                        // 8BitDo Zero 2 actual button layout: B=0, A=1, L1=2, X=3, Y=4, R1=5
                        if (js.number == JS_BUTTON_X) {  // Button 3 = TOP button (X)
                            input->gamepad_cycle_corner = true;
                        } else if (js.number == JS_BUTTON_B) {  // Button 0 = BOTTOM button (B)
                            input->gamepad_toggle_corner_border = true;
                        } else if (js.number == JS_BUTTON_Y) {  // Button 4 = LEFT button (Y)
                            input->toggle_help = true;
                        } else if (js.number == JS_BUTTON_L1) {
                            input->gamepad_increase_step = true;
                            if (input->debug_gamepad) printf("[GAMEPAD] L1 pressed - increase step\n");
                        } else if (js.number == JS_BUTTON_R1) {
                            input->gamepad_decrease_step = true;
                            if (input->debug_gamepad) printf("[GAMEPAD] R1 pressed - decrease step\n");
                        } else if (js.number == JS_BUTTON_SELECT) {
                            input->gamepad_reset_keystone = true;
                        } else if (js.number == JS_BUTTON_START) {
                            input->save_keystone = true;
                        } else if (js.number == JS_BUTTON_HOME) {
                            input->gamepad_toggle_mode = true;
                        }
                    }
                }
            } else if (js.type == JS_EVENT_AXIS) {
                // Axis event
                if (js.number == JS_AXIS_LEFT_X) {
                    int16_t old_value = input->gamepad_axis_x;
                    input->gamepad_axis_x = js.value;
                    if (input->debug_gamepad && abs(old_value - js.value) > 1000) {
                        printf("[GAMEPAD] Left stick X: %d\n", js.value);
                    }
                } else if (js.number == JS_AXIS_LEFT_Y) {
                    int16_t old_value = input->gamepad_axis_y;
                    input->gamepad_axis_y = js.value;
                    if (input->debug_gamepad && abs(old_value - js.value) > 1000) {
                        printf("[GAMEPAD] Left stick Y: %d\n", js.value);
                    }
                } else if (js.number == JS_AXIS_DPAD_X) {
                    // D-pad X: convert from -32767/0/32767 to -1/0/1
                    int8_t dpad_x = (js.value < -16000) ? -1 : (js.value > 16000) ? 1 : 0;
                    if (input->debug_gamepad && dpad_x != input->gamepad_dpad_x) {
                        printf("[GAMEPAD] D-pad X: %d\n", dpad_x);
                    }
                    input->gamepad_dpad_x = dpad_x;
                } else if (js.number == JS_AXIS_DPAD_Y) {
                    // D-pad Y: convert from -32767/0/32767 to -1/0/1
                    int8_t dpad_y = (js.value < -16000) ? -1 : (js.value > 16000) ? 1 : 0;
                    if (input->debug_gamepad && dpad_y != input->gamepad_dpad_y) {
                        printf("[GAMEPAD] D-pad Y: %d\n", dpad_y);
                    }
                    input->gamepad_dpad_y = dpad_y;
                }
            }
        }
        
        // Check if gamepad disconnected (read returned error other than EAGAIN)
        if (read_result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("Gamepad disconnected (error: %s), will retry connection...\n", strerror(errno));
            close(input->gamepad_fd);
            input->gamepad_fd = -1;
            input->gamepad_enabled = false;
            // Reset poll timer to try reconnecting soon
            struct timeval tv;
            gettimeofday(&tv, NULL);
            input->last_gamepad_poll_time = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        }
        
        // Check for START+SELECT held for 2 seconds (quit combo)
        if (input->gamepad_buttons[JS_BUTTON_START] && input->gamepad_buttons[JS_BUTTON_SELECT]) {
            if (input->gamepad_start_select_time == 0) {
                // Start timing
                struct timeval tv;
                gettimeofday(&tv, NULL);
                input->gamepad_start_select_time = tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
            } else {
                // Check if held for 2 seconds
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t now = tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
                if (now - input->gamepad_start_select_time >= 2000) {
                    input->should_quit = true;
                    printf("Quit requested (START+SELECT held)\n");
                }
            }
        } else {
            input->gamepad_start_select_time = 0;
        }
    }

}

bool input_is_key_pressed(input_context_t *input, int key) {
    if (key >= 0 && key < 256) {
        return input->keys_pressed[key];
    }
    return false;
}

bool input_is_key_just_pressed(input_context_t *input, int key) {
    // In terminal mode, use the one-shot array
    if (input->use_stdin_fallback) {
        if (key >= 0 && key < 256) {
            bool was_pressed = input->keys_just_pressed[key];
            if (was_pressed) {
                input->keys_just_pressed[key] = false; // Clear after reading
            }
            return was_pressed;
        }
        return false;
    }
    
    // For hardware input, use edge detection
    static bool prev_keys[256] = {false};
    static bool first_call = true;
    
    if (first_call) {
        memset(prev_keys, false, sizeof(prev_keys));
        first_call = false;
    }
    
    bool current_state = input_is_key_pressed(input, key);
    bool was_just_pressed = current_state && !prev_keys[key];
    
    prev_keys[key] = current_state;
    return was_just_pressed;
}

bool input_should_quit(input_context_t *input) {
    return input->should_quit;
}