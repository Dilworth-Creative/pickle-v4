#include "keystone.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Minimum area thresholds to prevent degenerate perspective transformations
// Keystone corners define a quad in normalized coordinates (-1 to 1, area = 4)
// MOVE: 0.001 = 0.025% of full quad - allows dramatic shrinking during user adjustment
// MATRIX: 0.0005 = 0.0125% of full quad - stricter guard for matrix solver stability
#define KEYSTONE_MIN_AREA_MOVE   0.001f
#define KEYSTONE_MIN_AREA_MATRIX 0.0005f

// Matrix math utilities for perspective transformation
static void matrix_identity(float *matrix) {
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

// Improved Gaussian elimination to solve 8x8 system for perspective transformation
static int solve_linear_system(float A[8][9]) {
    // Forward elimination with partial pivoting for better numerical stability
    for (int i = 0; i < 8; i++) {
        // Find pivot (maximum absolute value in column i)
        int max_row = i;
        float max_val = fabs(A[i][i]);
        
        for (int k = i + 1; k < 8; k++) {
            if (fabs(A[k][i]) > max_val) {
                max_row = k;
                max_val = fabs(A[k][i]);
            }
        }
        
        // Check for numerically singular matrix
        if (max_val < 1e-6) {
            // printf("Warning: Near-singular matrix encountered in keystone calculation\n");
            return -1; // Singular matrix
        }
        
        // Swap rows if needed for numerical stability
        if (max_row != i) {
            for (int j = 0; j < 9; j++) {
                float temp = A[i][j];
                A[i][j] = A[max_row][j];
                A[max_row][j] = temp;
            }
        }
        
        // Scale the pivot row for better numerical stability
        float pivot = A[i][i];
        for (int j = i; j < 9; j++) {
            A[i][j] /= pivot;
        }
        
        // Eliminate
        for (int k = 0; k < 8; k++) {
            if (k != i) {
                float factor = A[k][i];
                for (int j = i; j < 9; j++) {
                    A[k][j] -= factor * A[i][j];
                }
            }
        }
    }
    
    // At this point, A should be the identity matrix with solutions in column 8
    // Verify solution quality
    for (int i = 0; i < 8; i++) {
        if (fabs(A[i][i] - 1.0f) > 1e-5 || isnan(A[i][8]) || isinf(A[i][8])) {
            // printf("Warning: Potentially unstable keystone solution\n");
            return -1; // Unstable solution
        }
        
        // Limit extreme values in the solution to avoid unstable projections
        if (fabs(A[i][8]) > 10.0f) {
            // printf("Warning: Clamping extreme value in keystone matrix\n");  
            A[i][8] = (A[i][8] > 0) ? 10.0f : -10.0f;
        }
    }
    
    return 0;
}

// Calculate perspective transformation matrix using a more stable algorithm
static void calculate_perspective_matrix(float *matrix, const point_t corners[4]) {
    // CORRECTED: For rendering, we need to transform FROM standard quad TO keystone trapezoid
    // Source points: standard video quad (-1 to 1)
    float src_x[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    float src_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    
    // Destination points: keystone corners (where the video should appear)
    // Corner order: top-left, top-right, bottom-right, bottom-left
    float dst_x[4] = {corners[0].x, corners[1].x, corners[2].x, corners[3].x};
    float dst_y[4] = {corners[0].y, corners[1].y, corners[2].y, corners[3].y};
    
    // Check for default configuration (identity transformation)
    bool is_identity = true;
    for (int i = 0; i < 4; i++) {
        if (fabs(src_x[i] - dst_x[i]) > 1e-6 || fabs(src_y[i] - dst_y[i]) > 1e-6) {
            is_identity = false;
            break;
        }
    }
    
    if (is_identity) {
        matrix_identity(matrix);
        return;
    }
    
    // Check for degenerate configurations (to prevent erratic behavior)
    // Calculate the area of the destination quadrilateral (keystone shape) using the shoelace formula
    float area = 0.0f;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        area += dst_x[i] * dst_y[j] - dst_x[j] * dst_y[i];
    }
    area = 0.5f * fabs(area);
    
    // If area is too small, use identity matrix to prevent instability
    if (area < KEYSTONE_MIN_AREA_MATRIX) {
        // printf("Warning: Near-degenerate keystone configuration detected (area: %f). Using identity.\n", area);
        matrix_identity(matrix);
        return;
    }
    
    // Set up the system of equations for perspective transformation
    // We need to solve for 8 parameters (a-h) in the transformation:
    // x' = (ax + by + c) / (gx + hy + 1)
    // y' = (dx + ey + f) / (gx + hy + 1)
    float A[8][9]; // Augmented matrix
    
    for (int i = 0; i < 4; i++) {
        float x = src_x[i];
        float y = src_y[i];
        float x_prime = dst_x[i];
        float y_prime = dst_y[i];
        
        // Equation for x': ax + by + c - gx*x' - hy*x' = x'
        A[i*2][0] = x;       // a
        A[i*2][1] = y;       // b
        A[i*2][2] = 1.0f;    // c
        A[i*2][3] = 0.0f;    // d
        A[i*2][4] = 0.0f;    // e
        A[i*2][5] = 0.0f;    // f
        A[i*2][6] = -x * x_prime; // g
        A[i*2][7] = -y * x_prime; // h
        A[i*2][8] = x_prime; // result
        
        // Equation for y': dx + ey + f - gx*y' - hy*y' = y'
        A[i*2+1][0] = 0.0f;  // a
        A[i*2+1][1] = 0.0f;  // b
        A[i*2+1][2] = 0.0f;  // c
        A[i*2+1][3] = x;     // d
        A[i*2+1][4] = y;     // e
        A[i*2+1][5] = 1.0f;  // f
        A[i*2+1][6] = -x * y_prime; // g
        A[i*2+1][7] = -y * y_prime; // h
        A[i*2+1][8] = y_prime; // result
    }
    
    // Solve the system
    if (solve_linear_system(A) != 0) {
        // Fallback to identity matrix if system is singular
        matrix_identity(matrix);
        return;
    }
    
    // Extract parameters
    float a = A[0][8], b = A[1][8], c = A[2][8];
    float d = A[3][8], e = A[4][8], f = A[5][8];
    float g = A[6][8], h = A[7][8];
    
    // Build the 4x4 perspective transformation matrix
    matrix_identity(matrix);
    
    // The perspective transformation matrix for OpenGL (column-major):
    // [ a  d  0  g ]
    // [ b  e  0  h ]
    // [ 0  0  1  0 ]
    // [ c  f  0  1 ]
    
    matrix[0] = a;   matrix[1] = d;   matrix[2] = 0.0f; matrix[3] = g;
    matrix[4] = b;   matrix[5] = e;   matrix[6] = 0.0f; matrix[7] = h;
    matrix[8] = 0.0f; matrix[9] = 0.0f; matrix[10] = 1.0f; matrix[11] = 0.0f;
    matrix[12] = c;   matrix[13] = f;   matrix[14] = 0.0f; matrix[15] = 1.0f;
}

int keystone_init(keystone_context_t *keystone) {
    memset(keystone, 0, sizeof(*keystone));
    
    // Initialize default corners (no keystone correction)
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f,  1.0f};  // Corner 4 (visual)
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f,  1.0f};  // Corner 3 (visual)
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f, -1.0f};  // Corner 2 (visual) 
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f, -1.0f};  // Corner 1 (visual)
    
    keystone->selected_corner = CORNER_TOP_LEFT; // Start with top-left corner selected
    keystone->move_step = 0.0005f; // Default movement per key press (0.05% - ultra-fine control)
    keystone->matrix_dirty = true;
    keystone->corners_dirty = true;  // Initial corners need VBO upload
    keystone->show_corners = true;  // Show corners by default
    keystone->show_border = true;   // Show border by default
    keystone->show_help = true;     // Show help by default
    
    // Initialize identity matrix
    matrix_identity(keystone->matrix);
    
    // Keystone correction initialization complete
    return 0;
}

