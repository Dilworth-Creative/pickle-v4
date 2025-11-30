# Makefile for RPi4 Video Player with Keystone Correction

# Compiler and flags
CC = gcc

# OPTIMIZED: RPi4-specific compiler flags
CFLAGS = -Wall -Wextra -std=c99 -O2 -g

# Architecture selection: 64 (default) or 32 for RPi3
ARCH ?= 64

# ARM CPU Detection and NEON SIMD optimization (works for both armv7 and aarch64)
ARCH_FLAGS = $(shell uname -m | grep -qE 'arm|aarch64' && echo '-ftree-vectorize -ffast-math')

# Architecture-specific flags
ifeq ($(ARCH),32)
    # 32-bit RPi3 build
    CC = arm-linux-gnueabihf-gcc
    ARCH_FLAGS += -march=armv7-a -mtune=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard
    # 32-bit library paths
    LDFLAGS = -Wl,-rpath,/usr/lib/arm-linux-gnueabihf -L/usr/lib/arm-linux-gnueabihf -Wl,--no-as-needed
    INCLUDES = -I/usr/include/arm-linux-gnueabihf -I/usr/include/libdrm
    LIBS = -ldrm -lgbm -lEGL -lGLESv2 -lavformat -lavcodec -lavutil -lswscale -lpthread -lm
    # Use 32-bit pkg-config
    PKG_CFLAGS = $(shell PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig pkg-config --cflags libdrm gbm egl glesv2 libavcodec libavformat libavutil libswscale 2>/dev/null)
    PKG_LIBS = $(shell PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig pkg-config --libs libdrm gbm egl glesv2 libavcodec libavformat libavutil libswscale 2>/dev/null)
else
    # 64-bit RPi4 build (default)
    ifeq ($(shell uname -m),aarch64)
        ARCH_FLAGS += -march=armv8-a+crc -mtune=cortex-a72 -mcpu=cortex-a72
    endif
    LDFLAGS = -Wl,-rpath,/usr/lib/aarch64-linux-gnu -L/usr/lib/aarch64-linux-gnu -Wl,--no-as-needed
    INCLUDES = -I/usr/include -I/usr/include/libdrm
    LIBS = -ldrm -lgbm -lEGL -lGLESv2 -lavformat -lavcodec -lavutil -lswscale -lpthread -lm
    PKG_CFLAGS = $(shell pkg-config --cflags libdrm gbm egl glesv2 libavcodec libavformat libavutil libswscale 2>/dev/null)
    PKG_LIBS = $(shell pkg-config --libs libdrm gbm egl glesv2 libavcodec libavformat libavutil libswscale 2>/dev/null)
endif

CFLAGS += $(ARCH_FLAGS)

# Additional RPi4 optimizations
RPi4_FLAGS = -pipe -fomit-frame-pointer -finline-functions -ffunction-sections -fdata-sections
CFLAGS += $(RPi4_FLAGS)

# Link-time optimization (reduce binary size, improve performance)
CFLAGS += -flto=auto

TARGET = pickle
SOURCES = pickel.c video_player.c drm_display.c drm_video_overlay.c gl_context.c video_decoder.c keystone.c input_handler.c v4l2_utils.c logging.c
OBJECTS = $(SOURCES:.c=.o)

# Library dependencies for RPi4
# UPDATED: Use official Debian FFmpeg 7.1.2 from apt
# This version includes all necessary V4L2 M2M and DRM support

# Include paths - use system FFmpeg from apt

# PKG-config for better library detection

# Use pkg-config for all libraries (64-bit default)
ifneq ($(PKG_LIBS),)
    LIBS = $(PKG_LIBS) -lpthread -lm
endif

ifneq ($(PKG_CFLAGS),)
    INCLUDES = $(PKG_CFLAGS)
endif

# Default target
all: $(TARGET)

