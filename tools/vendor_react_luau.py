"""Vendor pinned, unmodified production modules from Roblox's MIT repositories.

Run manually with Python 3. This is not a build step; checked-in sources work offline.
Engine-specific compatibility code lives in engine/scripting/react, never in upstream.
"""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "third_party" / "react-luau"
REPOSITORIES = {
    "react-luau": ("Roblox/react-luau", "5f31aba406f25b4fc28a95df73c792c6b690af38"),
    "luau-polyfill": ("Roblox/luau-polyfill", "d4798c60abe3f80219a84b32f45a7b6d6783187a"),
    "symbol-luau": ("Roblox/symbol-luau", "83d73113d2fc0af7d7d9cb3338eb8577cacc57d1"),
    "promise": ("Roblox/roblox-lua-promise", "4508fc738f73c555ceebeb89e7dff1427b8f1e8b"),
}
PACKAGES = {
    "react-luau": {"react": "React", "react-globals": "ReactGlobals", "shared": "Shared",
                   "scheduler": "Scheduler", "react-reconciler": "ReactReconciler"},
    "luau-polyfill": {"luau-polyfill": "LuauPolyfill", "boolean": "Boolean", "collections": "Collections",
                      "console": "Console", "instance-of": "InstanceOf", "math": "Math", "number": "Number",
                      "es7-types": "ES7Types", "string": "String", "timers": "Timers"},
}


def archive(repository: str, revision: str) -> zipfile.ZipFile:
    url = f"https://codeload.github.com/{repository}/zip/{revision}"
    request = urllib.request.Request(url, headers={"User-Agent": "TurboGameEngine-vendor"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return zipfile.ZipFile(io.BytesIO(response.read()))


def main() -> None:
    modules: dict[str, str] = {}
    hashes: dict[str, str] = {}
    for label, (repository, revision) in REPOSITORIES.items():
        with archive(repository, revision) as source:
            for name in source.namelist():
                relative = "/".join(name.split("/")[1:])
                if relative in ("LICENSE", "LICENSE.txt"):
                    target = DEST / "licenses" / f"{label}.txt"
                else:
                    if not relative.endswith(".lua") or "/__tests__/" in relative or "/__mocks__/" in relative:
                        continue
                    if relative.endswith((".spec.lua", "jest.config.lua")):
                        continue
                    if label in PACKAGES:
                        parts = relative.split("/")
                        if len(parts) < 4 or parts[0] != "modules" or parts[2] != "src":
                            continue
                        package = PACKAGES[label].get(parts[1])
                        if package is None:
                            continue
                        tail = "/".join(parts[3:])
                    elif label == "symbol-luau" and relative.startswith("src/"):
                        package, tail = "Symbol", relative[4:]
                    elif label == "promise" and relative == "lib/init.lua":
                        package, tail = "Promise", "init.lua"
                    else:
                        continue
                    target = DEST / "packages" / package / tail
                    module_id = f"{package}/{tail[:-4]}"
                    if module_id.endswith("/init"):
                        module_id = module_id[:-5]
                    if module_id in modules:
                        raise RuntimeError(f"Duplicate module: {module_id}")
                    modules[module_id] = str(target.relative_to(DEST)).replace("\\", "/")
                content = source.read(name)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
                hashes[str(target.relative_to(DEST)).replace("\\", "/")] = hashlib.sha256(content).hexdigest()
    DEST.mkdir(parents=True, exist_ok=True)
    (DEST / "manifest.tsv").write_text("".join(f"{key}\t{modules[key]}\n" for key in sorted(modules)), encoding="utf-8")
    (DEST / "upstream.json").write_text(json.dumps({"repositories": REPOSITORIES, "sha256": hashes}, indent=2) + "\n", encoding="utf-8")
    print(f"Vendored {len(modules)} modules ({sum((DEST / path).stat().st_size for path in modules.values()):,} bytes)")


if __name__ == "__main__":
    main()