void keystone_cleanup(keystone_context_t *keystone) {
    // Nothing to cleanup for this simple implementation
    memset(keystone, 0, sizeof(*keystone));
}

void keystone_select_corner(keystone_context_t *keystone, int corner) {
    if (corner >= 0 && corner < 4) {
        keystone->selected_corner = corner;
    } else {
        keystone->selected_corner = -1;
    }
}

void keystone_move_corner(keystone_context_t *keystone, float dx, float dy) {
    if (keystone->selected_corner >= 0 && keystone->selected_corner < 4) {
        point_t *corner = &keystone->corners[keystone->selected_corner];

        // Allow callers to scale movement; fall back to a sane default
        float move_step = keystone->move_step;
        if (move_step <= 0.0f) {
            move_step = 0.05f;
        }
        
        // Store the previous position in case we need to revert
        float prev_x = corner->x;
        float prev_y = corner->y;
        
        // Apply movement with bounds checking
        corner->x += dx * move_step;
        corner->y += dy * move_step;
        
        // Enforce stricter bounds to prevent extreme transformations
        if (corner->x < -1.5f) corner->x = -1.5f;
        if (corner->x >  1.5f) corner->x =  1.5f;
        if (corner->y < -1.5f) corner->y = -1.5f;
        if (corner->y >  1.5f) corner->y =  1.5f;
        
        // Calculate area of the resulting quadrilateral to detect invalid configurations
        float area = 0.0f;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            area += keystone->corners[i].x * keystone->corners[j].y - 
                   keystone->corners[j].x * keystone->corners[i].y;
        }
        area = 0.5f * fabs(area);
        
        // If the resulting area is too small, revert the movement
        if (area < KEYSTONE_MIN_AREA_MOVE) {
            // printf("Invalid keystone configuration prevented (area too small)\n");
            corner->x = prev_x;
            corner->y = prev_y;
        } else {
            keystone->matrix_dirty = true;
            keystone->corners_dirty = true;  // Mark corners VBO needs update
        }
    }
}

