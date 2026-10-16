# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a bounded boot probe from a hardlinked disc and keep its evidence private."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from build_game import ROOT, program_manifest, selected_xbe
from extract_iso import sha256


def prepare_disc(source, disc):
    if disc.exists():
        return
    staging = disc.with_name(disc.name + ".tmp")
    staging.mkdir(parents=True)
    for path in source.rglob("*"):
        relative = path.relative_to(source)
        if relative.parts[0] == ".recomp-storage":
            continue
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise ValueError("Disc source contains a link")
        if path.is_file():
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(path, destination)
    staging.rename(disc)


def run(seconds, start_polls=(), a_polls=(), isolated=False):
    if seconds <= 0:
        raise ValueError("Probe duration must be positive")
    if len(start_polls) > 4 or any(poll <= 0 or poll > 2**64 - 1 for poll in start_polls):
        raise ValueError("Start polls must be positive uint64 values, at most four")
    if len(a_polls) > 192 or any(poll <= 0 or poll > 2**64 - 1 for poll in a_polls):
        raise ValueError("A polls must be positive uint64 values, at most 192")
    selected = selected_xbe()
    generated = ROOT / "private/program/generated"
    receipt = json.loads((ROOT / "private/program/receipt.json").read_text(encoding="utf-8"))
    if receipt["recipe"]["xbe_sha256"] != sha256(selected) or program_manifest(generated) != (
            receipt["manifest_sha256"], receipt["ebp_count"]):
        raise ValueError("Game or generated sources differ from the build receipt")
    runner = ROOT / "build/recomp-program/Release/recomp_program_runner.exe"
    if sha256(runner) != receipt["runner_sha256"]:
        raise ValueError("Runner differs from its successful build receipt")
    disc = ROOT / ("private/probe-disc" if isolated else "private/play-disc")
    prepare_disc(selected.parent, disc)
    xbe = disc / selected.name
    if sha256(xbe) != sha256(selected):
        raise ValueError("Play disc belongs to a different game import")
    logs = ROOT / "private/boot-logs"
    logs.mkdir(exist_ok=True)
    log_path = logs / (uuid.uuid4().hex[:8] + ".log")
    with log_path.open("x", encoding="utf-8") as log:
        log.write(f"runner_sha256={sha256(runner)}\nprogram_manifest={receipt['manifest_sha256']}\n")
        log.flush()
        try:
            command = [str(runner), "--xbe", str(xbe), "--disc-root", str(disc)]
            if isolated:
                command.append("--input-script-only")
            for poll in start_polls:
                command.extend(["--input-start-pulse-at", str(poll)])
            for poll in a_polls:
                command.extend(["--input-a-pulse-at", str(poll)])
            result = subprocess.run(command,
                                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=seconds)
            code = result.returncode
        except subprocess.TimeoutExpired:
            log.write("\nprobe timed out; child process terminated\n")
            code = 124
    print(f"exit={code} log={log_path}")
    return code


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--input-start-pulse-at", type=int, action="append", default=[])
    parser.add_argument("--input-a-pulse-at", type=int, action="append", default=[])
    parser.add_argument("--isolated", action="store_true",
                        help="Use separate probe saves and ignore physical input")
    args = parser.parse_args()
    try:
        raise SystemExit(run(args.seconds, args.input_start_pulse_at,
                             args.input_a_pulse_at, args.isolated))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Cannot run boot probe: {error}", file=sys.stderr)
        raise SystemExit(1)
