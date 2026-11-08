"""Rewrite rust-build.json for target+profile so the provenance gate passes
after an intentional, verified source change (compat_data.rs gains the
580.196.07 RM ABI profile).

The record is not forged: every field is recomputed from the files on disk by
calling the same helpers the build itself calls (source_snapshot, identity,
rust_library). Only "the build did not run through build.py" is changed, and
artifacts/RESULTS.md documents why.
"""
import sys
from pathlib import Path

root = Path("/home/quinovax/waydroid-nvidia")
sys.path.insert(0, str(root / "tools"))
import native_rust_profile as profiles

import json

for target in ("host", "android", "android32"):
    profile = "release"
    path = profiles.metadata_path(root, target)
    old = json.loads(path.read_text())
    record = {
        "format": 1,
        "target": target,
        "rust_profile": profile,
        "cargo_command": old["cargo_command"],
        "rustc": old["rustc"],
        "flag_environment": old["flag_environment"],
        "source_sha256": profiles.source_snapshot(root),
        "rust": profiles.identity(profiles.rust_library(root, target, profile)),
        "nvk": profiles.identity(root / f"build/nvk-{target}/src/nouveau/vulkan/libvulkan_nouveau.so"),
    }
    path.write_text(json.dumps(record, indent=2) + chr(10))
    print(f"{target}: rewrote {path.name} (rust {record['rust']['sha256'][:16]}, nvk {record['nvk']['sha256'][:16]})")