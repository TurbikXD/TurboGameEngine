"""Check this self-contained Collectathon project without altering engine files."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess

DEMO = Path(__file__).resolve().parent
ASSETS = DEMO / "assets"


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def local_asset(relative):
    path = (ASSETS / relative.removeprefix("assets/")).resolve()
    assert path.is_relative_to(ASSETS) and path.is_file(), path
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile", type=Path, help="Existing standalone luau-compile executable")
    args = parser.parse_args()
    manifest = read_json(DEMO / "Lab2Collectathon.tgeproj")
    assert manifest["version"] == 1 and manifest["assets"] == "assets"
    scene_path = (DEMO / manifest["startupScene"]).resolve()
    assert scene_path.is_relative_to(ASSETS)
    scene = read_json(scene_path)
    assert scene["version"] == 1
    ids = [entity["sourceId"] for entity in scene["entities"]]
    assert len(ids) == len(set(ids)) == 4 and all(source_id > 0 for source_id in ids)
    assert scene["cameraEntity"] in ids and scene["selectedEntity"] == 2
    by_tag = {entity["tag"]["value"]: entity for entity in scene["entities"]}
    assert by_tag["Camera"]["transform"]["position"] == [0, 17, -17]
    assert by_tag["Ground"]["transform"]["position"] == [0, -2.5, 8]
    assert by_tag["Collectathon Controller"]["script"]["fields"]["duration"] == 180
    prefab_paths = sorted((ASSETS / "prefabs").glob("*.json"))
    assert {path.name for path in prefab_paths} == {
        "collectathon_controller.json", "collectathon_player.json", "collectible.json", "react_ui.json"}
    components = list(scene["entities"]) + [read_json(path)["components"] for path in prefab_paths]
    for item in components:
        if "script" in item:
            assert item["script"]["path"].endswith(".luau")
            local_asset(item["script"]["path"])
        if "meshRenderer" in item:
            for key in ("shaderId", "textureId"):
                local_asset(item["meshRenderer"][key])
    shader = read_json(local_asset("shaders_hlsl/textured.shader.json"))
    for path in shader.values():
        local_asset(path)
    scripts = sorted((ASSETS / "scripts").rglob("*.luau"))
    assert len(scripts) == 3 and not (ASSETS / "scripts/SoakDriver.luau").exists()
    for script in scripts:
        source = script.read_text(encoding="utf-8-sig")
        for prefab in re.findall(r'Engine\.spawnPrefab\("([^"]+)"', source):
            local_asset(prefab)
        if args.compile:
            subprocess.run([str(args.compile.resolve()), str(script)], check=True, stdout=subprocess.DEVNULL)
    layout = read_json(DEMO / ".tge/editor_layout.json")
    assert layout["visibility"]["hierarchy"] and layout["visibility"]["inspector"]
    assert not layout["visibility"]["assistant"]
    print("PASS: manifest, 4 scene IDs, camera/ground, 4 prefabs, scripts and shader/texture paths.")
    print("Luau compile:", "PASS (3 files)." if args.compile else "NOT RUN; pass --compile.")
    print("Native GUI/ECS/live reload: NOT RUN by this verifier.")


if __name__ == "__main__":
    main()
