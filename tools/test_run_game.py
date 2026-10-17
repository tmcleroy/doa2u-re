# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for the disc view used by boot probes."""
from pathlib import Path
import tempfile
import unittest
import json
from unittest.mock import patch

from run_game import prepare_disc, run


class DiscTests(unittest.TestCase):
    def test_play_keeps_player_saves_and_has_no_deadline_or_script(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "private/program").mkdir(parents=True)
            selected = root / "private/import/game.input"
            selected.parent.mkdir()
            selected.write_bytes(b"synthetic input")
            receipt = dict(recipe=dict(xbe_sha256="hash"), manifest_sha256="hash",
                           ebp_count=0, runner_sha256="hash")
            (root / "private/program/receipt.json").write_text(json.dumps(receipt))
            disc = root / "private/play-disc"
            save = disc / ".recomp-storage/profile"
            save.parent.mkdir(parents=True)
            save.write_text("user save")
            with patch("run_game.ROOT", root), patch("run_game.selected_xbe", return_value=selected), \
                    patch("run_game.program_manifest", return_value=("hash", 0)), \
                    patch("run_game.sha256", return_value="hash"), \
                    patch("run_game.subprocess.run") as execute:
                execute.return_value.returncode = 0
                self.assertEqual(run(None), 0)
                self.assertIsNone(execute.call_args.kwargs["timeout"])
                self.assertEqual(execute.call_args.args[0], [
                    str(root / "build/recomp-program/Release/recomp_program_runner.exe"),
                    "--xbe", str(disc / selected.name), "--disc-root", str(disc)])
            self.assertEqual(save.read_text(), "user save")
            logs = list((root / "private/play-logs").glob("*.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("runner_sha256=hash", logs[0].read_text())

    def test_isolated_probe_uses_separate_saves_and_scripted_input(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "private/program").mkdir(parents=True)
            selected = root / "private/import/game.input"
            selected.parent.mkdir()
            selected.write_bytes(b"synthetic input")
            receipt = dict(recipe=dict(xbe_sha256="hash"), manifest_sha256="hash",
                           ebp_count=0, runner_sha256="hash")
            (root / "private/program/receipt.json").write_text(json.dumps(receipt))
            save = root / "private/play-disc/.recomp-storage/profile"
            save.parent.mkdir(parents=True)
            save.write_text("user save")
            with patch("run_game.ROOT", root), patch("run_game.selected_xbe", return_value=selected), \
                    patch("run_game.program_manifest", return_value=("hash", 0)), \
                    patch("run_game.sha256", return_value="hash"), \
                    patch("run_game.subprocess.run") as execute:
                execute.return_value.returncode = 0
                self.assertEqual(run(1, [30, 100], [200], isolated=True,
                                     buttons_polls=[(3800, 8), (4000, 1)],
                                     analog_polls=[(4500, 3), (4600, 1)]), 0)
                command = execute.call_args.args[0]
                self.assertEqual(command[command.index("--disc-root") + 1],
                                 str(root / "private/probe-disc"))
                self.assertIn("--input-script-only", command)
                self.assertEqual(command[-12:-6], ["--input-buttons-pulse-at", "3800", "8",
                                                  "--input-buttons-pulse-at", "4000", "1"])
                self.assertEqual(command[-6:], ["--input-analog-pulse-at", "4500", "3",
                                               "--input-analog-pulse-at", "4600", "1"])
            self.assertEqual(save.read_text(), "user save")
            self.assertFalse((root / "private/probe-disc/.recomp-storage").exists())

    def test_rejects_invalid_start_polls_before_reading_inputs(self):
        for polls in ([0], [-1], [2**64], [1, 2, 3, 4, 5]):
            with self.assertRaisesRegex(ValueError, "Start polls"):
                run(1, polls)

    def test_rejects_invalid_button_polls_before_reading_inputs(self):
        for polls in ([(0, 8)], [(-1, 8)], [(2**64, 8)], [(1, 0)], [(1, 256)],
                      [(1, -1)], [(1, 8)] * 1025):
            with self.assertRaisesRegex(ValueError, "Button polls"):
                run(1, buttons_polls=polls)

    def test_rejects_invalid_analog_polls_before_reading_inputs(self):
        for polls in ([(0, 3)], [(-1, 3)], [(2**64, 3)], [(1, -1)], [(1, 8)],
                      [(1, 3)] * 1025):
            with self.assertRaisesRegex(ValueError, "Analog polls"):
                run(1, analog_polls=polls)

    def test_links_files_and_keeps_source_saves_out(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, disc = root / "source", root / "play"
            (source / "nested").mkdir(parents=True)
            (source / "nested/payload.txt").write_text("synthetic content")
            (source / ".recomp-storage").mkdir()
            (source / ".recomp-storage/save.txt").write_text("source save")
            prepare_disc(source, disc)
            self.assertTrue((source / "nested/payload.txt").samefile(disc / "nested/payload.txt"))
            self.assertFalse((disc / ".recomp-storage").exists())
            (disc / "local-save.txt").write_text("play save")
            prepare_disc(source, disc)
            self.assertEqual((disc / "local-save.txt").read_text(), "play save")

    def test_partial_staging_is_not_treated_as_a_disc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, disc = root / "source", root / "play"
            source.mkdir()
            (root / "play.tmp").mkdir()
            with self.assertRaises(FileExistsError):
                prepare_disc(source, disc)
            self.assertFalse(disc.exists())

    def test_rejects_a_stale_runner_before_preparing_disc(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "private/program").mkdir(parents=True)
            selected = root / "private/synthetic.input"
            selected.write_bytes(b"synthetic input")
            runner = root / "build/recomp-program/Release/recomp_program_runner.exe"
            runner.parent.mkdir(parents=True)
            runner.write_bytes(b"stale runner")
            receipt = dict(recipe=dict(xbe_sha256="input-hash"),
                           manifest_sha256="program-hash", ebp_count=0,
                           runner_sha256="expected-runner-hash")
            (root / "private/program/receipt.json").write_text(json.dumps(receipt))
            with patch("run_game.ROOT", root), patch("run_game.selected_xbe", return_value=selected), \
                    patch("run_game.program_manifest", return_value=("program-hash", 0)), \
                    patch("run_game.sha256", side_effect=lambda p: "input-hash" if p == selected else "different"), \
                    patch("run_game.prepare_disc") as prepare:
                with self.assertRaisesRegex(ValueError, "Runner differs"):
                    run(1)
                prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
