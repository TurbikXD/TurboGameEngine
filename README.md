# TurboGameEngine

Minimal, extensible C++20 game engine skeleton with a modern RHI (Render Hardware Interface) that is Vulkan/D3D12-shaped, plus a fully working OpenGL backend for fast delivery.

## Architecture Overview

1. **Core and platform first**: lifecycle, logging, config, timing, window/event/input abstractions.
2. **Strict RHI boundary**: renderer/game code depends only on `engine/rhi/*` interfaces, never GL/Vulkan/D3D12 headers.
3. **Backend modules**:
   - `engine/rhi_opengl`: fully functional rendering path (triangle + transform + input).
   - `engine/rhi_vulkan`: compile-ready scaffolding aligned to modern explicit API concepts.
   - `engine/rhi_d3d12`: compile-ready scaffolding on Windows with D3D12 responsibilities.
4. **Renderer on top of RHI**: `Renderer` owns device/swapchain/pipeline/resources and issues frame commands.
5. **Game state stack**: deferred push/pop/replace with `MenuState`, `GameplayState`, and `PauseState`.

## Layering Rules

- `engine/game` and `engine/renderer` use only:
  - `engine/rhi/*`
  - `engine/platform/*`
  - `engine/core/*`
- OpenGL/Vulkan/D3D12 headers are limited to backend implementations (`engine/rhi_*`).
- GLFW types are hidden in platform implementation (`WindowGLFW.cpp`); public platform headers expose engine event/input types only.

## Implemented Assignment Features

- Application lifecycle: init -> fixed-step main loop -> shutdown.
- High precision timing with `std::chrono::steady_clock`.
- Once-per-second frame timing logs (avg/min/max) via aggregated stats.
- Logging to console + rotating file sink in `logs/engine.log`.
- Backend abstraction through RHI + `RenderAdapter` behavior via `Renderer`.
- Working OpenGL demo rendering a primitive through RHI interfaces.
- Input-driven transform controls using `deltaTime`.
- State pattern with deferred transitions:
  - `MenuState` -> Enter -> `GameplayState`
  - `GameplayState` -> Esc -> push `PauseState`
  - `PauseState` -> Esc -> pop (resume gameplay)

## Repository Layout

Important engine directories:

- `engine/core`: `Application`, `Log`, `Time`, `Config`, `Assert`
- `engine/platform`: API-agnostic window, GLFW implementation, events, input
- `engine/rhi`: backend-agnostic interfaces and types
- `engine/rhi_opengl`: fully working backend
- `engine/rhi_vulkan`: compile-ready skeleton
- `engine/rhi_d3d12`: compile-ready skeleton (Windows)
- `engine/renderer`: RHI-only renderer wrapper
- `engine/game`: state stack and states
- `app/main.cpp`: executable entry

Shader assets are stored under `assets/shaders_gl` and `assets/shaders_hlsl` so they can be copied as one runtime asset tree.

## Build Requirements

- CMake >= 3.24
- C++20 compiler
- Python 3 (used to generate glad OpenGL loader from FetchContent source)
- Internet connection for first configure (FetchContent dependencies)

Dependencies are pulled with `FetchContent`:

- GLFW
- glad (generated from fetched source)
- glm
- spdlog
- nlohmann/json
- Vulkan-Headers (when enabled)

## Build and Run (Command Line)

### Debug (default OpenGL)

```bash
cmake --preset debug --fresh
cmake --build --preset debug
```

Run:

- Windows: `build/debug/Debug/app.exe`

### Release

```bash
cmake --preset release --fresh
cmake --build --preset release
```

Run:

- Windows: `build/release/Release/app.exe`

## CLion Setup

1. Open the project root in CLion.
2. Select `debug` or `release` CMake preset.
3. Build target `app`.
4. Run target `app`.

Presets are Visual Studio generator based in this repo for compatibility on this Windows environment.

## Backend Selection Options

CMake cache options:

- `ENGINE_BACKEND=opengl|vulkan|d3d12` (default `opengl`)
- `ENGINE_ENABLE_VULKAN=ON|OFF` (default `ON`)
- `ENGINE_ENABLE_D3D12=ON|OFF` (default `ON` on Windows, `OFF` elsewhere)

Examples:

```bash
cmake --preset debug --fresh -DENGINE_BACKEND=opengl
cmake --preset debug --fresh -DENGINE_BACKEND=vulkan -DENGINE_ENABLE_VULKAN=ON
cmake --preset debug --fresh -DENGINE_BACKEND=d3d12 -DENGINE_ENABLE_D3D12=ON
```

Backend status:

- **OpenGL**: implemented and runnable.
- **Vulkan**: compiles and is wired, runtime path is scaffolded/stubbed.
- **D3D12**: compiles on Windows and is wired, runtime path is scaffolded/stubbed.

If Vulkan or D3D12 is selected at runtime, the architecture is in place but full rendering behavior is not implemented yet.

## Controls

- `Enter`: start game from menu
- `Esc`:
  - gameplay -> pause
  - pause -> resume
- `Arrow keys`: move primitive
- `LMB` (hold): scale up
- `RMB` (hold): rotate

All gameplay transform controls are `deltaTime`-based for smooth, frame-rate-independent movement.

## Config

Optional `config.json` in working directory:

```json
{
  "width": 1280,
  "height": 720,
  "title": "TurboGameEngine",
  "vsync": true,
  "clearColor": [0.1, 0.1, 0.16, 1.0],
  "initialState": "menu"
}
```

If missing, defaults are used.

## Logging

- Console logger with color
- Rotating file logger:
  - Path: `logs/engine.log`
  - Size: 5 MB per file
  - Rotations: 3

Logged events include init/shutdown, backend selection, state transitions, resize events, and once-per-second frame timing stats.

## Optional Shader Compiler Target

`tools/shader_compiler` adds target:

- `shader_compile_hlsl`

If `dxc` is on PATH, it compiles `assets/shaders_hlsl/basic.hlsl`; otherwise it exits gracefully with a message.
