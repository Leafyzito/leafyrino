# Writes version.json, which the in-app updater reads from the nightly release
# (see src/singletons/Updates.cpp).
import argparse
import hashlib
import json
import os
import subprocess


ROOT_DOWNLOAD_URL = (
    "https://github.com/leafyzito/leafyrino/releases/download/nightly-build"
)
# keep in sync with build-installer.ps1 and bundle-portable.ps1
WINDOWS_FILES = {
    "x86-64": {
        "installer": "Leafyrino.Nightly.Installer.exe",
        "portable": "Leafyrino.Nightly.Portable.zip",
    },
    "arm64": {
        "installer": "Experimental-ARM64-Leafyrino.Nightly.Installer.exe",
        "portable": "Experimental-ARM64-Leafyrino.Nightly.Portable.zip",
    },
}


def run_git_command(args: list[str]) -> str:
    p = subprocess.run(
        ["git", *args],
        cwd=os.path.dirname(os.path.realpath(__file__)),
        text=True,
        check=True,
        capture_output=True,
    )
    return p.stdout.strip()


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


parser = argparse.ArgumentParser()
parser.add_argument("--artifacts", required=True)
parser.add_argument("--output", required=True)
args = parser.parse_args()

# must match GIT_BUILD_NUMBER in cmake/GIT.cmake
if run_git_command(["rev-parse", "--is-shallow-repository"]) != "false":
    raise SystemExit("The build number needs the full git history (fetch-depth: 0)")
build = int(run_git_command(["rev-list", "--count", "HEAD"]))

windows: dict[str, dict[str, dict[str, str]]] = {}
for arch, files in WINDOWS_FILES.items():
    for kind, name in files.items():
        path = os.path.join(args.artifacts, name)
        if not os.path.isfile(path):
            raise SystemExit(f"Missing {name} in {args.artifacts}")
        windows.setdefault(arch, {})[kind] = {
            "url": f"{ROOT_DOWNLOAD_URL}/{name}",
            "sha256": sha256_of(path),
        }

info = {
    "build": build,
    "commit": run_git_command(["rev-parse", "HEAD"]),
    "windows": windows,
}

with open(args.output, "w") as f:
    json.dump(info, f, indent=2)
    f.write("\n")

print(json.dumps(info, indent=2))
