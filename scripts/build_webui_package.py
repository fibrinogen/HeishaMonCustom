#!/usr/bin/env python3
"""Build the SD-hosted HeishaMon Custom Web UI package.

The resulting uncompressed USTAR archive is intentionally simple so the
ESP32 can validate and unpack it while streaming the upload.
"""

from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import tarfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "webui" / "src"
DIST = ROOT / "webui" / "dist"
PACKAGE = DIST / "heishamon-webui.tar"


def macro_value(name: str) -> str:
    text = (ROOT / "HeishaMon" / "custom_version.h").read_text(encoding="utf-8")
    match = re.search(rf'^#define\s+{re.escape(name)}\s+"([^"]+)"', text, re.MULTILINE)
    if not match:
        raise RuntimeError(f"Missing {name} in custom_version.h")
    return match.group(1)


def render(source: Path, replacements: dict[str, str]) -> bytes:
    text = source.read_text(encoding="utf-8")
    for key, value in replacements.items():
        text = text.replace("{{" + key + "}}", value)
    unresolved = sorted(set(re.findall(r"{{[A-Z0-9_]+}}", text)))
    if unresolved:
        raise RuntimeError(f"Unresolved placeholders in {source.name}: {unresolved}")
    return text.encode("utf-8")


def main() -> None:
    custom_version = macro_value("CUSTOM_FEATURES_VERSION")
    base_version = macro_value("HEISHAMON_BASE_VERSION")
    webui_version = (ROOT / "webui" / "version.txt").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z._-]{0,30}", webui_version):
        raise RuntimeError("webui/version.txt contains an invalid version")
    replacements = {
        "CUSTOM_VERSION": custom_version,
        "BASE_VERSION": base_version,
        "WEBUI_VERSION": webui_version,
    }

    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir(parents=True)

    payloads: dict[str, bytes] = {}
    for source in sorted(SOURCE.iterdir()):
        if not source.is_file():
            continue
        output_name = source.name[:-3] if source.name.endswith(".in") else source.name
        payloads[output_name] = render(source, replacements)

    manifest_files = []
    for name, data in sorted(payloads.items()):
        manifest_files.append({
            "path": name,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    manifest = {
        "format": 1,
        "version": webui_version,
        "customVersion": custom_version,
        "baseVersion": base_version,
        "files": manifest_files,
    }
    payloads["manifest.json"] = (json.dumps(manifest, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")

    for name, data in payloads.items():
        (DIST / name).write_bytes(data)

    with tarfile.open(PACKAGE, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in sorted(payloads.items()):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            archive.addfile(info, io.BytesIO(data))

    print(f"Built {PACKAGE.relative_to(ROOT)}")
    print(f"Web UI version: {webui_version}; files: {len(payloads)}; package: {PACKAGE.stat().st_size} bytes")


if __name__ == "__main__":
    main()
