# Verification — 24 September 2026

- `cmake --build --preset profile-release --target app engine_physics_tests engine_transform_batch_tests engine_async_load_tests -- /m:4`: successful. `engine_job_system_tests` was built in the preceding core validation. The final app was rebuilt after the D3D12 descriptor-capacity correction.
- `ctest --test-dir C:/tge/profile -C Release --output-on-failure`: 4/4 passed; final repeat 5.78 seconds. See `ctest-release.log`. Checks are explicit in Release, not disabled `assert` statements.
- `Invoke-Lab1Measurements.ps1 -Scene all -Runs 3 -IncludeUnboundedPump -OutputDirectory reports/lab1/release-verified`: all 15 app/capture pairs exited 0; full ranges, load completion and frame progress verified. See adjacent `release-verified/runs.json` and `summary.json`.
- `Invoke-Lab1Stress.ps1 -OutputDirectory reports/lab1/stress-final`: 6/6 passed. Missing resources, exit with 16 pending CPU requests, 60-second session, three repeated process starts. This is bounded testing, not a proof of absence of all races.
- Visible loading demo: verified real engine window, 16 scene objects, 4 workers, Ready=18, Failed=0; screenshot `reports/lab1-defense/live-loading.png`. Two Ready resources belong to initial scene setup.
- Presentation: `TurboGameEngine-Lab1-final.pptx`, 7 slides, 5 native editable tables. Package integrity, layout, fonts and reimport passed. Every slide rendered and visually inspected; revised slides rechecked. Native PowerPoint/Google Slides round-trip was not performed.
- Tracy GUI was launched with the saved `ecs-after-03` trace, but further UI inspection stopped when Computer Use received physical Escape. No subsequent UI actions were performed. CLI capture/export and data validation had already completed successfully.
- First failed diagnostic capture remains under `reports/lab1/release-final`; it is excluded. LLDB identified exhausted default D3D12 dynamic descriptors (8192). Final common capacity 262144; measured scene peak 8704. Both A/B modes use the same final binary.
- No ThreadSanitizer or formal read/write race checker claimed. GPU/display latency is not measured by these CPU zones. Existing user applications were not closed; see `docs/lab1/results.md` for measurement limitations.
- External instructor agreement in MM is still required. A proposed message is in `docs/lab1/acceptance.md`; nothing was sent externally.

App SHA-256: `187B71457DA4B5F41401ED806D7A58A335A8F29F7D742298F8510E95BD59DB20`.

Final presentation SHA-256: `62bd7d18c46f5ddaca78769784a11b2df6b6a7c36ec428d4541a7e02ed7324c4`.
