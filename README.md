# TurboGameEngine

`TurboGameEngine` now uses **Diligent Engine as the only rendering backend**.

The engine initializes one Diligent device and lets you choose the low-level API at runtime (`d3d12`, `vk`, `gl`, `d3d11`, `webgpu`, `auto`) via config.

## What Changed

- Legacy internal backends (`rhi_opengl`, `rhi_vulkan`, `rhi_d3d12`) are no longer used by runtime.
- `engine/rhi_diligent` contains the active implementation of `engine/rhi/*`.
- CMake enforces `ENGINE_BACKEND=diligent`.
- Diligent samples/tutorials are built from `third_party/DiligentEngine` (enabled by default).

## Build Requirements

- CMake >= 3.24
- Visual Studio 2022 (Windows)
- C++20 compiler
- Python 3

## Configure And Build

```bash
cmake --preset debug --fresh
cmake --build --preset debug
```

Release:

```bash
cmake --preset release --fresh
cmake --build --preset release
```

## Runtime Backend Selection (Inside Diligent)

Use `config.json` in project root (working directory):

```json
{
  "width": 1280,
  "height": 720,
  "title": "TurboGameEngine",
  "vsync": true,
  "diligentDevice": "d3d12",
  "clearColor": [0.1, 0.1, 0.16, 1.0],
  "initialState": "menu"
}
```

`diligentDevice` values:

- `auto` (default auto-pick)
- `d3d12`
- `vk`
- `gl`
- `d3d11`
- `webgpu`

Notes:

- `gl` requires OpenGL window/context creation (handled automatically by app config path).
- If selected API is unavailable in current build/platform, device creation fails with log error.

## Run App

- Debug: `C:/tge/dbg/Debug/app.exe`
- Release: `C:/tge/rel/Release/app.exe`

Menu controls in `app`:

- `Enter`: start gameplay
- `Esc`: pause/resume from gameplay

Built-in ImGui workspace panel (inside the same `app`) lets you switch runtime views:

- `Engine States`
- `Diligent Tutorial01: Hello Triangle` (in-app)
- `Diligent Tutorial03: Texturing` (in-app)
- `Diligent Sample: ImGui Demo` (in-app)
- `Diligent Samples Hub (all demos)`:
  discovers demos from `third_party/DiligentEngine/DiligentSamples`,
  shows built/missing status, and launches selected demo with chosen `--mode`.

## Build And Run Diligent Samples

Standalone Diligent samples are still available when `ENGINE_BUILD_DILIGENT_SAMPLES=ON` (default), but they are optional for daily use because the main workflows can now run from a single `app`.

You can choose sample runtime mode through CMake cache:

```bash
cmake --preset debug --fresh -DDILIGENT_SAMPLE_MODE=d3d12
cmake --build --preset debug --target run_diligent_glfw_demo
```

Available helper targets:

- `run_diligent_glfw_demo`
- `run_diligent_tutorial01`
- `run_diligent_imgui_demo`
- `run_diligent_tutorial03`
- `build_diligent_all_samples` (build every available Diligent demo/tutorial target)
- `build_diligent_app_samples` (compatibility alias to `build_diligent_all_samples`)

Example for Vulkan mode:

```bash
cmake --preset debug --fresh -DDILIGENT_SAMPLE_MODE=vk
cmake --build --preset debug --target run_diligent_tutorial01
```

## Project Structure

- `engine/rhi`: RHI interfaces
- `engine/rhi_diligent`: active Diligent-based implementation
- `engine/renderer`: renderer on top of RHI
- `engine/core`, `engine/platform`, `engine/game`, `engine/ecs`, `engine/resources`: engine/gameplay layers
- `third_party/DiligentEngine`: embedded Diligent Engine tree and samples

## Logging

- Console logger
- Rotating file logger: `logs/engine.log`
