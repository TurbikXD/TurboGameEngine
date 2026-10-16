"""Exercise an owned Collectathon/React project and record Windows process counters.

This is a soak test, not a native heap leak detector. It never changes user projects.
The fixture and raw evidence remain in the requested output directory.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("page_faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
            "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_bytes")]


def create_fixture(root: Path) -> Path:
    assets = root / "assets"
    (assets / "scripts/ui").mkdir(parents=True)
    (assets / "prefabs").mkdir()
    for name in ("Collectathon.luau", "Collectible.luau"):
        shutil.copy2(REPO / "assets/scripts" / name, assets / "scripts" / name)
    shutil.copy2(REPO / "assets/scripts/ui/demo_ui.luau", assets / "scripts/ui/demo_ui.luau")
    for name in ("collectathon_controller.json", "collectathon_player.json", "collectible.json", "react_ui.json"):
        value = json.loads((REPO / "assets/prefabs" / name).read_text(encoding="utf-8-sig"))
        value.get("components", {}).get("meshRenderer", {}).pop("textureId", None)
        (assets / "prefabs" / name).write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
    driver = '''local B = {}
function B:Spawn()
    Engine.spawnPrefab("prefabs/collectathon_controller.json")
    Engine.spawnPrefab("prefabs/react_ui.json")
end
function B:Init()
    self.state.elapsed = 0
    self.state.cycles = 0
    self.state.reloads = 0
    self:Spawn()
end
function B:Update(dt)
    Engine.setStatus("soakCycles", self.state.cycles)
    Engine.setStatus("soakReloads", self.state.reloads)
    self.state.elapsed += dt
    if self.state.elapsed < 3 then return end
    self.state.elapsed = 0
    for _, tag in {"ScriptToken", "ScriptPlayer", "Collectathon Controller", "ReactLuauHUD"} do
        for _, id in Engine.find(tag) do Engine.destroy(id) end
    end
    self.state.cycles += 1
    self:Spawn()
    print("SOAK_CYCLE " .. self.state.cycles)
end
function B:OnReload()
    self.state.reloads += 1
    print("SOAK_RELOAD " .. self.state.reloads)
end
return B
'''
    (assets / "scripts/SoakDriver.luau").write_text(driver, encoding="utf-8")
    scene = {"version": 1, "cameraEntity": 1, "entities": [
        {"sourceId": 1, "tag": {"value": "Camera"}, "transform": {
            "position": [0, 17, -17], "rotationEulerRadians": [-0.55, 0, 0], "scale": [1, 1, 1]},
         "camera": {"active": True, "farPlane": 200}},
        {"sourceId": 2, "tag": {"value": "Soak Driver"}, "script": {
            "path": "scripts/SoakDriver.luau", "enabled": True, "fields": {}}},
        {"sourceId": 3, "tag": {"value": "Ground"}, "transform": {
            "position": [0, -2.5, 8], "rotationEulerRadians": [0, 0, 0], "scale": [28, 1, 20]},
         "meshRenderer": {"meshId": "__diligent_cube__", "shaderId": "assets/shaders_hlsl/textured.shader.json",
                          "tint": [0.18, 0.24, 0.3, 1], "visible": True}}
    ]}
    (root / "scene.json").write_text(json.dumps(scene), encoding="utf-8")
    manifest = {"version": 1, "name": "Lab2 Memory Soak", "root": str(root), "assets": str(assets),
                "liveScene": "", "width": 1280, "height": 720, "vsync": True, "device": "d3d12"}
    (root / "play.json").write_text(json.dumps(manifest), encoding="utf-8")
    return assets / "scripts/SoakDriver.luau"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=300)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows native process counters are required")
    if args.seconds < 30:
        parser.error("Use at least 30 seconds")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    project = output / "fixture"
    project.mkdir()
    driver = create_fixture(project)
    original = driver.read_text(encoding="utf-8")
    environment = dict(os.environ, TGE_DEMO_SECONDS=str(args.seconds), TGE_LAB_SCENE="", TGE_FRAME_DIAGNOSTICS="1",
                       TGE_STRESS_SCENE="0", TGE_WAIT_FOR_TRACY="0")
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
    kernel.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    started = datetime.now(timezone.utc).isoformat()
    samples = []
    with (output / "runtime.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(args.app.resolve()), "--play-session", str(project / "play.json")],
                                   cwd=project, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        handle = kernel.OpenProcess(0x0410, False, process.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        begin = time.monotonic()
        next_reload = 10
        reload_requests = 0
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - begin
                if elapsed >= next_reload and elapsed < args.seconds - 5:
                    reload_requests += 1
                    driver.write_text(original + f"\n-- reload generation {reload_requests}\n", encoding="utf-8")
                    next_reload += 10
                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                handles = wintypes.DWORD()
                if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    kernel.GetProcessHandleCount(handle, ctypes.byref(handles))
                    samples.append({"elapsed_seconds": round(elapsed, 3), "private_bytes": counters.private_bytes,
                                    "working_set_bytes": counters.working_set, "handles": handles.value})
                # No process termination: the application's own demo timer closes this owned fixture.
                time.sleep(1)
        finally:
            kernel.CloseHandle(handle)
        code = process.wait()
        elapsed = time.monotonic() - begin
    with (output / "samples.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["elapsed_seconds", "private_bytes", "working_set_bytes", "handles"])
        writer.writeheader()
        writer.writerows(samples)
    warm = [s for s in samples if s["elapsed_seconds"] >= min(60, args.seconds / 4)]
    slope = None
    if len(warm) > 2:
        xmean = statistics.mean(s["elapsed_seconds"] for s in warm)
        ymean = statistics.mean(s["private_bytes"] for s in warm)
        slope = sum((s["elapsed_seconds"] - xmean) * (s["private_bytes"] - ymean) for s in warm) / sum(
            (s["elapsed_seconds"] - xmean) ** 2 for s in warm) * 60 / 1048576
    runtime = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
    summary = {"startedUtc": started, "app": str(args.app.resolve()), "appSha256": hashlib.sha256(args.app.read_bytes()).hexdigest(),
               "configuration": "Release / D3D12 / 1280x720 / vsync", "requestedSeconds": args.seconds,
               "elapsedSeconds": round(elapsed, 3), "exitCode": code, "samples": len(samples),
               "spawnResetIntervalSeconds": 3, "reloadRequests": reload_requests,
               "appliedReloadBatches": runtime.count("Luau live: Reloaded scripts/SoakDriver.luau"),
               "scriptErrorLines": runtime.count("[error] [engine] Luau:"),
               "outOfMemoryErrors": runtime.count("Luau: not enough memory"),
               "scriptPrintCounters": "Print is retained in the editor Output, not stdout; no cycle/callback counts are inferred from it.",
               "postWarmupPrivateMiBPerMinute": slope,
               "postWarmupPrivateMiBRange": [min(s["private_bytes"] for s in warm) / 1048576,
                                              max(s["private_bytes"] for s in warm) / 1048576] if warm else None,
               "handleRange": [min(s["handles"] for s in warm), max(s["handles"] for s in warm)] if warm else None,
               "complete": code == 0 and elapsed >= args.seconds - 3 and len(samples) >= args.seconds - 5,
               "limits": "Windows process counters do not identify native/GPU leaks. A bounded run cannot prove that all engine paths are leak-free."}
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["complete"] and summary["scriptErrorLines"] == 0 and summary["appliedReloadBatches"] > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
