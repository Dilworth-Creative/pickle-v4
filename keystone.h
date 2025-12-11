#ifndef KEYSTONE_H
#define KEYSTONE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x, y;
} point_t;

typedef struct {
    point_t corners[4];  // Top-left, top-right, bottom-right, bottom-left
    int selected_corner; // 0-3, -1 if none selected
    float matrix[16];    // 4x4 transformation matrix
    bool matrix_dirty;   // Need to recalculate matrix
    
    // Corner highlighting
    bool show_corners;   // Toggle corner visibility
    bool corners_dirty;  // Corner positions changed, need VBO update
    
    // Border highlighting
    bool show_border;    // Toggle border visibility
    
    // Help overlay
    bool show_help;      // Toggle help overlay visibility
    
    // Movement sensitivity
    float move_step;
    
    // Video aspect ratio for perspective-corrected mapping
    int video_width;     // Current video width (0 = not set)
    int video_height;    // Current video height (0 = not set)
    int display_width;   // Display width for aspect calculation
    int display_height;  // Display height for aspect calculation
} keystone_context_t;

// Keystone correction functions
int keystone_init(keystone_context_t *keystone);
void keystone_cleanup(keystone_context_t *keystone);
void keystone_select_corner(keystone_context_t *keystone, int corner);
void keystone_move_corner(keystone_context_t *keystone, float dx, float dy);
void keystone_calculate_matrix(keystone_context_t *keystone);
const float* keystone_get_matrix(keystone_context_t *keystone);
void keystone_reset_corners(keystone_context_t *keystone);
void keystone_set_inset_corners(keystone_context_t *keystone, float margin);
void keystone_toggle_corners(keystone_context_t *keystone);
bool keystone_corners_visible(keystone_context_t *keystone);
void keystone_toggle_border(keystone_context_t *keystone);
bool keystone_border_visible(keystone_context_t *keystone);
void keystone_toggle_help(keystone_context_t *keystone);
bool keystone_help_visible(keystone_context_t *keystone);
void keystone_increase_step_size(keystone_context_t *keystone);
void keystone_decrease_step_size(keystone_context_t *keystone);
float keystone_get_step_size(keystone_context_t *keystone);
int keystone_save_to_file(keystone_context_t *keystone, const char *filename);
int keystone_load_from_file(keystone_context_t *keystone, const char *filename);
int keystone_save_settings(keystone_context_t *keystone);
int keystone_load_settings(keystone_context_t *keystone);

// Video aspect ratio support - call when video dimensions change
// This recalculates the perspective matrix to map the aspect-corrected video
// directly to the keystone corners (instead of letterboxing/pillarboxing inside)
void keystone_set_video_dimensions(keystone_context_t *keystone, 
                                   int video_width, int video_height,
                                   int display_width, int display_height);
void keystone_clear_video_dimensions(keystone_context_t *keystone);
bool keystone_has_video_dimensions(keystone_context_t *keystone);

// Corner indices
#define CORNER_TOP_LEFT     0
#define CORNER_TOP_RIGHT    1
#define CORNER_BOTTOM_RIGHT 2
#define CORNER_BOTTOM_LEFT  3

#endif // KEYSTONE_H