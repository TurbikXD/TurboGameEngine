# Lab2 Linux headless memory check

Builds the repository's ScriptingSystemTests and a budget-adapted driver copy of
ReactRuntimeTests, including the real React, TweenService and CanvasGroup fixtures.
It uses existing dependency sources and never downloads packages. Luau, enkiTS,
spdlog and the engine code are compiled with GCC/Clang ASan, LSan and UBSan,
`-g -O1 -fno-omit-frame-pointer`; sanitizer recovery is disabled. Debug retains
the existing100ms callback budget. It does not build GLFW, ImGui or a GPU backend.

The React test **driver copy** sets its configured batch callback budget to1000ms
because the512-tween fixture is much slower under ASan/UBSan. Its explicit5ms
hostile-callback check remains intact. The original test file and production
budgets are unchanged. Initial unadapted watchdog failures are preserved in the
investigation logs, rather than represented as successful sanitizer runs.

Example in the already installed Ubuntu24.04 WSL distribution:

```sh
python3 /mnt/c/Users/rocco/CLionProjects/TurboGameEngine/tools/verify_lab2_memory/run.py \
  --deps /mnt/c/tge/profile/_deps \
  --build /tmp/tge-lab2-memory-20261016 \
  --output /mnt/c/Users/rocco/CLionProjects/TurboGameEngine/docs/lab2/defense/memory-linux/next-run \
  --repeat 5 --jobs 2
```

Use a new/empty output directory; the runner rejects existing evidence rather
than overwriting it. Use a fresh build directory for a separate experiment.
The runner records exact
commands, source hashes, sanitizer options, exit codes and individual logs in
`summary.json`. It runs both tests once and repeats only if both passed. A failure
stops further iterations. No suppression files are configured. Byte-identical
scripts/prefabs are copied into the Linux build directory, with hashes recorded,
to avoid charging WSL `/mnt/c` latency against the unchanged callback watchdog.
Test fixtures/logs do not replace user scenes.

After the repeated contract tests, `lab2_lifecycle_memory` runs two120-cycle
same-VM scenarios using actual Collectathon, prefabs and demo_ui. The earliest
entity's Update destroys later scripted entities and respawns them every180
simulated frames. Root/event/entity counts and VM bytes are recorded before/end
fullGC and after shutdown. The second scenario additionally collects once per
cycle to distinguish GC pacing from reachable retention. Hot reload is excluded
so it cannot obscure the destroy/respawn path.

These are Linux CPU scripting/React checks. They are not a native Windows D3D12
memory profile, a GPU leak check, a CLion Profiler capture, or proof that every
engine path is free of leaks. See the captured logs and coverage before making
claims about a particular run. The original Windows build caches remain separate.
