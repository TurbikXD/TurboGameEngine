"""Exercise unmodified vendored React with a standalone Luau CLI.

Usage: python tools/test_react_luau.py /path/to/luau.exe
The compiled engine integration has its own C++ test of the same contract.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "third_party/react-luau"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("luau", type=Path)
    args = parser.parse_args()
    modules = dict(line.split("\t") for line in (PACKAGE / "manifest.tsv").read_text().splitlines())
    nodes = set(modules)
    for name in tuple(nodes):
        parts = name.split("/")
        nodes.update("/".join(parts[:count]) for count in range(1, len(parts)))
    nodes.update({"SafeFlags", "ReactRoblox"})
    output = ["local __reactModuleTree = {}", "local loaders, cache = {}, {}", "local require",
              "local packages = {}; packages.Parent = packages",
              "function packages:FindFirstChild(name) return self[name] end",
              "function packages:GetFullName() return 'Packages' end"]
    for name in sorted(nodes):
        output.append(f"__reactModuleTree[{json.dumps(name)}] = {{}}")
    for name in sorted(nodes):
        parent, _, leaf = name.rpartition("/")
        reference = f"__reactModuleTree[{json.dumps(name)}]"
        parent_reference = f"__reactModuleTree[{json.dumps(parent)}]" if parent else "packages"
        output.append(f"{reference}.Parent = {parent_reference}; {parent_reference}[{json.dumps(leaf)}] = {reference}")
        if name in modules or name in {"SafeFlags", "ReactRoblox"}:
            output.append(f"{reference}.__module = {json.dumps(name)}")
    output.append("require = function(name) if type(name) == 'table' then name = name.__module end; if cache[name] ~= nil then return cache[name] end; assert(loaders[name], tostring(name)); local result = loaders[name](); cache[name] = result; return result end")
    output.append((PACKAGE / "adapters/Compatibility.luau").read_text())
    output.append((PACKAGE / "adapters/RobloxUI.luau").read_text())
    output.append((PACKAGE / "adapters/TweenService.luau").read_text())
    for name, path in sorted(modules.items()):
        task_alias = ", task = __turboSchedulerTask" if name == "Scheduler" or name.startswith("Scheduler/") else ""
        output.append(f"loaders[{json.dumps(name)}] = setfenv(function()\n{(PACKAGE / path).read_text()}\nend, setmetatable({{script = __reactModuleTree[{json.dumps(name)}]{task_alias}}}, {{__index = getfenv()}}))")
    output.append("loaders.SafeFlags = function() return __turboSafeFlags end")
    output.append("loaders.ReactTurbo = function()\n" + (PACKAGE / "adapters/ReactTurbo.luau").read_text() + "\nend")
    output.append("loaders.ReactRoblox = function() return require('ReactTurbo').ReactRoblox end")
    for fixture in ("ReactRuntimeTests.luau", "TweenServiceTests.luau", "CanvasGroupTests.luau"):
        output.append("do\n" + (ROOT / "tests" / fixture).read_text() + "\nend")
    destination = ROOT / "build/react-check/contract.luau"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(output), encoding="utf-8")
    subprocess.run([str(args.luau.resolve()), str(destination)], check=True)


if __name__ == "__main__":
    main()
