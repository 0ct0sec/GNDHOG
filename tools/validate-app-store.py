#!/usr/bin/env python3
"""Validate GNDHOG ZERO's Cardputer Zero store manifest and optional .deb."""

from __future__ import annotations

import io
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath


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
    return re.sub(r"[^a-z0-9]", "", value.lower())


def named_after_owner(value: str, *owners: str) -> bool:
    stem = re.sub(r"\.(desktop|service|socket|timer|target|path|mount)$", "", value)
    name = normalized_name(stem)
    return bool(name) and any(
        owner and name.startswith(owner)
        for owner in (normalized_name(candidate) for candidate in owners)
    )


def manifest_asset_path(value: object) -> Path | None:
    if not isinstance(value, str) or not value or "\\" in value:
        return None
    relative = PurePosixPath(value)
    if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
        return None
    path = (ROOT / Path(*relative.parts)).resolve()
    try:
        path.relative_to(ROOT.resolve())
    except ValueError:
        return None
    return path


def archive_path(value: str) -> str | None:
    while value.startswith("./"):
        value = value[2:]
    value = value.rstrip("/")
    if not value:
        return ""
    if "\\" in value or "//" in value:
        return None
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        return None
    return str(path)


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
    icon_path = manifest_asset_path(icon)
    icon_dimensions = png_dimensions(icon_path) if icon_path else None
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
            screenshot_path = manifest_asset_path(screenshot)
            dimensions = png_dimensions(screenshot_path) if screenshot_path else None
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


def install_path_allowed(path: str, package: str, app_name: str) -> bool:
    if path.startswith("usr/share/APPLaunch/applications/"):
        name = path.removeprefix("usr/share/APPLaunch/applications/")
        return "/" not in name and name.endswith(".desktop") and \
            named_after_owner(name, package, app_name)
    if path.startswith("usr/share/APPLaunch/share/images/"):
        name = path.removeprefix("usr/share/APPLaunch/share/images/")
        return "/" not in name and name.endswith(".png") and \
            named_after_owner(name, package, app_name)
    if path.startswith("usr/share/APPLaunch/"):
        return False
    if path.startswith("usr/share/doc/"):
        rest = path.removeprefix("usr/share/doc/")
        return "/" in rest and rest.split("/", 1)[0] == package
    if path.startswith(("lib/systemd/system/", "usr/lib/systemd/system/")):
        name = path.rsplit("/", 1)[-1]
        return named_after_owner(name, package, app_name)
    if path.startswith("usr/bin/"):
        name = path[len("usr/bin/"):]
        return "/" not in name and named_after_owner(name, package, app_name)
    if path.startswith("etc/"):
        return named_after_owner(path[len("etc/"):].split("/", 1)[0], package, app_name)
    for prefix in ("usr/share/", "usr/lib/", "opt/", "var/lib/"):
        if path.startswith(prefix):
            rest = path[len(prefix):]
            return "/" in rest and named_after_owner(
                rest.split("/", 1)[0], package, app_name
            )
    return False


