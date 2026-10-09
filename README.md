# TurboGameEngine

`TurboGameEngine` now uses **Diligent Engine as the only rendering backend**.

The engine initializes one Diligent device and lets you choose the low-level API at runtime (`d3d12`, `vk`, `gl`, `d3d11`, `webgpu`, `auto`) via config.

## Understand The Code

Read the [Russian project/code walkthrough](docs/project-code-guide.md): file map,
startup and frame flow, ECS, editor, rendering, asynchronous assets, JobSystem,
Tracy, debugging checkpoints, and a suggested reading order.

[Runtime Delete and Tracy FPS troubleshooting](docs/runtime-delete.md): temporary
Play-mode deletion, safe job boundaries, Stop restore, and profiling-build caveats.

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

Clone with the Diligent submodule and its nested dependencies:

```bash
git clone --recurse-submodules https://github.com/TurbikXD/TurboGameEngine.git
cd TurboGameEngine
```

For an existing checkout, run `git submodule update --init --recursive` first.
The supplied presets target Windows / Visual Studio 2022 and use `C:/tge` build
directories. Lab scripts additionally require Node.js and the matching Tracy
0.13.1 capture / CSV-export tools (see `tools/lab1/README.md`).

```bash
cmake --preset debug --fresh
cmake --build --preset debug
```

Release:

```bash
cmake --preset release --fresh
cmake --build --preset release
```

## Lab 1: Job System And Tracy

Start with the [Lab 1 defense guide](docs/lab1/README.md) for build/test commands,
live scenes, the three-minute demo, architecture, and questions.
The verified package includes [15 measured runs](docs/lab1/results.md), 4/4 CTest
and 6/6 stress scenarios, plus the [seven-slide presentation](reports/lab1-defense/TurboGameEngine-Lab1-final.pptx).

The published evidence is under `reports/lab1/release-verified`: all 15 `.tracy`
captures, original CSV exports, metadata, and calculated results. Validation,
stress logs, and the final presentation are included separately. These are the
24 September 2026 measurements, not a new benchmark made while publishing.
Generated fixtures, executables, build trees, and failed diagnostic runs are
not included; regenerate the fixtures with the supplied script.

Launch the prepared visible loading demo from the repository root:

```powershell
.\tools\lab1\Launch-Lab1Demo.ps1 -Scene loading -Mode after
```

Use `-Scene ecs` for transform preparation, `-Mode before` for its serial
comparison, or add `-Tracy` to connect the matching profiler GUI.

`Application` owns one enkiTS-backed `core::JobSystem`. It serves asynchronous
texture/model loading and existing ECS render-transform preparation. Physics
integration and collision proxies also use it. The former separate asset
worker pool has been removed. Asset CPU work runs on `Job Worker N`; GPU
finalization stays on the main thread with a count/time-limited pump.

ECS entities, component storage and queries use EnTT through `engine_ecs`.
See [the ECS threading contract](docs/ecs-entt.md) for versioned handles,
stable phases and `World::forEachParallel` with the shared JobSystem.

The lab preset uses optimized **Release with debug symbols**, Tracy 0.13.1,
1280x720 and VSync off:

```powershell
cmake --preset profile-release
cmake --build --preset profile-release --target app engine_job_system_tests engine_async_load_tests engine_transform_batch_tests engine_physics_tests
ctest --test-dir C:/tge/profile -C Release --output-on-failure
.\tools\lab1\New-BenchmarkAssets.ps1
.\tools\lab1\Invoke-Lab1Measurements.ps1 -Scene all -Runs 3 -IncludeUnboundedPump
.\tools\lab1\Invoke-Lab1Stress.ps1
```

Executable: `C:/tge/profile/Release/app.exe`. The older `profile` preset remains
available for `RelWithDebInfo`, but the current lab procedure uses
`profile-release`. The [Tracy GUI 0.13.1](https://github.com/wolfpld/tracy/releases/tag/v0.13.1)
must match the client. Connections are local and profiling is on-demand.

`TGE_LAB_SCENE=loading` requests 12 PNG textures and four OBJ models during the
frame loop; `TGE_ASYNC_LOADING=0/1` controls its synchronous/async experiment.
`TGE_LAB_SCENE=ecs` creates 4096 render objects with hierarchy;
`TGE_JOBS=0/1` selects serial/parallel ECS preparation without disabling the
shared asset scheduler. Camera, input and workload are fixed for lab runs.

The runner records at least three runs per mode, excludes three seconds of
warmup, and derives frame median/p95/p99 and target-zone timing from saved
Tracy traces. Compare equal ranges relative to `LAB_RUN_START`, using
**Limit range**, **Statistics**, **Find zone**, and **Compare**. Useful zones:
`Main Frame`, `Render Prepare Transforms`, `Asset Decode Job`,
`Asset Main Thread Tasks`, `Job Dispatch`, and `Job Wait`.

The original loader was already asynchronous. The synchronous loading mode
is an explicit ablation, not a claim about the original implementation;
`legacy-pump` separately tests an effectively unbounded pump using the new
scheduler. See the [measurement protocol](tools/lab1/README.md) and
[acceptance status](docs/lab1/acceptance.md). The
[earlier physics comparison](docs/job-system-tracy-measurements.md) is retained
as historical material and is not the final Lab 1 evidence.

These are CPU zones. GPU-bound conclusions require separate GPU timing or a
controlled resolution test; worker CPU durations cannot be summed into frame
wall time.

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
- `Esc` in gameplay: release camera look or return from editor Play to Edit

The separate `PauseState` supports Escape to pop itself, but gameplay Escape
does not currently push it. See the code walkthrough for the distinction.

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
