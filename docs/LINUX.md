# Linux

## Installing Pre-built Packages

Pre-built packages install the `projectm` GStreamer plugin element into your existing GStreamer installation. After installing, the element is available in any pipeline on the system.

For presets and textures, see [Presets and Textures](../README.md#presets-and-textures) in the main README.

### Ubuntu / Debian

```bash
# Static GL (recommended):
sudo apt install ./gstreamer1.0-projectm-static-gl_*.deb

# Static GLES (for embedded / GLES-only systems):
sudo apt install ./gstreamer1.0-projectm-static-gles_*.deb

# Dynamic (requires projectM >= 4.1.0 installed separately):
sudo apt install ./gstreamer1.0-projectm-dynamic_*.deb
```

### Fedora / RHEL

```bash
# Static GL (recommended):
sudo rpm -i gstreamer1-projectm-static-gl-*.rpm

# Static GLES (for embedded / GLES-only systems):
sudo rpm -i gstreamer1-projectm-static-gles-*.rpm

# Dynamic (requires projectM >= 4.1.0 installed separately):
sudo rpm -i gstreamer1-projectm-dynamic-*.rpm
```

### Verify

```bash
gst-inspect-1.0 projectm
```

## Building from Source Packages

Source packages let you rebuild the plugin for your specific distribution and architecture without cloning the repository. Available as `.dsc` (Debian) and `.src.rpm` (RPM) on each [release](https://github.com/projectM-visualizer/gst-projectm/releases).

### Debian / Ubuntu

```bash
# Install build dependencies
sudo apt-get install build-essential fakeroot devscripts debhelper cmake \
  ninja-build rsync pkg-config libgl1-mesa-dev mesa-common-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

# Extract and build (download the .dsc, .orig.tar.gz, and .debian.tar.xz first)
dpkg-source -x gstreamer1.0-projectm-static_*.dsc
cd gstreamer1.0-projectm-static-*/
debuild -us -uc

# Install
sudo apt install ../gstreamer1.0-projectm-static_*.deb
```

Replace `static` with `dynamic` if you want the dynamic variant. The dynamic variant requires projectM (>= 4.1.0) development headers to be installed before building.

### Fedora / RHEL

```bash
# Install build dependencies
sudo dnf install rpm-build cmake ninja-build rsync gcc gcc-c++ pkgconfig \
  mesa-libGL-devel mesa-libEGL-devel gstreamer1-devel \
  gstreamer1-plugins-base-devel glib2-devel

# Rebuild from source RPM
rpmbuild --rebuild gstreamer1-projectm-static-*.src.rpm

# Install
sudo rpm -i ~/rpmbuild/RPMS/x86_64/gstreamer1-projectm-static-*.rpm
```

## Building from Source

### Prerequisites

* Git, CMake (>= 3.8), Ninja
* GStreamer development packages (>= 1.16)
* [projectM](https://github.com/projectM-visualizer/projectm) (>= 4.0)

### Build

```bash
git clone https://github.com/projectM-visualizer/gst-projectm.git
cd gst-projectm

# Install system dependencies
./setup.sh --auto

# Set projectM location (if not in /usr/local)
export PROJECTM_ROOT=/path/to/projectm

# Build
./build.sh --auto
```

### Install

The build script offers to install automatically. To install manually:

```bash
mkdir -p $HOME/.local/share/gstreamer-1.0/plugins/
cp dist/libgstprojectm.so $HOME/.local/share/gstreamer-1.0/plugins/
export GST_PLUGIN_PATH=$HOME/.local/share/gstreamer-1.0/plugins/
```

Add the `GST_PLUGIN_PATH` export to your shell profile (`~/.bashrc`, `~/.zshrc`, etc.) to persist it.

### Test

```bash
./test.sh --inspect               # Inspect the plugin
./test.sh --audio                 # Test with audio
./test.sh --preset                # Test with a preset
./test.sh --properties            # Test with properties
./test.sh --output-video          # Test with video output
./test.sh --encode-output-video   # Test with encoded video output
```

---

## Portable Bundles (Flatpak / AppImage)

Flatpak and AppImage bundles are **self-contained executables that include their own copy of GStreamer** and run `gst-launch-1.0` directly. They are intended for quick testing of the plugin without installing anything on your system. They do **not** install the `projectm` element into your system GStreamer — for that, use the DEB/RPM packages above.

Presets and textures are **not** included — see [Presets and Textures](../README.md#presets-and-textures) in the main README.

### Flatpak

```bash
# Install Flatpak if not present
sudo apt install flatpak    # Debian/Ubuntu
sudo dnf install flatpak    # Fedora

# Install the GL bundle (recommended for desktop):
flatpak install --user gst-projectm-*-gl-x86_64.flatpak

# — or — GLES bundle (embedded / GLES-only systems):
flatpak install --user gst-projectm-*-gles-x86_64.flatpak
```

#### Running

Pass `gst-launch-1.0` pipeline arguments directly:

```bash
flatpak run org.projectmvisualizer.GstLaunch \
  audiotestsrc ! queue ! audioconvert \
  ! projectm preset=/path/to/presets texture-dir=/path/to/textures preset-duration=5 \
  ! "video/x-raw,width=1920,height=1080,framerate=60/1" \
  ! videoconvert ! autovideosink sync=false
```

Note: the Flatpak sandbox has read-only access to your home directory. Place presets and textures under your home directory so the bundle can access them.

#### Overriding Environment Variables

The bundle ships GL defaults per variant. Override at launch:

```bash
flatpak run --env=GST_GL_API=gles2 --env=GST_DEBUG=3 \
  org.projectmvisualizer.GstLaunch ...
```

### AppImage

```bash
chmod +x gst-projectm-*-x86_64.AppImage

./gst-projectm-*-gl-x86_64.AppImage \
  audiotestsrc ! queue ! audioconvert \
  ! projectm preset=/path/to/presets preset-duration=5 \
  ! "video/x-raw,width=1920,height=1080,framerate=60/1" \
  ! videoconvert ! autovideosink sync=false
```

AppImages are available for multiple GStreamer versions (e.g. 1.24.x, 1.28.x). Choose the version that matches your needs.

#### Overriding Environment Variables

Override defaults by exporting before launch:

```bash
GST_GL_API=gles2 GST_DEBUG=gl*:4 ./gst-projectm-*.AppImage ...
```

### Bundle Environment Variable Defaults

Both Flatpak and AppImage set GL defaults per variant:

| Variable | GL Default | GLES Default | Description |
| --- | --- | --- | --- |
| `GST_GL_API` | `opengl3` | `gles2` | GL API to request |
| `GST_GL_CONTEXT` | `"opengl=3.3"` | `"gles2=3.2"` | GL context version |

These can be overridden as shown above for each bundle type.

---

## GStreamer GL Reference

GStreamer's GL subsystem is configured through environment variables. These are useful when running inside Flatpak/AppImage, in containers (Docker, Podman), in headless environments, or when troubleshooting GPU issues.

### Core GL Variables

| Variable | Values | Description |
| --- | --- | --- |
| `GST_GL_API` | `opengl`, `opengl3`, `gles2` | Force a specific GL API. `opengl3` requests a core profile; `gles2` requests GLES 2.0+. |
| `GST_GL_CONTEXT` | `"opengl=3.3"`, `"gles2=3.2"` | Request a specific GL context version. Quotes are part of the value. |
| `GST_GL_PLATFORM` | `egl`, `glx`, `wgl`, `cgl` | Force a GL platform. `egl` works with Wayland and X11; `glx` is X11-only. |
| `GST_GL_WINDOW` | `x11`, `wayland`, `viv-fb`, `gbm`, `dispmanx` | Force a windowing backend. |

### Debugging

| Variable | Example | Description |
| --- | --- | --- |
| `GST_DEBUG` | `gl*:4`, `3`, `*:2,projectm:5` | Debug output level. `gl*:4` enables verbose GL logging. |
| `GST_DEBUG_NO_COLOR` | `1` | Disable colored debug output. |
| `GST_DEBUG_FILE` | `/tmp/gst.log` | Write debug output to a file. |

### GPU Pass-through in Containers

**Flatpak** bundles already configure `--device=dri` for GPU access. If you experience issues:

```bash
# Force EGL (often more reliable in sandboxed environments):
flatpak run --env=GST_GL_PLATFORM=egl org.projectmvisualizer.GstLaunch ...
```

**AppImage** uses the host GPU directly. Troubleshooting:

```bash
GST_GL_PLATFORM=egl ./gst-projectm-*.AppImage ...    # Force EGL
GST_GL_WINDOW=x11 ./gst-projectm-*.AppImage ...      # Force X11
GST_GL_WINDOW=wayland ./gst-projectm-*.AppImage ...   # Force Wayland
```

**Docker / Podman**:

```bash
# X11 with DRI access
docker run --device /dev/dri -e DISPLAY=$DISPLAY \
  -v /tmp/.X11-unix:/tmp/.X11-unix ...

# NVIDIA GPUs
docker run --gpus all -e DISPLAY=$DISPLAY \
  -v /tmp/.X11-unix:/tmp/.X11-unix ...

# Headless rendering (no display server)
docker run --device /dev/dri \
  -e GST_GL_PLATFORM=egl -e GST_GL_WINDOW=gbm ...
```

### NVIDIA-specific Variables

| Variable | Values | Description |
| --- | --- | --- |
| `__GLX_VENDOR_LIBRARY_NAME` | `nvidia`, `mesa` | Select GLX vendor on multi-GPU systems (PRIME). |
| `__NV_PRIME_RENDER_OFFLOAD` | `1` | Enable PRIME render offload to NVIDIA GPU. |
| `__EGL_VENDOR_LIBRARY_DIRS` | `/usr/share/glvnd/egl_vendor.d` | Override EGL vendor search path. |
