from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess

DEMO = Path(__file__).resolve().parent
ROOT = DEMO.parents[1]
PACKAGE = ROOT / "third_party/react-luau"


def ensure(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_project() -> dict:
    manifest = json.loads((DEMO / "Lab3Materials.tgeproj").read_text(encoding="utf-8"))
    ensure(manifest["version"] == 1, "Unsupported project version")
    name = manifest.get("name")
    ensure(isinstance(name, str) and 1 <= len(name.encode("utf-8")) <= 80
           and not any(character in '<>:"/\\|?*' or ord(character) < 32 or ord(character) == 127 for character in name)
           and name == name.strip() and not name.endswith("."),
           "Project name violates ProjectManager manifest rules")
    assets = (DEMO / manifest["assets"]).resolve()
    ensure(assets.is_relative_to(DEMO) and assets.is_dir(), "Invalid asset root")

    def asset(relative: str) -> Path:
        ensure(isinstance(relative, str) and relative and "\0" not in relative, "Invalid asset reference")
        path = (assets / relative.removeprefix("assets/")).resolve()
        ensure(path.is_relative_to(assets) and path.is_file(), "Missing/escaping asset: " + relative)
        return path

    scene_path = (DEMO / manifest["startupScene"]).resolve()
    ensure(scene_path.is_relative_to(assets), "Scene escapes assets")
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    ids = [item["sourceId"] for item in scene["entities"]]
    ensure(len(ids) == len(set(ids)) and all(value > 0 for value in ids), "Invalid source IDs")
    ensure(scene["cameraEntity"] in ids and scene["selectedEntity"] in ids, "Invalid editor selection/camera")
    controllers = [item["script"] for item in scene["entities"]
                   if item.get("script", {}).get("path") == "scripts/MaterialLab.luau"]
    ensure(len(controllers) == 1 and controllers[0].get("enabled", True), "Expected one enabled playable controller")
    ensure(any("А" <= character <= "я" or character in "Ёё" for character in controllers[0].get("fields", {}).get("title", "")),
           "Authored demo title must be in Russian")
    materials = {}

    def material(relative: str, chain: tuple = ()) -> dict:
        path = asset(relative)
        ensure(path not in chain and len(chain) < 16, "Material cycle/depth")
        raw = json.loads(path.read_text(encoding="utf-8"))
        ensure(isinstance(raw, dict), "Material must be a data object: " + relative)
        known = {"version", "name", "base", "shader", "texture", "tint", "uvScale", "blend", "depthTest", "depthWrite",
                 "graph", "graphTextures"}
        ensure(set(raw) <= known, "Unknown material keys: " + relative)
        ensure(raw.get("version", 1) in {1, 2}, "Unsupported material version: " + relative)
        if "base" in raw:
            ensure(isinstance(raw["base"], str) and raw["base"], "Invalid material base: " + relative)
        value = material(raw["base"], (*chain, path)) if "base" in raw else {
            "tint": [1, 1, 1, 1], "uvScale": [1, 1], "blend": "opaque", "depthTest": True, "depthWrite": True,
        }
        value = {**value, **raw}
        ensure(isinstance(value["blend"], str) and value["blend"] in {"opaque", "alpha", "additive"}, "Invalid material blend")
        ensure(all(type(value[key]) is bool for key in ("depthTest", "depthWrite")), "Invalid material depth state")
        if "graph" in value:
            graph = value["graph"]
            ensure(isinstance(graph, dict) and isinstance(graph.get("nodes"), list), "Invalid material graph: " + relative)
            nodes = {}
            for node in graph["nodes"]:
                ensure(isinstance(node, dict) and type(node.get("id")) is int and node["id"] > 0,
                       "Invalid material graph node: " + relative)
                ensure(str(node["id"]) not in nodes, "Duplicate graph node ID: " + relative)
                nodes[str(node["id"])] = dict(node)
            overrides = raw.get("graphTextures", {})
            ensure(isinstance(overrides, dict), "Invalid graph texture overrides: " + relative)
            for node_id, texture in overrides.items():
                ensure(node_id in nodes and nodes[node_id].get("type") == "texture",
                       "Graph override requires a texture node: " + relative)
                nodes[node_id]["texture"] = texture
            for node in nodes.values():
                if node.get("type") == "texture":
                    texture = node.get("texture", "")
                    ensure(isinstance(texture, str), "Graph texture must be a path: " + relative)
                    if texture:
                        asset(texture)
            value["graph"] = {**graph, "nodes": list(nodes.values())}
        else:
            ensure("graphTextures" not in raw, "Graph texture overrides require a graph: " + relative)
            asset(value["shader"])
            if value.get("texture"):
                asset(value["texture"])
        for key, length in (("tint", 4), ("uvScale", 2)):
            if key in value:
                ensure(isinstance(value[key], list) and len(value[key]) == length
                       and all(type(n) in {int, float} and math.isfinite(n) and abs(n) <= 1000000 for n in value[key]),
                       "Invalid " + key)
        materials[relative] = value
        return value

    prefabs = {path.stem: json.loads(path.read_text(encoding="utf-8"))["components"]
               for path in sorted((assets / "prefabs").glob("*.json"))}
    ensure({"jade", "amber", "violet"} <= prefabs.keys(), "Missing required material spawn choices")
    for item in [*scene["entities"], *prefabs.values()]:
        if "script" in item:
            asset(item["script"]["path"])
            ensure(isinstance(item["script"].get("fields", {}), dict), "Script fields must be data object")
        if "meshRenderer" in item:
            mesh = item["meshRenderer"]
            material(mesh["materialId"])
            ensure(mesh["meshId"] in {"__diligent_cube__", "__diligent_sphere__"}, "Unknown built-in mesh")
    shader = json.loads(asset("shaders_hlsl/textured.shader.json").read_text(encoding="utf-8"))
    asset(shader["vertex"])
    asset(shader["fragment"])
    layout = json.loads((DEMO / ".tge/editor_layout.json").read_text(encoding="utf-8"))
    ensure(layout["visibility"]["inspector"] and layout["visibility"]["hierarchy"], "Demo must expose property workflow")
    ensure(layout.get("dockSlots", {}).get("inspector") == "right_top" and layout.get("sizing", {}).get("rightSplit", 0) >= 0.65,
           "Demo Inspector needs at least 65% of the right panel height to expose behavior fields")
    for tag in ("LayeredMaterialPreview", "LayeredMaterialReference"):
        comparison = [item.get("meshRenderer", {}) for item in scene["entities"]
                      if item.get("tag", {}).get("value") == tag]
        ensure(len(comparison) == 1 and comparison[0].get("visible", True)
               and comparison[0].get("materialId") == "materials/layered.material.json",
               "Graph comparison needs a visible object sharing Layered Tile: " + tag)
    previews = {
        "jade": ("JadePreview", "opaque", "materials/jade-gallery.json"),
        "amber": ("AmberPreview", "alpha", "materials/amber.json"),
        "violet": ("VioletPreview", "additive", "materials/violet.json"),
    }
    for sample, (tag, blend, expected_material) in previews.items():
        entities = [item for item in scene["entities"] if item.get("tag", {}).get("value") == tag]
        ensure(len(entities) == 1, "Missing/duplicate gallery preview: " + tag)
        preview = entities[0]
        ensure(preview.get("meshRenderer", {}).get("materialId") == expected_material
               and preview["meshRenderer"].get("visible", True), "Preview must display its sample material: " + tag)
        ensure(sample in prefabs and prefabs[sample].get("meshRenderer", {}).get("materialId") == expected_material
               and prefabs[sample]["meshRenderer"].get("visible", True), "Spawn choice lost its visible sample material: " + sample)
        definition = materials[expected_material]
        ensure(definition["blend"] == blend and definition["depthTest"], "Incorrect visible blend demonstration: " + sample)
        ensure(definition["depthWrite"] == (blend == "opaque"), "Incorrect sample depth-write state: " + sample)
        ensure((definition["tint"][3] == 1 if blend == "opaque" else 0 < definition["tint"][3] < 1),
               "Sample alpha does not demonstrate its blend mode: " + sample)
        motion = preview.get("script", {})
        ensure(motion.get("path") == "scripts/SampleMotion.luau" and motion.get("enabled", True),
               "Preview lost editable motion behavior: " + sample)
    ensure(json.loads(asset("materials/pedestal.json").read_text(encoding="utf-8")).get("base") == "materials/stage.json"
           and json.loads(asset("materials/stage.json").read_text(encoding="utf-8")).get("base") == "materials/base.json",
           "Missing three-level material inheritance demonstration")
    result = {"sceneEntities": len(ids), "prefabs": len(prefabs), "materials": len(materials), "blends": ["opaque", "alpha", "additive"]}
    print("Project data/path validation passed:", json.dumps(result, ensure_ascii=False))
    return result


def build_bundle() -> Path:
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
        alias = ", task = __turboSchedulerTask" if name == "Scheduler" or name.startswith("Scheduler/") else ""
        output.append(f"loaders[{json.dumps(name)}] = setfenv(function()\n{(PACKAGE / path).read_text(encoding='utf-8')}\nend, setmetatable({{script = __reactModuleTree[{json.dumps(name)}]{alias}}}, {{__index = getfenv()}}))")
    output.append("loaders.SafeFlags = function() return __turboSafeFlags end")
    output.append("loaders.ReactTurbo = function()\n" + (PACKAGE / "adapters/ReactTurbo.luau").read_text(encoding="utf-8") + "\nend")
    output.append("loaders.ReactRoblox = function() return require('ReactTurbo').ReactRoblox end")
    for path in sorted((DEMO / "assets/scripts").glob("*.luau")):
        output.append(f"loaders[{json.dumps('scripts/' + path.name)}] = function()\n{path.read_text(encoding='utf-8')}\nend")
    output.append((DEMO / "tests/smoke.luau").read_text(encoding="utf-8"))
    destination = DEMO / ".tge/check/headless.luau"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(output), encoding="utf-8")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--luau", help="Path to existing Luau CLI")
    parser.add_argument("--wsl", action="store_true", help="Use existing Luau CLI in the default WSL distribution")
    parser.add_argument("--write-only", action="store_true", help="Validate project and prepare fixture without executing Luau")
    args = parser.parse_args()
    validate_project()
    destination = build_bundle()
    if args.write_only:
        print("Prepared, not executed:", destination)
        return
    if not args.luau:
        parser.error("Use --luau for execution or --write-only for preparation")
    if args.wsl:
        path = subprocess.check_output(["wsl.exe", "--exec", "wslpath", "-u", str(destination)], text=True).strip()
        command = ["wsl.exe", "--exec", args.luau, path]
    else:
        command = [str(Path(args.luau).resolve()), str(destination)]
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