def desktop_fields(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    in_desktop = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_desktop = line == "[Desktop Entry]"
            continue
        if in_desktop and "=" in line:
            key, value = line.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


def is_aarch64_elf(payload: bytes) -> bool:
    return (
        len(payload) >= 20
        and payload[:4] == b"\x7fELF"
        and payload[4] == 2       # ELFCLASS64
        and payload[5] == 1       # little endian
        and int.from_bytes(payload[18:20], "little") == 183  # EM_AARCH64
    )


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
            path
            for member in control_archive.getmembers()
            if not member.isdir()
            for path in [archive_path(member.name)]
            if path is not None
        }
    maintainer_scripts = control_names & {"preinst", "postinst", "prerm", "postrm"}
    if maintainer_scripts:
        fail(errors, "package unexpectedly contains maintainer scripts: " +
             ", ".join(sorted(maintainer_scripts)))

    files: dict[str, tarfile.TarInfo] = {}
    contents: dict[str, bytes] = {}
    app_name = str(manifest.get("app_name", ""))
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as archive:
        for member in archive.getmembers():
            path = archive_path(member.name)
            if path is None:
                fail(errors, f"package contains an unsafe archive path: {member.name!r}")
                continue
            if not path:
                continue
            if member.uid != 0 or member.gid != 0:
                fail(errors, f"package path is not owned by root:root: {path}")
            if member.isdir():
                continue
            if path in files:
                fail(errors, f"package contains a duplicate archive path: {path}")
            files[path] = member
            if not member.isfile():
                fail(errors, f"package contains a non-regular payload: {path}")
                continue
            stream = archive.extractfile(member)
            contents[path] = stream.read() if stream else b""
            if member.mode & (stat.S_ISUID | stat.S_ISGID):
                fail(errors, f"package contains setuid/setgid path {path}")
            if not install_path_allowed(path, package, app_name):
                fail(errors, f"package installs to a disallowed path: {path}")

    desktop_path = "usr/share/APPLaunch/applications/bfcli.desktop"
    if desktop_path not in files:
        fail(errors, "package is missing the APPLaunch desktop entry")
    else:
        desktop = contents.get(desktop_path, b"").decode("utf-8", "replace")
        desktop_data = desktop_fields(desktop)
        if desktop_data.get("Name") != "GNDHOG ZERO":
            fail(errors, "desktop entry does not preserve the visible app name")
        if desktop_data.get("Exec") != "/opt/bfcli/run-bfcli":
            fail(errors, "desktop Exec does not point at the packaged launcher")
        if desktop_data.get("Icon") != "share/images/gndhog-zero_100.png":
            fail(errors, "desktop Icon does not point at the packaged mascot")

    expected_payloads = {
        # package.sh accepts an alternate cross-build artifact for CI/release
        # staging; compare against that exact input instead of an unrelated
        # default build that may not exist.
        "opt/bfcli/bin/bfcli": (
            Path(os.environ.get("GNDHOG_ARM64_BINARY") or
                 ROOT / "build-arm64/bfcli").resolve(),
            0o755,
        ),
        "opt/bfcli/run-bfcli": (ROOT / "packaging/run-bfcli", 0o755),
        desktop_path: (ROOT / "packaging/bfcli.desktop", 0o644),
        "usr/share/APPLaunch/share/images/gndhog-zero_100.png": (
            ROOT / "assets/gndhog-zero_100.png", 0o644
        ),
        "usr/share/doc/bfcli/README.md": (ROOT / "README.md", 0o644),
        "usr/share/doc/bfcli/copyright": (ROOT / "packaging/copyright", 0o644),
    }
    for required, (source, expected_mode) in expected_payloads.items():
        member = files.get(required)
        if member is None:
            fail(errors, f"package is missing {required}")
            continue
        if member.mode & 0o7777 != expected_mode:
            fail(errors, f"package mode for {required} must be {expected_mode:04o}")
        try:
            expected = source.read_bytes()
        except OSError as exc:
            fail(errors, f"cannot read package source {source}: {exc}")
            continue
        if contents.get(required) != expected:
            fail(errors, f"packaged payload does not match its source: {required}")

    binary_path = "opt/bfcli/bin/bfcli"
    if binary_path in contents and not is_aarch64_elf(contents[binary_path]):
        fail(errors, "packaged bfcli is not a little-endian AArch64 ELF executable")

    for path, payload in contents.items():
        if path.endswith(".service") and path.startswith(
            ("lib/systemd/system/", "usr/lib/systemd/system/", "etc/systemd/system/")
        ):
            store = manifest.get("store", {})
            permissions = store.get("permissions", {}) if isinstance(store, dict) else {}
            if not permissions.get("background_service", False):
                fail(errors, f"package contains an undeclared background service: {path}")
            elif service_runs_as_root(payload.decode("utf-8", "replace")):
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