void keystone_calculate_matrix(keystone_context_t *keystone) {
    if (!keystone->matrix_dirty) {
        return;
    }
    
    calculate_perspective_matrix(keystone->matrix, keystone->corners);
    keystone->matrix_dirty = false;
}

const float* keystone_get_matrix(keystone_context_t *keystone) {
    if (keystone->matrix_dirty) {
        keystone_calculate_matrix(keystone);
    }
    return keystone->matrix;
}

void keystone_reset_corners(keystone_context_t *keystone) {
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f,  1.0f};
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f,  1.0f};
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f, -1.0f};
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f, -1.0f};

    keystone->matrix_dirty = true;
    keystone->corners_dirty = true;  // Mark corners VBO needs update
}

void keystone_set_inset_corners(keystone_context_t *keystone, float margin) {
    // Set corners inset from the full -1.0 to 1.0 range by the specified margin
    // margin is in normalized coordinates (0.0 to 1.0)
    keystone->corners[CORNER_TOP_LEFT]     = (point_t){-1.0f + margin,  1.0f - margin};
    keystone->corners[CORNER_TOP_RIGHT]    = (point_t){ 1.0f - margin,  1.0f - margin};
    keystone->corners[CORNER_BOTTOM_RIGHT] = (point_t){ 1.0f - margin, -1.0f + margin};
    keystone->corners[CORNER_BOTTOM_LEFT]  = (point_t){-1.0f + margin, -1.0f + margin};

    keystone->matrix_dirty = true;
    keystone->corners_dirty = true;  // Mark corners VBO needs update
}

void keystone_toggle_corners(keystone_context_t *keystone) {
    keystone->show_corners = !keystone->show_corners;
}

bool keystone_corners_visible(keystone_context_t *keystone) {
    return keystone->show_corners;
}

void keystone_toggle_border(keystone_context_t *keystone) {
    keystone->show_border = !keystone->show_border;
}

bool keystone_border_visible(keystone_context_t *keystone) {
    return keystone->show_border;
}

