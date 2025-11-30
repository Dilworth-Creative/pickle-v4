#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

// Hardware decoding types
typedef enum {
    HW_DECODE_NONE = 0,
    HW_DECODE_V4L2M2M,
    HW_DECODE_MMAL,
    HW_DECODE_DRM_PRIME
} hw_decode_type_t;

typedef struct {
    AVFormatContext *format_ctx;
    const AVCodec *codec;
    AVCodecContext *codec_ctx;
    AVPacket *packet;
    AVFrame *frame;
    int video_stream_index;
    int width;
    int height;
    double fps;
    int64_t duration;
    
    bool initialized;
    bool use_hardware_decode;
    hw_decode_type_t hw_decode_type;
    int avcc_length_size; // For V4L2 M2M hardware decoder
    bool eof_reached;
    bool loop_playback;
    bool using_drm_prime;
    bool advanced_diagnostics; // Flag for detailed diagnostics output
    bool enable_hardware_decode; // Flag to enable hardware decode (--hw flag)
    bool skip_sw_transfer;     // Skip av_hwframe_transfer_data when using EGL/DMA zero-copy
    
    // 2-stage Bitstream filter chain for V4L2 M2M: avcC→Annex-B + AUD insertion
    AVBSFContext *bsf_annexb_ctx;    // Stage 1: h264_mp4toannexb (avcC to Annex-B conversion)
    AVBSFContext *bsf_aud_ctx;       // Stage 2: h264_metadata (AUD insertion)
    
    // V4L2 M2M hardware decoder device handle for DMA buffer export
    int v4l2_fd;                     // V4L2 device file descriptor (-1 if unavailable)
    unsigned int v4l2_buffer_index;  // Current output buffer index from V4L2 M2M decoder
    
    // NV12 conversion buffer (per context to support multi-video playback)
    uint8_t *nv12_buffer;
    int nv12_buffer_size;
    
    // Hardware acceleration contexts (FFmpeg hwaccel API)
    AVBufferRef *hw_device_ctx;      // Hardware device context (DRM/VAAPI/etc)
    AVBufferRef *hw_frames_ctx;      // Hardware frames context for zero-copy buffers
    enum AVPixelFormat hw_pix_fmt;   // Hardware pixel format (AV_PIX_FMT_DRM_PRIME/NV12)

    // Software frame for hardware-decoded content
    // When using DRM_PRIME or other hw formats, we transfer
    // the frame into this CPU-accessible YUV420P frame so that
    // existing CPU upload paths can render it.
    AVFrame *sw_frame;
    
    // DMA buffer zero-copy support (for hardware decoded frames)
    bool supports_dma_export;        // True if hardware decoder supports DMA buffer export
    int dma_fd;                      // DMA buffer file descriptor for current frame (-1 if unavailable)
    int dma_offset;                  // Byte offset within DMA buffer where frame data starts
    size_t dma_size;                 // Total size of DMA buffer
    
    // DMA plane layout (for YUV420P zero-copy rendering)
    int dma_plane_offset[3];         // Byte offsets for Y, U, V planes
    int dma_plane_pitch[3];          // Pitch (stride) for Y, U, V planes
    
    // Thread safety
    pthread_mutex_t lock;            // Mutex for thread-safe access to context
    
    // Per-context state (moved from static variables for thread safety)
    int callback_count;              // Format callback invocation count
    int frame_count;                 // Decoded frame count
    bool debug_printed;              // Debug output flag
    int decode_call_count;           // Number of decode function calls
    int hw_fallback_retry_count;     // Hardware decode fallback retry counter
    
    // Cached buffers for V4L2 M2M frames (speeds up GL texture upload)
    uint8_t *cached_y_buffer;
    uint8_t *cached_u_buffer;
    uint8_t *cached_v_buffer;
    size_t cached_y_size;
    size_t cached_u_size;
    size_t cached_v_size;
    void *last_y_source;             // Track if we already copied this frame
    void *last_u_source;
    void *last_v_source;
    
    // PRODUCTION: Interrupt callback for network timeout protection
    int64_t last_io_activity;        // Timestamp of last I/O activity (av_gettime_relative)
    int io_timeout_us;               // I/O timeout in microseconds (default 5s)
} video_context_t;

// Video decoder functions
int video_init(video_context_t *video, const char *filename, bool advanced_diagnostics, bool enable_hardware_decode);
void video_cleanup(video_context_t *video);
int video_decode_frame(video_context_t *video);
uint8_t* video_get_rgb_data(video_context_t *video);  // Legacy - for fallback
uint8_t* video_get_y_data(video_context_t *video);
uint8_t* video_get_u_data(video_context_t *video);
uint8_t* video_get_v_data(video_context_t *video);
int video_get_y_stride(video_context_t *video);
int video_get_u_stride(video_context_t *video);
int video_get_v_stride(video_context_t *video);
void video_get_yuv_data(video_context_t *video, uint8_t **y, uint8_t **u, uint8_t **v, 
                       int *y_stride, int *u_stride, int *v_stride);  // New YUV output
uint8_t* video_get_nv12_data(video_context_t *video);  // NV12 packed format
int video_get_nv12_stride(video_context_t *video);
bool video_frame_is_nv12(video_context_t *video);
double video_get_frame_time(video_context_t *video);
bool video_is_eof(video_context_t *video);
void video_seek(video_context_t *video, int64_t timestamp);
void video_get_dimensions(video_context_t *video, int *width, int *height);
bool video_is_hardware_decoded(video_context_t *video);
int video_restart_playback(video_context_t *video);
void video_set_loop(video_context_t *video, bool loop);

// DMA buffer zero-copy support
bool video_has_dma_buffer(video_context_t *video);
int video_get_dma_fd(video_context_t *video);
void video_get_dma_plane_layout(video_context_t *video, int offsets[3], int pitches[3]);
int video_get_dma_offset(video_context_t *video);
size_t video_get_dma_size(video_context_t *video);

// Debug flag for hardware decoder diagnostics
extern bool hw_debug_enabled;

#endif // VIDEO_DECODER_H