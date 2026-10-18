# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a tester ZIP (e.g. 0.0.1-test): source, the pinned lifter and setup tools, no game data."""
# Adapted from committed DOA3 tools/package_test.py at 5747204.
import argparse
import io
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SUBMODULE = "tools/xboxrecomp"
# Agent notes and dev-only submodule metadata stay out of the tester package.
EXCLUDE = {"AGENTS.md", ".gitmodules", SUBMODULE}
# extract-xiso.exe and its LICENSE.TXT, copied from the DOA3 test package.
ARTIFACTS = ROOT / "private" / "release-assets"


def git(*args, cwd=ROOT):
    return subprocess.check_output(["git", *args], cwd=cwd)


def archive(cwd, prefix):
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", "HEAD", cwd=cwd))) as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name not in EXCLUDE:
                yield prefix + member.name, tar.extractfile(member).read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    version = parser.parse_args().version
    if git("status", "--porcelain", "--untracked-files=no").strip():
        raise SystemExit("Commit first: the package is built from HEAD")
    pinned = git("ls-tree", "HEAD", SUBMODULE).split()[2].decode()
    if git("rev-parse", "HEAD", cwd=ROOT / SUBMODULE).decode().strip() != pinned or \
            git("status", "--porcelain", cwd=ROOT / SUBMODULE).strip():
        raise SystemExit(f"{SUBMODULE} must be a clean checkout of {pinned}")

    name = f"DOA2U-{version}"
    out = ROOT / "private" / "release" / f"{name}.zip"
    if out.exists():
        raise SystemExit(f"{out} already exists")
    artifacts = ("extract-xiso.exe", "LICENSE.TXT")
    missing = [a for a in artifacts if not (ARTIFACTS / a).is_file()]
    if missing:
        raise SystemExit(f"Missing from {ARTIFACTS}: {', '.join(missing)}")
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_suffix(".zip.tmp")
    with zipfile.ZipFile(partial, "w", zipfile.ZIP_DEFLATED) as package:
        for path, data in archive(ROOT, f"{name}/"):
            package.writestr(path, data)
        for path, data in archive(ROOT / SUBMODULE, f"{name}/{SUBMODULE}/"):
            package.writestr(path, data)
        for artifact in artifacts:
            package.write(ARTIFACTS / artifact, f"{name}/tools/artifacts/{artifact}")
        package.write(ROOT / "docs" / "test-release.md", f"{name}/README.txt")
    partial.rename(out)
    print(out)


if __name__ == "__main__":
    main()
