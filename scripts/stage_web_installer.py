#!/usr/bin/env python3
"""Stage the ESP32-S3 GitHub Pages installer from ESP-IDF flash metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def project_version(build_dir: Path, root: Path) -> str:
    try:
        description = json.loads(
            (build_dir / "project_description.json").read_text(encoding="utf-8")
        )
        return str(description["project_version"])
    except (OSError, KeyError, json.JSONDecodeError):
        try:
            return subprocess.check_output(
                ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
                text=True,
                stderr=subprocess.DEVNULL,
            ).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return "0.1.0"


def main() -> None:
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    flash_args_path = args.build / "flasher_args.json"
    if not flash_args_path.is_file():
        raise SystemExit(f"missing {flash_args_path}; run make build first")

    flash_args = json.loads(flash_args_path.read_text(encoding="utf-8"))
    built_target = flash_args.get("extra_esptool_args", {}).get("chip")
    if built_target and built_target != "esp32s3":
        raise SystemExit(f"expected esp32s3 build, got {built_target}")
    flash_files = flash_args.get("flash_files", {})
    if not flash_files:
        raise SystemExit(f"{flash_args_path} contains no flash files")

    if args.output_dir.exists():
        shutil.rmtree(args.output_dir)
    firmware_dir = args.output_dir / "firmware" / "esp32s3"
    firmware_dir.mkdir(parents=True)
    shutil.copy2(root / "web-installer" / "index.html", args.output_dir / "index.html")
    shutil.copy2(root / "web-installer" / "favicon.svg", args.output_dir / "favicon.svg")

    parts = []
    for offset, relative_name in flash_files.items():
        source = args.build / relative_name
        if not source.is_file():
            raise SystemExit(f"missing build artifact: {source}")
        destination_name = relative_name.replace("/", "-")
        shutil.copy2(source, firmware_dir / destination_name)
        parts.append(
            {
                "path": f"./firmware/esp32s3/{destination_name}",
                "offset": int(offset, 0),
            }
        )
    parts.sort(key=lambda part: part["offset"])

    version = project_version(args.build, root)
    manifest = {
        "name": "espDrop",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    app_file = flash_args.get("app", {}).get("file")
    build_info = {
        "project": "espDrop",
        "version": version,
        "builtAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "chipFamilies": ["ESP32-S3"],
        "protocolStatus": "research scaffold",
    }
    if app_file:
        app_path = args.build / app_file
        build_info["app"] = {
            "size": app_path.stat().st_size,
            "sha256": hashlib.sha256(app_path.read_bytes()).hexdigest(),
        }
    (args.output_dir / "build-info.json").write_text(
        json.dumps(build_info, indent=2) + "\n", encoding="utf-8"
    )
    (args.output_dir / ".nojekyll").touch()
    print(f"staged espDrop ESP32-S3 installer in {args.output_dir}")


if __name__ == "__main__":
    main()
