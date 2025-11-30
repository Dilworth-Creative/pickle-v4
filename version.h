#ifndef VERSION_H
#define VERSION_H

// Semantic versioning for pickle video player
// Format: MAJOR.MINOR.PATCH
// MAJOR: Incompatible API/behavior changes
// MINOR: Backward-compatible functionality additions
// PATCH: Backward-compatible bug fixes

#define VERSION_MAJOR 1
#define VERSION_MINOR 1
#define VERSION_PATCH 1

// Build metadata (optional)
#define VERSION_BUILD "main"

// Helper macros for version string construction
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define VERSION_STRING \
    TOSTRING(VERSION_MAJOR) "." \
    TOSTRING(VERSION_MINOR) "." \
    TOSTRING(VERSION_PATCH)

#define VERSION_FULL \
    "pickle v" VERSION_STRING " (" VERSION_BUILD ")"

#endif // VERSION_H
