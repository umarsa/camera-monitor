#!/opt/homebrew/bin/python3
"""Bundle non-system dylibs into CameraMonitor.app for Apple Silicon sharing."""

from __future__ import annotations

import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path


SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")


def run(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def dependencies(binary: Path) -> list[str]:
    lines = run("/usr/bin/otool", "-L", str(binary)).stdout.splitlines()[1:]
    result: list[str] = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        result.append(stripped.split(" (compatibility version", 1)[0])
    return result


def is_system(dependency: str) -> bool:
    return dependency.startswith(SYSTEM_PREFIXES)


def change_reference(binary: Path, old: str, new: str) -> None:
    run("/usr/bin/install_name_tool", "-change", old, new, str(binary))


def add_runtime_path(executable: Path) -> None:
    result = run(
        "/usr/bin/install_name_tool",
        "-add_rpath",
        "@executable_path/../Frameworks",
        str(executable),
        check=False,
    )
    if result.returncode != 0 and "would duplicate" not in result.stderr:
        raise RuntimeError(result.stderr.strip())


def bundle(app: Path) -> None:
    info_path = app / "Contents" / "Info.plist"
    with info_path.open("rb") as handle:
        executable_name = plistlib.load(handle)["CFBundleExecutable"]

    executable = app / "Contents" / "MacOS" / executable_name
    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)

    queue = [executable]
    copied: dict[str, Path] = {}
    processed: set[Path] = set()

    # sdl2-compat deliberately loads SDL3 with dlopen(), so SDL3 is absent from
    # otool's static dependency list. Place the exact filename it probes beside
    # the compatibility dylib and feed it through the same recursive validator.
    sdl3_source = Path("/opt/homebrew/opt/sdl3/lib/libSDL3.dylib")
    if sdl3_source.exists():
        sdl3_destination = frameworks / "libSDL3.dylib"
        shutil.copy2(sdl3_source.resolve(), sdl3_destination)
        sdl3_destination.chmod(0o755)
        copied[sdl3_destination.name] = sdl3_source.resolve()
        run(
            "/usr/bin/install_name_tool",
            "-id",
            "@rpath/libSDL3.dylib",
            str(sdl3_destination),
        )
        queue.append(sdl3_destination)

    while queue:
        binary = queue.pop(0)
        if binary in processed:
            continue
        processed.add(binary)

        for dependency in dependencies(binary):
            if is_system(dependency):
                continue
            if dependency.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
                continue

            source = Path(dependency).resolve()
            if not source.exists():
                raise FileNotFoundError(f"Missing dependency: {dependency}")
            destination = frameworks / source.name
            prior = copied.get(source.name)
            if prior and prior != source:
                raise RuntimeError(
                    f"Dependency name collision: {prior} and {source}"
                )
            if not destination.exists():
                shutil.copy2(source, destination)
                destination.chmod(0o755)
                copied[source.name] = source
                run(
                    "/usr/bin/install_name_tool",
                    "-id",
                    f"@rpath/{source.name}",
                    str(destination),
                )
                queue.append(destination)
            change_reference(binary, dependency, f"@rpath/{source.name}")

    add_runtime_path(executable)

    for binary in [executable, *sorted(frameworks.glob("*.dylib"))]:
        for dependency in dependencies(binary):
            if is_system(dependency):
                continue
            if dependency.startswith("@rpath/"):
                expected = frameworks / Path(dependency).name
                if not expected.exists():
                    raise RuntimeError(
                        f"Unbundled runtime dependency in {binary.name}: {dependency}"
                    )
                continue
            if dependency == f"@rpath/{binary.name}":
                continue
            raise RuntimeError(
                f"Non-portable dependency in {binary.name}: {dependency}"
            )

    run("/usr/bin/xattr", "-cr", str(app), check=False)
    run(
        "/usr/bin/codesign",
        "--force",
        "--deep",
        "--sign",
        "-",
        "--timestamp=none",
        str(app),
    )
    print(f"Bundled {len(copied)} runtime libraries into {app}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: bundle_dependencies.py <CameraMonitor.app>", file=sys.stderr)
        return 2
    app = Path(sys.argv[1]).resolve()
    if not app.is_dir():
        print(f"app bundle not found: {app}", file=sys.stderr)
        return 1
    bundle(app)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