void keystone_toggle_help(keystone_context_t *keystone) {
    keystone->show_help = !keystone->show_help;
}

bool keystone_help_visible(keystone_context_t *keystone) {
    return keystone->show_help;
}

void keystone_increase_step_size(keystone_context_t *keystone) {
    // PRODUCTION: Ultra-fine increments for pixel-perfect control
    keystone->move_step += 0.0001f;  // 0.01% increment
    if (keystone->move_step > 0.01f) {
        keystone->move_step = 0.01f; // Max step size
    }
    LOG_DEBUG("KEYSTONE", "Step size: %.4f", keystone->move_step);
}

void keystone_decrease_step_size(keystone_context_t *keystone) {
    // PRODUCTION: Ultra-fine decrements for pixel-perfect control
    keystone->move_step -= 0.0001f;  // 0.01% decrement
    if (keystone->move_step < 0.0001f) {
        keystone->move_step = 0.0001f; // Min step size
    }
    LOG_DEBUG("KEYSTONE", "Step size: %.4f", keystone->move_step);
}

float keystone_get_step_size(keystone_context_t *keystone) {
    return keystone->move_step;
}

int keystone_save_to_file(keystone_context_t *keystone, const char *filename) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        return -1;
    }
    
    // Save version marker for future compatibility
    fprintf(file, "# Pickle Keystone Configuration v1.0\n");
    
    // Save overlay display states
    fprintf(file, "show_corners=%d\n", keystone->show_corners ? 1 : 0);
    fprintf(file, "show_border=%d\n", keystone->show_border ? 1 : 0);
    fprintf(file, "show_help=%d\n", keystone->show_help ? 1 : 0);
    
    // Save the corner positions
    fprintf(file, "# Corner positions (x y)\n");
    for (int i = 0; i < 4; i++) {
        fprintf(file, "corner%d=%.6f %.6f\n", i, keystone->corners[i].x, keystone->corners[i].y);
    }
    
    fclose(file);
    return 0;
}

int keystone_load_from_file(keystone_context_t *keystone, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return -1;
    }
    
    char line[256];
    int corners_loaded = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        
        // Parse overlay display states
        int value;
        if (sscanf(line, "show_corners=%d", &value) == 1) {
            keystone->show_corners = (value != 0);
        } else if (sscanf(line, "show_border=%d", &value) == 1) {
            keystone->show_border = (value != 0);
        } else if (sscanf(line, "show_help=%d", &value) == 1) {
            keystone->show_help = (value != 0);
        } else {
            // Parse corner positions
            int corner_num;
            float x, y;
            if (sscanf(line, "corner%d=%f %f", &corner_num, &x, &y) == 3) {
                if (corner_num >= 0 && corner_num < 4) {
                    keystone->corners[corner_num].x = x;
                    keystone->corners[corner_num].y = y;
                    corners_loaded++;
                }
            }
        }
    }
    
    fclose(file);
    
    // Verify we loaded all corners
    if (corners_loaded != 4) {
        return -1;
    }
    
    keystone->matrix_dirty = true;
    return 0;
}

int keystone_save_settings(keystone_context_t *keystone) {
    const char *filename = "pickle_keystone.conf";
    return keystone_save_to_file(keystone, filename);
}

int keystone_load_settings(keystone_context_t *keystone) {
    const char *filename = "pickle_keystone.conf";
    if (keystone_load_from_file(keystone, filename) != 0) {
        // No config file found - create one with reset/default values
        LOG_INFO("KEYSTONE", "No config found, creating default pickle_keystone.conf");

        // Reset to default corner positions
        keystone_reset_corners(keystone);

        // Set default overlay states (all off)
        keystone->show_corners = false;
        keystone->show_border = false;
        keystone->show_help = false;

        // Save the default config
        if (keystone_save_to_file(keystone, filename) == 0) {
            LOG_INFO("KEYSTONE", "Created default pickle_keystone.conf with reset corner positions");
            return 0;
        } else {
            LOG_WARN("KEYSTONE", "Failed to create default config file");
            return -1;
        }
    }
    return 0;
}
