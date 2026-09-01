#!/usr/bin/env python3
"""Validate GNDHOG ZERO's Cardputer Zero store manifest and optional .deb."""

from __future__ import annotations

import io
import json
import re
import stat
import struct
import subprocess
import sys
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "app-builder.json"
CATEGORIES = {
    "System Tools", "Development", "Hardware & IoT", "Security",
    "Radio & Comms", "AI", "Creative & Office", "Media", "Games",
    "Emulators", "Education", "Lifestyle", "Other",
}
PERMISSIONS = {
    "camera", "microphone", "imu", "network", "additional_hardware",
    "background_service", "external_display",
}
PLACEHOLDER_CODES = {"TEMP", "TODO", "YOUR", "XXXX", "MYAP"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def png_dimensions(path: Path) -> tuple[int, int] | None:
    try:
        header = path.read_bytes()[:24]
    except OSError:
        return None
    if len(header) != 24 or header[:8] != PNG_SIGNATURE:
        return None
    return struct.unpack(">II", header[16:24])


def normalized_name(value: str) -> str:
    return re.sub(r"[-_.]", "", value.lower())


def named_after_package(value: str, package: str) -> bool:
    name = normalized_name(re.sub(r"\.(service|socket|timer|target|path|mount)$", "", value))
    pkg = normalized_name(package)
    return bool(name and pkg) and (name.startswith(pkg) or pkg.startswith(name))


def validate_manifest(errors: list[str]) -> dict:
    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(errors, f"cannot read {MANIFEST}: {exc}")
        return {}

    for key in ("app_name", "package_name", "version", "bin_name", "description", "store"):
        if key not in manifest:
            fail(errors, f"manifest field {key!r} is required")

    package = manifest.get("package_name", "")
    if not isinstance(package, str) or not re.fullmatch(r"[a-z0-9][a-z0-9.+-]+", package):
        fail(errors, "package_name must be a valid lowercase Debian package name")
    if manifest.get("bin_name") != "bfcli":
        fail(errors, "bin_name must preserve the bfcli executable identity")
    if manifest.get("app_name") != "GNDHOG ZERO":
        fail(errors, "app_name must preserve the exact GNDHOG ZERO identity")
    if not isinstance(manifest.get("version"), str) or not manifest.get("version"):
        fail(errors, "version must be a non-empty string")

    store = manifest.get("store")
    if not isinstance(store, dict):
        fail(errors, "store must be an object")
        return manifest

    required = {
        "summary", "description", "categories", "screenshots", "icon",
        "license", "source_repo", "author", "share_code", "permissions",
    }
    missing = sorted(required - store.keys())
    if missing:
        fail(errors, "missing store fields: " + ", ".join(missing))

    summary = store.get("summary")
    if not isinstance(summary, str) or not summary.strip() or len(summary.strip()) > 80:
        fail(errors, "store.summary must contain 1-80 characters")
    description = store.get("description")
    if not isinstance(description, str) or not description.strip():
        fail(errors, "store.description must be non-empty")

    categories = store.get("categories")
    if not isinstance(categories, list) or not 1 <= len(categories) <= 2:
        fail(errors, "store.categories must contain 1-2 entries")
    elif any(category not in CATEGORIES for category in categories):
        fail(errors, "store.categories contains a value outside the current enum")

    icon = store.get("icon")
    icon_dimensions = png_dimensions(ROOT / icon) if isinstance(icon, str) else None
    if icon_dimensions is None:
        fail(errors, "store.icon must point to an existing PNG")
    elif icon_dimensions[0] != icon_dimensions[1] or not 128 <= icon_dimensions[0] <= 512:
        fail(errors, f"store.icon must be square and 128-512 px; got {icon_dimensions}")

    screenshots = store.get("screenshots")
    if not isinstance(screenshots, list) or not 1 <= len(screenshots) <= 6:
        fail(errors, "store.screenshots must contain 1-6 paths")
    else:
        basenames: set[str] = set()
        for screenshot in screenshots:
            dimensions = png_dimensions(ROOT / screenshot) if isinstance(screenshot, str) else None
            if dimensions != (320, 170):
                fail(errors, f"screenshot {screenshot!r} must be a 320x170 PNG; got {dimensions}")
            if isinstance(screenshot, str):
                name = Path(screenshot).name
                if name in basenames:
                    fail(errors, f"screenshot basename {name!r} is duplicated")
                basenames.add(name)

    if store.get("license") != "MIT":
        fail(errors, "store.license must match the repository MIT license")
    source_repo = store.get("source_repo")
    if not isinstance(source_repo, str) or not source_repo.startswith("https://"):
        fail(errors, "store.source_repo must be an HTTPS URL")
    author = store.get("author")
    if not isinstance(author, dict) or not str(author.get("display_name", "")).strip():
        fail(errors, "store.author.display_name is required")

    share_code = store.get("share_code")
    if not isinstance(share_code, str) or not re.fullmatch(r"[A-Za-z0-9]{4}", share_code):
        fail(errors, "store.share_code must contain exactly four letters or digits")
    elif share_code.upper() in PLACEHOLDER_CODES:
        fail(errors, "store.share_code is still a template placeholder")

    permissions = store.get("permissions")
    if not isinstance(permissions, dict) or set(permissions) != PERMISSIONS:
        fail(errors, "store.permissions must contain exactly the seven current permission keys")
    elif any(type(value) is not bool for value in permissions.values()):
        fail(errors, "every store permission must be boolean")
    elif permissions != {
        "camera": False,
        "microphone": False,
        "imu": False,
        "network": False,
        "additional_hardware": True,
        "background_service": False,
        "external_display": False,
    }:
        fail(errors, "permissions do not match the shipped hardware behavior")

    serialized = json.dumps(store).lower()
    if "todo" in serialized or "cardputerzero/template" in serialized:
        fail(errors, "store metadata still contains a rejected template placeholder")
    return manifest


def control_field(package_path: Path, field: str) -> str:
    result = subprocess.run(
        ["dpkg-deb", "-f", str(package_path), field],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def install_path_allowed(path: str, package: str) -> bool:
    if path.startswith(("usr/share/APPLaunch/", "usr/share/doc/")):
        return True
    if path.startswith(("lib/systemd/system/", "usr/lib/systemd/system/")):
        name = path.rsplit("/", 1)[-1]
        return named_after_package(name, package)
    if path.startswith("usr/bin/"):
        name = path[len("usr/bin/"):]
        return "/" not in name and named_after_package(name, package)
    if path.startswith("etc/"):
        return named_after_package(path[len("etc/"):].split("/", 1)[0], package)
    for prefix in ("usr/share/", "usr/lib/", "opt/", "var/lib/"):
        if path.startswith(prefix):
            rest = path[len(prefix):]
            return "/" in rest and named_after_package(rest.split("/", 1)[0], package)
    return False


def service_runs_as_root(text: str) -> bool:
    in_service = False
    user = None
    dynamic_user = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_service = line.lower() == "[service]"
            continue
        if not in_service or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key.lower() == "user":
            user = value
        elif key.lower() == "dynamicuser":
            dynamic_user = value.lower() in {"1", "yes", "true", "on"}
    return not dynamic_user and (not user or user in {"root", "0"})


def validate_package(errors: list[str], manifest: dict, package_path: Path) -> None:
    if not package_path.is_file():
        fail(errors, f"package does not exist: {package_path}")
        return
    try:
        package = control_field(package_path, "Package")
        version = control_field(package_path, "Version")
        architecture = control_field(package_path, "Architecture")
        maintainer = control_field(package_path, "Maintainer")
        tar_bytes = subprocess.run(
            ["dpkg-deb", "--fsys-tarfile", str(package_path)],
            check=True,
            capture_output=True,
        ).stdout
        control_bytes = subprocess.run(
            ["dpkg-deb", "--ctrl-tarfile", str(package_path)],
            check=True,
            capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(errors, f"cannot inspect Debian package: {exc}")
        return

    if package != manifest.get("package_name"):
        fail(errors, f"Debian Package {package!r} does not match the manifest")
    if version != manifest.get("version"):
        fail(errors, f"Debian Version {version!r} does not match the manifest")
    if architecture != "arm64":
        fail(errors, f"Debian Architecture must be arm64, got {architecture!r}")
    expected_name = f"{package}_{version}_{architecture}.deb"
    if package_path.name != expected_name:
        fail(errors, f"package filename must be {expected_name!r}")
    if not maintainer or "m5stack" in maintainer.lower() or "example." in maintainer.lower():
        fail(errors, "Debian Maintainer is empty or still a template identity")

    with tarfile.open(fileobj=io.BytesIO(control_bytes)) as control_archive:
        control_names = {
            re.sub(r"^\./", "", member.name).rstrip("/")
            for member in control_archive.getmembers()
            if not member.isdir()
        }
    maintainer_scripts = control_names & {"preinst", "postinst", "prerm", "postrm"}
    if maintainer_scripts:
        fail(errors, "package unexpectedly contains maintainer scripts: " +
             ", ".join(sorted(maintainer_scripts)))

    files: dict[str, tarfile.TarInfo] = {}
    contents: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as archive:
        for member in archive.getmembers():
            path = re.sub(r"^\./", "", member.name).rstrip("/")
            if not path or member.isdir():
                continue
            files[path] = member
            if member.isfile():
                stream = archive.extractfile(member)
                contents[path] = stream.read() if stream else b""
            if member.ischr() or member.isblk():
                fail(errors, f"package contains device node {path}")
            if member.mode & (stat.S_ISUID | stat.S_ISGID):
                fail(errors, f"package contains setuid/setgid path {path}")
            if not install_path_allowed(path, package):
                fail(errors, f"package installs to a disallowed path: {path}")

    desktop_path = "usr/share/APPLaunch/applications/bfcli.desktop"
    if desktop_path not in files:
        fail(errors, "package is missing the APPLaunch desktop entry")
    else:
        desktop = contents.get(desktop_path, b"").decode("utf-8", "replace")
        if "Name=GNDHOG ZERO" not in desktop:
            fail(errors, "desktop entry does not preserve the visible app name")
        if "Exec=/opt/bfcli/run-bfcli" not in desktop:
            fail(errors, "desktop Exec does not point at the packaged launcher")
        if "Icon=share/images/gndhog-zero_100.png" not in desktop:
            fail(errors, "desktop Icon does not point at the packaged mascot")

    for required in (
        "opt/bfcli/bin/bfcli",
        "opt/bfcli/run-bfcli",
        "usr/share/APPLaunch/share/images/gndhog-zero_100.png",
        "usr/share/doc/bfcli/README.md",
        "usr/share/doc/bfcli/copyright",
    ):
        if required not in files:
            fail(errors, f"package is missing {required}")

    for path, payload in contents.items():
        if path.endswith(".service") and path.startswith(
            ("lib/systemd/system/", "usr/lib/systemd/system/", "etc/systemd/system/")
        ) and service_runs_as_root(payload.decode("utf-8", "replace")):
            fail(errors, f"system service would run as root: {path}")


def main() -> int:
    errors: list[str] = []
    manifest = validate_manifest(errors)
    if len(sys.argv) > 2:
        fail(errors, "usage: validate-app-store.py [package.deb]")
    elif len(sys.argv) == 2:
        validate_package(errors, manifest, Path(sys.argv[1]).resolve())

    if errors:
        print("Cardputer Zero AppStore validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    suffix = " and Debian package" if len(sys.argv) == 2 else ""
    print(f"Cardputer Zero AppStore manifest{suffix} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
