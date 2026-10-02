# Job system: Tracy A/B measurement

## Setup

- Build: `RelWithDebInfo`, Tracy 0.13.1, VSync disabled, 1280x720.
- Scene: deterministic profile scene with 1280 additional dynamic bodies; no user input.
- Warm-up: 3 seconds before capture.
- Capture: 12 seconds per run.
- Statistics limit range: 4.0-14.0 seconds from process start (the same 10-second interval in both traces).
- Before: `TGE_JOBS=0` (serial fallback through the same code path).
- After: `TGE_JOBS=1` (four permanent workers plus the main thread participating in each batch).

Local traces:

- `reports/tracy_job_system_before.tracy`
- `reports/tracy_job_system_after.tracy`

Open the first trace in Tracy, choose **Compare -> Open second trace**, set the
limit range above, and search for the two target zones.

## Results

| Tracy zone | Before mean | After mean | Mean improvement | Before median | After median | Median improvement |
|---|---:|---:|---:|---:|---:|---:|
| `Physics Integrate Bodies` | 0.640 ms | 0.508 ms | 20.6% | 0.540 ms | 0.484 ms | 10.3% |
| `Physics Build Body Proxies` | 0.630 ms | 0.563 ms | 10.7% | 0.580 ms | 0.535 ms | 7.8% |
| `Main Frame` | 11.03 ms | 10.99 ms | 0.4% | 13.37 ms | 13.28 ms | 0.6% |

The two jobified phases together fell from about 1.27 ms to 1.07 ms per fixed
update (about 15.7%). Their worker slices are visible on the `Job Worker 0-3`
threads as `Job Physics Integrate` and `Job Physics Build Proxies`.

The total frame gain is intentionally smaller: serial broadphase and
narrowphase/solver still dominate the physics update. This is the next
bottleneck visible in the same traces, not an estimate based on FPS alone.

## Reproduction

```powershell
cmake --preset profile --fresh
cmake --build --preset profile --target app

$env:TGE_JOBS = '0' # before
C:\tge\profile\RelWithDebInfo\app.exe

$env:TGE_JOBS = '1' # after
C:\tge\profile\RelWithDebInfo\app.exe
```

For each run, connect the matching Tracy 0.13.1 GUI after the three-second
warm-up and save a 12-second trace.
