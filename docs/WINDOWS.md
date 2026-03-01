# Windows

For plugin properties and presets/textures, see the [main README](../README.md).

## Installing Pre-built Packages

Download the `.zip` for your preferred variant from [GitHub Releases](https://github.com/projectM-visualizer/gst-projectm/releases):

- **static-gl** — desktop OpenGL (recommended)
- **static-gles** — OpenGL ES
- **dynamic** — requires [projectM](https://github.com/projectM-visualizer/projectm) (>= 4.1.0) installed separately

Extract `gstprojectm.dll` and copy it to your GStreamer plugins directory:

```powershell
# User-local plugins directory:
Copy-Item gstprojectm.dll "$Env:USERPROFILE\.gstreamer\1.0\plugins\"

# — or — System GStreamer installation:
Copy-Item gstprojectm.dll "C:\gstreamer\1.0\msvc_x86_64\lib\gstreamer-1.0\"
```

Optionally set `GST_PLUGIN_PATH` so GStreamer always finds the plugin:

```powershell
[Environment]::SetEnvironmentVariable("GST_PLUGIN_PATH", "$Env:USERPROFILE\.gstreamer\1.0\plugins", "User")
```

### Verify

```powershell
gst-inspect-1.0 projectm
```

## Building from Source

### Prerequisites

* Git, CMake (>= 3.8)
* [Visual Studio 2017+](https://www.visualstudio.com/downloads/) (Community edition is fine)
* [Vcpkg](https://github.com/Microsoft/vcpkg)
* [GStreamer](https://gstreamer.freedesktop.org/download/) (>= 1.16)
* [projectM](https://github.com/projectM-visualizer/projectm) (>= 4.0)

### Build

```powershell
git clone https://github.com/projectM-visualizer/gst-projectm.git
cd gst-projectm

[Environment]::SetEnvironmentVariable("PROJECTM_ROOT", "C:\path\to\projectm", "User")
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\path\to\vcpkg", "User")

.\build.ps1 --auto
```

### Install

The build script offers to install automatically. To install manually:

```powershell
New-Item -Path "$Env:USERPROFILE\.gstreamer\1.0\plugins" -ItemType Directory -Force | Out-Null
Copy-Item "dist\gstprojectm.dll" "$Env:USERPROFILE\.gstreamer\1.0\plugins\" -Force
[Environment]::SetEnvironmentVariable("GST_PLUGIN_PATH", "$Env:USERPROFILE\.gstreamer\1.0\plugins", "User")
```

### Test

```powershell
./test.ps1 --inspect               # Inspect the plugin
./test.ps1 --audio                 # Test with audio
./test.ps1 --preset                # Test with a preset
./test.ps1 --properties            # Test with properties
./test.ps1 --output-video          # Test with video output
./test.ps1 --encode-output-video   # Test with encoded video output
```