# Build the executable with optimizations
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--gc-sections -Wl,-O1 -o $(TARGET) $(OBJECTS) $(LIBS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Run the program (requires video file argument)
run: $(TARGET)
	@echo "Usage: ./$(TARGET) <video_file.mp4>"
	@echo "Make sure you have a video file to test with"

# Test with a sample video (if available)
test: $(TARGET)
	@if [ -f test.mp4 ]; then \
		echo "Running with test.mp4..."; \
		sudo ./$(TARGET) test.mp4; \
	else \
		echo "No test.mp4 file found. Please provide a video file:"; \
		echo "sudo ./$(TARGET) your_video.mp4"; \
	fi

# Install dependencies (Debian/Ubuntu/Raspberry Pi OS)
install-deps:
	@echo "Installing development dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		libdrm-dev \
		libgbm-dev \
		libegl1-mesa-dev \
		libgles2-mesa-dev \
		libavformat-dev \
		libavcodec-dev \
		libavutil-dev \
		libswscale-dev \
		pkg-config

# Install 32-bit dependencies for RPi3 cross-compilation
install-deps-32:
	@echo "Installing 32-bit development dependencies for RPi3..."
	sudo apt-get update
	sudo apt-get install -y \
		gcc-arm-linux-gnueabihf \
		libc6-dev-armhf-cross \
		libdrm-dev:armhf \
		libgbm-dev:armhf \
		libegl1-mesa-dev:armhf \
		libgles2-mesa-dev:armhf \
		libavformat-dev:armhf \
		libavcodec-dev:armhf \
		libavutil-dev:armhf \
		libswscale-dev:armhf

# Clean up generated files
clean:
	rm -f $(TARGET) $(OBJECTS)

# Rebuild (clean + build)
rebuild: clean all

# Strip binary for smaller size (optional)
strip:
	strip $(TARGET)

# Show build flags
show-flags:
	@echo "Build flags for this system (ARCH=$(ARCH)):"
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "ARCH_FLAGS: $(ARCH_FLAGS)"
	@echo "System: $$(uname -m)"

# Release build with maximum optimization
release: CFLAGS = -Wall -Wextra -std=c99 -O3 -DNDEBUG
release: CFLAGS += $(ARCH_FLAGS) $(RPi4_FLAGS)
release: CFLAGS += -flto=auto -ffunction-sections -fdata-sections
# RPi4-specific release optimizations
ifeq ($(shell uname -m),aarch64)
release: CFLAGS += -march=armv8-a+crc+simd -mtune=cortex-a72 -mcpu=cortex-a72
endif
release: LDFLAGS = -Wl,--gc-sections,-s -flto=auto -L/usr/local/lib -Wl,-rpath,/usr/local/lib
release: clean $(TARGET)
	@echo "Release build complete: $(TARGET)"
	@ls -lh $(TARGET)

# Debug build with extra symbols
debug: override CFLAGS = -Wall -Wextra -std=c99 -O0 -g3 -ggdb -fno-omit-frame-pointer -fno-inline
debug: override CFLAGS += $(ARCH_FLAGS)
debug: override LDFLAGS =
debug: clean $(TARGET)
	@echo "Debug build complete: $(TARGET)"
	@ls -lh $(TARGET)

# 32-bit RPi3 build
rpi3: clean
	$(MAKE) TARGET=pickle3 ARCH=32 all
	@echo "32-bit RPi3 build complete: pickle3"
	@ls -lh pickle3

# Show build information
info:
	@echo "Target: $(TARGET)"
	@echo "Architecture: $(ARCH)-bit"
	@echo "Compiler: $(CC)"
	@echo "Sources: $(SOURCES)"
	@echo "Objects: $(OBJECTS)"
	@echo "Flags: $(CFLAGS)"
	@echo "Includes: $(INCLUDES)"
	@echo "Libraries: $(LIBS)"

# Help target
help:
	@echo "Pickle Video Player - RPi Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build the video player (default, 64-bit RPi4 -> pickle)"
	@echo "  rpi3         - Build 32-bit version for RPi3 (-> pickle3)"
	@echo "  release      - Build with maximum optimization (-O3 -flto, stripped)"
	@echo "  debug        - Build with debug symbols (-ggdb3)"
	@echo "  clean        - Remove generated files"
	@echo "  rebuild      - Clean and build"
	@echo "  install-deps - Install required system dependencies (64-bit)"
	@echo "  install-deps-32 - Install 32-bit dependencies for RPi3 cross-compilation"
	@echo "  test         - Run with test.mp4 if available"
	@echo "  info         - Show build configuration"
	@echo "  show-flags   - Display compiler optimization flags"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Usage: sudo ./$(TARGET) <video_file.mp4>"
	@echo "Note: Requires root privileges for direct hardware access (DRM/KMS)"
	@echo ""
	@echo "For 32-bit RPi3 builds:"
	@echo "  make rpi3"
	@echo "  ARCH=32 make all"

# Dependencies
pickel.o: pickel.c video_player.h
video_player.o: video_player.c video_player.h drm_display.h gl_context.h video_decoder.h keystone.h input_handler.h
drm_display.o: drm_display.c drm_display.h
gl_context.o: gl_context.c gl_context.h drm_display.h
video_decoder.o: video_decoder.c video_decoder.h
keystone.o: keystone.c keystone.h
input_handler.o: input_handler.c input_handler.h

# Phony targets
.PHONY: all run test clean rebuild debug release info help install-deps install-deps-32 rpi3