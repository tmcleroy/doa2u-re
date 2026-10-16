# SPDX-License-Identifier: GPL-3.0-or-later
"""Lift the privately selected game executable and build its native runner."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import uuid

from extract_iso import run_logged, sha256

ROOT = Path(__file__).resolve().parents[1]
LIFTER = ROOT / "tools/xboxrecomp"


def selected_xbe():
    target = json.loads((ROOT / "private/target.json").read_text(encoding="utf-8"))
    xbe = (ROOT / target["xbe_path"]).resolve(strict=True)
    if not xbe.is_relative_to((ROOT / "private").resolve()) or not xbe.is_file():
        raise ValueError("Selected executable must be a file under private/")
    if sha256(xbe) != target["xbe_sha256"]:
        raise ValueError("Selected executable differs from its private identity receipt")
    return xbe


def program_manifest(generated):
    chunks = sorted(generated.glob("recomp_[0-9][0-9][0-9][0-9].c"))
    if not chunks or [p.name for p in chunks] != [f"recomp_{i:04d}.c" for i in range(len(chunks))]:
        raise ValueError("Generated program has no chunks or a gap in its chunk sequence")
    members = chunks + [generated / "recomp_dispatch.c", generated / "recomp_funcs.h"]
    manifest = "".join(f"{p.name}\t{p.stat().st_size}\t{sha256(p)}\n" for p in members)
    ebp = sum(p.read_text(encoding="utf-8").count("\n    uint32_t ebp;\n")
              for p in members if p.suffix == ".c")
    return hashlib.sha256(manifest.encode("utf-8")).hexdigest(), ebp


def build(build_only=False, lift_only=False):
    import capstone
    xbe = selected_xbe()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LIFTER, text=True).strip()
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=LIFTER):
        raise ValueError("Commit lifter changes and update its pin before generating")
    seeds = ROOT / "tools/doa2u/seed_functions.json"
    recipe = dict(xbe_sha256=sha256(xbe), lifter_commit=revision, capstone=capstone.__version__,
                  seed_functions_sha256=sha256(seeds))
    work = ROOT / "private/program"
    work.mkdir(parents=True, exist_ok=True)
    logs = ROOT / "private/build-logs" / uuid.uuid4().hex[:8]
    logs.mkdir(parents=True)
    generated, disasm = work / "generated", work / "disasm"
    receipt_path = work / "receipt.json"
    if build_only:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt["recipe"] != recipe:
            raise ValueError("Generated receipt belongs to a different executable or lifter recipe")
        if program_manifest(generated) != (receipt["manifest_sha256"], receipt["ebp_count"]):
            raise ValueError("Generated program differs from its authenticated receipt")
    else:
        analysis = work / "analysis.json"
        run_logged([sys.executable, "-u", "-m", "tools.xbe_parser", xbe, "--json", analysis],
                   logs / "xbe-parser.log", LIFTER)
        run_logged([sys.executable, "-u", "-m", "tools.disasm", xbe, "-o", disasm,
                    "--analysis-json", analysis, "--seed-functions", seeds],
                   logs / "disasm.log", LIFTER)
        run_logged([sys.executable, "-u", "-m", "tools.recomp", xbe, "--all", "--split", "1000",
                    "--game-name", "Dead or Alive 2 Ultimate", "--disasm-dir", disasm,
                    "--func-id-dir", work / "none", "--abi-dir", work / "none",
                    "--icall-sites", work / "none.json", "--gen-dir", generated,
                    "--output-dir", work / "metadata"], logs / "lift.log", LIFTER)
        manifest, ebp = program_manifest(generated)
        receipt = dict(recipe=recipe, manifest_sha256=manifest, ebp_count=ebp)
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    if lift_only:
        return
    build_dir = ROOT / "build/recomp-program"
    run_logged(["cmake", "-S", ROOT / "recomp-runtime", "-B", build_dir, "-A", "x64",
                f"-DRECOMP_PROGRAM_DIR={generated.as_posix()}",
                f"-DRECOMP_XBE_SHA256={recipe['xbe_sha256']}",
                f"-DRECOMP_PROGRAM_MANIFEST_SHA256={receipt['manifest_sha256']}",
                f"-DRECOMP_PROGRAM_EBP_EXPECTED={receipt['ebp_count']}"], logs / "configure.log")
    run_logged(["cmake", "--build", build_dir, "--config", "Release", "--parallel", "4",
                "--target", "recomp_program_runner"], logs / "build.log")
    receipt["runner_sha256"] = sha256(build_dir / "Release/recomp_program_runner.exe")
    receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--build-only", action="store_true", help="Rebuild authenticated generated sources")
    modes.add_argument("--lift-only", action="store_true", help="Generate without building the runner")
    args = parser.parse_args()
    try:
        build(args.build_only, args.lift_only)
    except (OSError, ValueError, KeyError, ImportError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
