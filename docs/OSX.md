# macOS

For plugin properties and presets/textures, see the [main README](../README.md).

## Installing Pre-built Packages

Download the `.tar.gz` for your preferred variant from [GitHub Releases](https://github.com/projectM-visualizer/gst-projectm/releases):

- **static-gl** — desktop OpenGL (recommended)
- **static-gles** — OpenGL ES
- **dynamic** — requires [projectM](https://github.com/projectM-visualizer/projectm) (>= 4.1.0) installed separately

```bash
tar xzf gstprojectm-*-macos-arm64-static-gl.tar.gz

# Homebrew GStreamer:
cp gstprojectm-*/lib/gstreamer-1.0/libgstprojectm.dylib \
  $(brew --prefix)/lib/gstreamer-1.0/

# — or — GStreamer.framework:
cp gstprojectm-*/lib/gstreamer-1.0/libgstprojectm.dylib \
  /Library/Frameworks/GStreamer.framework/Versions/1.0/lib/gstreamer-1.0/
```

### Verify

```bash
gst-inspect-1.0 projectm
```

## Building from Source

### Prerequisites

* Git, CMake (>= 3.8), Ninja
* [Homebrew](https://brew.sh/)
* GStreamer (>= 1.16) — `brew install gstreamer`
* [projectM](https://github.com/projectM-visualizer/projectm) (>= 4.0)

### Build

```bash
git clone https://github.com/projectM-visualizer/gst-projectm.git
cd gst-projectm

./setup.sh --auto
export PROJECTM_ROOT=/path/to/projectm
./build.sh --auto
```

### Install

The build script offers to install automatically. To install manually:

```bash
mkdir -p $HOME/.local/share/gstreamer-1.0/plugins/
cp dist/libgstprojectm.dylib $HOME/.local/share/gstreamer-1.0/plugins/
export GST_PLUGIN_PATH=$HOME/.local/share/gstreamer-1.0/plugins/
```

Add the `GST_PLUGIN_PATH` export to your shell profile to persist it.

### Test

```bash
./test.sh --inspect               # Inspect the plugin
./test.sh --audio                 # Test with audio
./test.sh --preset                # Test with a preset
./test.sh --properties            # Test with properties
./test.sh --output-video          # Test with video output
./test.sh --encode-output-video   # Test with encoded video output
```
