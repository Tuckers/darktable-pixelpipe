# libdtpipe

A standalone C library that extracts darktable's pixelpipe (image processing pipeline) for use without the darktable GUI or database.

## Overview

libdtpipe lets you load raw image files and apply darktable's image processing operations (IOPs) programmatically. It is designed to be embedded in other applications, including a Node.js addon.

## Directory Structure

```
libdtpipe/
├── CMakeLists.txt           # Root CMake file
├── cmake/
│   └── FindLCMS2.cmake      # Custom find modules
├── include/
│   └── dtpipe.h             # Public API header
├── src/
│   ├── CMakeLists.txt       # Source CMake file
│   ├── init.c               # Library initialization
│   ├── pipe/                # Pixelpipe implementation
│   ├── iop/                 # Image operation modules
│   ├── common/              # Shared utilities
│   └── imageio/             # Image I/O (rawspeed wrapper)
├── data/
│   ├── kernels/             # OpenCL kernels
│   └── color/               # Color profiles
├── tests/
│   └── CMakeLists.txt
└── node/                    # Node.js addon (Phase 6)
```

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Status

This library is under active development. See the [extraction project plan](../darktable-extraction-project-plan.md) for progress.

| Phase | Description               | Status      |
|-------|---------------------------|-------------|
| 0     | Analysis & Discovery      | In progress |
| 1     | Build System Bootstrap    | In progress |
| 2     | Strip GUI from IOPs       | Not started |
| 3     | Core Infrastructure       | Not started |
| 4     | Public API Implementation | Not started |
| 5     | History Serialization     | Not started |
| 6     | Node.js Addon             | Not started |
