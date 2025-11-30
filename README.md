# Pickle Video Player

**Version:** 1.1.0 (main)

A high-performance dual-video player designed for embedded Linux systems. Features hardware-accelerated decoding, OpenGL ES rendering via DRM/KMS, and real-time keystone correction for projection mapping applications.

## Target Platform

**Raspberry Pi 4 (2GB model)** - Optimized for low-memory embedded deployment.

### Memory Usage
| Resolution | Per-Video Buffers | Dual Video Total |
|------------|------------------|------------------|
| 1080p      | ~15-25 MB        | ~40-60 MB        |
| 4K         | ~60-100 MB       | ~140-220 MB      |

**Recommendation:** Use 1080p content for reliable dual-video playback on 2GB systems. 4K is supported but may cause memory pressure under heavy system load.

## Features

- **Dual video playback** with independent keystone correction for each stream
- **Hardware decode support** via V4L2 M2M (use `--hw` flag)
- **Optimized software decode** with direct YUV420P texture upload (default)
- **DRM/KMS direct scanout** using OpenGL ES 3.1 rendering
- **Gamepad and keyboard** input support for interactive control
- **Real-time profiling** with `--timing` flag
- **Persistent keystone settings** automatically saved per video configuration

## Quick Start

### Build
```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libdrm-dev libgbm-dev libgles2-mesa-dev

# Compile
make clean
make
```

### Usage
```bash
# Single video
./pickle /path/to/video.mp4

# Dual video playback
./pickle /path/to/left.mp4 /path/to/right.mp4

# With hardware decoding
./pickle --hw /path/to/video.mp4

# Check version
./pickle --version
```

## Performance Optimization

**Critical:** Set CPU governor to performance mode to prevent stuttering from frequency scaling.

### Quick Fix (Temporary)
```bash
sudo sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
```

### Verify
```bash
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
# Should show 'performance' for all cores
```

### Permanent Configuration

**Method 1: cpufrequtils**
```bash
sudo apt install cpufrequtils
# Add to /etc/default/cpufrequtils: GOVERNOR="performance"
sudo systemctl restart cpufrequtils
```

**Method 2: systemd service**

Create `/etc/systemd/system/cpu-performance.service`:
```ini
[Unit]
Description=Set CPU Governor to Performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Enable with:
```bash
sudo systemctl enable --now cpu-performance.service
```

## Versioning

Edit `version.h` to bump versions following semantic versioning:
- `VERSION_MAJOR`: Incompatible changes
- `VERSION_MINOR`: New features (backward-compatible)
- `VERSION_PATCH`: Bug fixes (backward-compatible)

## Troubleshooting

- **Stuttering/Spikes:** Check CPU governor and thermal throttling (`cat /sys/class/thermal/thermal_zone*/temp`)
- **Hardware decode fails:** Player auto-falls back to software. Check `dmesg` for V4L2 errors
- **Display issues:** Ensure user is in `video` and `render` groups for DRM/KMS access

## Performance Results

With performance governor enabled:
- Consistent 3-6ms frame times
- GL operations: 2.9-3.3ms
- Decode times: 0.6-1.4ms average
- No periodic 100-140ms spikes

---

*Note: Performance mode increases power consumption. Revert to "ondemand" on battery devices when not in use.*







