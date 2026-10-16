"""Convert the common defense JSON to this project's local Luau data module."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

DEMO = Path(__file__).resolve().parent
ROOT = DEMO.parents[1]
SOURCE_PATHS = {
    "mission": ["demos/lab2-defense/assets/scripts/Defense.luau", "docs/lab2/README.md"],
    "boundary": ["engine/scripting/ScriptingSystem.cpp: installBindings", "third_party/react-luau/adapters/RobloxUI.luau"],
    "lifecycle": ["engine/scripting/ScriptComponent.h", "engine/scripting/ScriptingSystem.cpp: lifecycle/inspect"],
    "prefab": ["demos/lab2-defense/assets/prefabs/marker.json", "engine/scripting/ScriptingSystem.cpp: spawnPrefab/api"],
    "reload": ["demos/lab2-defense/assets/scripts/ReloadRules.luau", "engine/core/LiveFileWatcher.cpp", "engine/scripting/ScriptingSystem.cpp: pollReload"],
    "react": ["third_party/react-luau/adapters/ReactTurbo.luau", "engine/scripting/react/ReactUI.cpp"],
    "editor": ["engine/editor/ScriptWorkspace.cpp", "engine/game/GameplayState.cpp: enterPlayMode/stopPlayMode", "docs/projects.md"],
    "verification": ["AGENTS.md", "tests/ScriptingSystemTests.cpp", "tests/ReactRuntimeTests.cpp"],
}


def luau(value, depth=0):
    if value is None:
        return "nil"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return json.dumps(value, allow_nan=False)
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, (dict, list)):
        indent = "    " * (depth + 1)
        if isinstance(value, list):
            rows = [luau(item, depth + 1) for item in value]
        else:
            rows = [f"[{luau(key)}] = {luau(item, depth + 1)}" for key, item in value.items() if item is not None]
        if not rows:
            return "{}"
        return "{\n" + "\n".join(indent + row + "," for row in rows) + "\n" + "    " * depth + "}"
    raise TypeError(type(value).__name__)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", nargs="?", type=Path, default=ROOT / "docs/lab2/defense/content.json")
    parser.add_argument("--check", action="store_true", help="Verify synchronization without writing")
    args = parser.parse_args()
    document = json.loads(args.source.read_text(encoding="utf-8"))
    slides = document["slides"]
    assert len(slides) == 8 and len({slide["id"] for slide in slides}) == 8
    topics = []
    for slide in slides:
        assert slide["id"] in SOURCE_PATHS and slide.get("station") in {None, "quiz", "spawn", "reload"}
        assert isinstance(slide["title"], str) and isinstance(slide["lead"], str)
        assert all(isinstance(item, str) for item in slide["bullets"])
        topics.append({
            "id": slide["id"], "title": slide["title"], "summary": slide["lead"],
            "bullets": slide["bullets"], "station": slide.get("station"),
            "notes": slide.get("notes", []),
            "sources": list(dict.fromkeys(slide.get("sources", []) + SOURCE_PATHS[slide["id"]])),
        })
    data = {"title": document["title"], "subtitle": document["subtitle"], "date": document.get("date"),
            "topics": topics, "appendix": document.get("appendix", {})}
    text = "-- Generated from docs/lab2/defense/content.json by sync_content.py.\n"
    text += "-- UI content only; quiz rules remain local and explicit.\nlocal content = " + luau(data) + "\n"
    text += 'content.quiz = require("scripts/DefenseQuiz.luau")\nreturn content\n'
    destination = DEMO / "assets/scripts/DefenseContent.luau"
    if args.check:
        assert destination.read_text(encoding="utf-8") == text, "DefenseContent.luau differs; run sync_content.py"
        print("Common JSON and game content are synchronized.")
    else:
        destination.write_text(text, encoding="utf-8")
        print("Updated", destination)


if __name__ == "__main__":
    main()
