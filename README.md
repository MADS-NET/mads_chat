# mads-chat

`mads-chat` is a small cross-platform GUI tool for inspecting and publishing messages on a MADS broker. It is built with Dear ImGui and the MADS C++ library and is mainly intended for debugging agent communication.

The window is split into two panes:

- `Publish`: choose a topic, edit a JSON payload with syntax highlighting, validate it, and publish it to the broker
- `Receive`: inspect the latest JSON received per topic, track elapsed time since the last message, and clear the current topic view

The top connection section lets you configure the host, subscribe and publish ports, subscription topics, and optional CURVE key files. The footer shows the detected MADS version and install prefix.

## Requirements

- CMake 3.24 or newer
- A C++20 compiler
- Ninja recommended as the generator
- A working MADS installation
- Network access during CMake configure, since Dear ImGui, GLFW, `portable-file-dialogs`, and `ImGuiColorTextEdit` are fetched with `FetchContent`

The build expects MADS to be installed already.

- The default MADS root is taken from `mads -p`
- Headers are expected under `$(mads -p)/include`
- The MADS core library is expected under:
  - macOS: `$(mads -p)/lib/libMadsCore.dylib`
  - Linux: `$(mads -p)/lib/libMadsCore.so`
  - Windows: `$(mads -p)/lib/MadsCore.lib`

Platform notes:

- macOS: Xcode command line tools are sufficient for building
- Linux: you need OpenGL/X11 development packages in addition to your compiler toolchain
- Windows: build with Visual Studio / MSVC and ensure the MADS import library and runtime DLL are installed

## Build

Configure and build with the default settings:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

By default:

- `CMAKE_BUILD_TYPE` is set to `Release` for single-config generators
- `MADS_ROOT` defaults to the output of `mads -p`
- `CMAKE_INSTALL_PREFIX` defaults to the output of `mads -p`

If you need a different MADS installation, override it explicitly:

```sh
cmake -S . -B build -G Ninja -DMADS_ROOT=/path/to/mads
cmake --build build
```

If you want a different install prefix:

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/some/prefix
cmake --build build
```

## Install

Install the executable with:

```sh
cmake --install build
```

This installs `mads-chat` into:

```text
<install-prefix>/bin
```

On macOS and Linux the target is configured with runtime search paths so the installed executable can resolve the MADS core library from the selected MADS prefix.

## Run

After build:

```sh
./build/mads-chat
```

After install:

```sh
mads-chat
```

or simply:

```sh
mads chat
```

At startup the application reads and writes `mads_multitool_settings.json` in the working directory to persist the connection settings.

