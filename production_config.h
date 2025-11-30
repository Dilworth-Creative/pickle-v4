#ifndef PRODUCTION_CONFIG_H
#define PRODUCTION_CONFIG_H

// Production safety limits - Optimized for Raspberry Pi 4 (2GB model)
// RPi4 hardware decode supports up to 4K@60fps H.264, 4K@30fps HEVC
// Limiting to 4K prevents excessive memory usage on 2GB model
#define MAX_VIDEO_WIDTH 3840   // 4K width
#define MAX_VIDEO_HEIGHT 2160  // 4K height
#define MAX_FRAME_SIZE (MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT * 3 / 2)
#define MAX_DECODE_ATTEMPTS 3
#define DECODE_TIMEOUT_MS 5000
#define MEMORY_LIMIT_MB 512     // Conservative limit for 2GB Pi 4 (leaves headroom for GPU/system)

// File size limits
#define MAX_VIDEO_FILE_SIZE (4ULL * 1024 * 1024 * 1024) // 4GB

// Performance tuning
#define FRAME_BUFFER_COUNT 3
#define DECODE_THREAD_COUNT 4
#define GL_SYNC_INTERVAL 1

// Debug/logging configuration
#ifdef NDEBUG
  #define PRODUCTION_BUILD 1
  #define DEFAULT_LOG_LEVEL VIDEO_LOG_WARN
#else
  #define PRODUCTION_BUILD 0
  #define DEFAULT_LOG_LEVEL VIDEO_LOG_INFO
#endif

// Feature flags
#define ENABLE_HARDWARE_DECODE 1
#define ENABLE_FRAME_DROPPING 1
#define ENABLE_AUTO_RECOVERY 1

#endif // PRODUCTION_CONFIG_H
