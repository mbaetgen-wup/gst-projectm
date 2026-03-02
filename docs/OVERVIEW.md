# Overview

## Build System

Automated CI/CD via GitHub Actions produces packages for every push, pull request, and tagged release.

| Platform | Formats | Architectures |
| --- | --- | --- |
| Linux | DEB, RPM, DEB Source, SRPM, Flatpak, AppImage | x86_64 |
| macOS | tar.gz | arm64 |
| Windows | ZIP | x64 |

Each binary package is built in three variants: **static-gl**, **static-gles**, and **dynamic**. See the [main README](../README.md#release-packages) for details.

## Features

- Accepts `audio/x-raw` streams as input
- Produces `video/x-raw(memory:GLMemory)` streams — rendered via OpenGL/GLES FBO, buffers stay in GPU memory
- Uses the C API of libprojectM 4.0+
- Configurable properties for presets, textures, mesh size, transitions, and more
- GL and GLES variant support
- Vendor metadata embedded in static builds (projectM ref, commit, GL variant)

## Contributors

- [Discord: tristancmoi](https://github.com/hashFactory)
- [Discord: CodAv](https://github.com/kblaschke)