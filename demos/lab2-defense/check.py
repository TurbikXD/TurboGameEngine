"""Validate project files and exercise its Luau using the existing React fixture approach.

No C++ sources, build configuration, accounts or user projects are changed.
Requires an existing standalone Luau CLI; --write-only prepares the bundle.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess

DEMO = Path(__file__).resolve().parent
ROOT = DEMO.parents[1]
PACKAGE = ROOT / "third_party/react-luau"


def validate_project() -> None:
    manifest = json.loads((DEMO / "Lab2Defense.tgeproj").read_text(encoding="utf-8"))
    assert manifest["version"] == 1
    assets = (DEMO / manifest["assets"]).resolve()
    assert assets.is_relative_to(DEMO) and assets.is_dir()
    scene_path = (DEMO / manifest["startupScene"]).resolve()
    assert scene_path.is_relative_to(assets)
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    assert scene["version"] == 1 and len(scene["entities"]) >= 2
    ids = [entity["sourceId"] for entity in scene["entities"]]
    assert len(set(ids)) == len(ids) and all(entity_id > 0 for entity_id in ids)
    assert scene["cameraEntity"] in ids and scene["selectedEntity"] in ids
    prefab = json.loads((assets / "prefabs/marker.json").read_text(encoding="utf-8"))
    assert prefab["components"]["tag"] == "Lab2Marker"
    components = [entity for entity in scene["entities"]] + [prefab["components"]]
    for entity in components:
        if "script" in entity:
            path = (assets / entity["script"]["path"]).resolve()
            assert path.is_relative_to(assets) and path.is_file(), path
        if "meshRenderer" in entity:
            for key in ("textureId", "shaderId"):
                relative = entity["meshRenderer"][key].removeprefix("assets/")
                path = (assets / relative).resolve()
                assert path.is_relative_to(assets) and path.is_file(), path
    layout = json.loads((DEMO / ".tge/editor_layout.json").read_text(encoding="utf-8"))
    assert layout["visibility"]["viewport"] and not layout["visibility"]["assistant"]
    print("Project manifest, unique scene IDs, prefab and project-local paths passed.")


def build_bundle() -> Path:
    # Same loader topology as tools/test_react_luau.py, with this project's
    # application fixture instead of editing the repository's test files.
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
    for adapter in ("Compatibility.luau", "RobloxUI.luau", "TweenService.luau"):
        output.append((PACKAGE / "adapters" / adapter).read_text(encoding="utf-8"))
    for name, path in sorted(modules.items()):
        task_alias = ", task = __turboSchedulerTask" if name == "Scheduler" or name.startswith("Scheduler/") else ""
        output.append(f"loaders[{json.dumps(name)}] = setfenv(function()\n{(PACKAGE / path).read_text(encoding='utf-8')}\nend, setmetatable({{script = __reactModuleTree[{json.dumps(name)}]{task_alias}}}, {{__index = getfenv()}}))")
    output.append("loaders.SafeFlags = function() return __turboSafeFlags end")
    output.append("loaders.ReactTurbo = function()\n" + (PACKAGE / "adapters/ReactTurbo.luau").read_text(encoding="utf-8") + "\nend")
    output.append("loaders.ReactRoblox = function() return require('ReactTurbo').ReactRoblox end")
    for path in sorted((DEMO / "assets/scripts").glob("*.luau")):
        # The native project loader resolves exact paths, including extension.
        name = "scripts/" + path.name
        output.append(f"loaders[{json.dumps(name)}] = function()\n{path.read_text(encoding='utf-8')}\nend")
    output.append((DEMO / "tests/smoke.luau").read_text(encoding="utf-8"))
    destination = DEMO / ".tge/check/headless.luau"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(output), encoding="utf-8")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--luau", help="Existing native Luau CLI executable or Linux path with --wsl")
    parser.add_argument("--wsl", action="store_true", help="Run the provided Linux CLI through existing WSL Ubuntu")
    parser.add_argument("--write-only", action="store_true", help="Validate JSON and prepare a bundle without claiming Luau execution")
    args = parser.parse_args()
    validate_project()
    destination = build_bundle()
    if args.write_only:
        print("Prepared (NOT executed):", destination)
        return
    if not args.luau:
        parser.error("--luau is required for execution; use --write-only to prepare only")
    if args.wsl:
        linux_path = subprocess.check_output(["wsl.exe", "-d", "Ubuntu", "wslpath", "-u", str(destination)], text=True).strip()
        command = ["wsl.exe", "-d", "Ubuntu", args.luau, linux_path]
    else:
        command = [str(Path(args.luau).resolve()), str(destination)]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
