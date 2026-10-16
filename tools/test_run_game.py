# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for the disc view used by boot probes."""
from pathlib import Path
import tempfile
import unittest
import json
from unittest.mock import patch

from run_game import prepare_disc, run


class DiscTests(unittest.TestCase):
    def test_rejects_invalid_start_polls_before_reading_inputs(self):
        for polls in ([0], [-1], [2**64], [1, 2, 3, 4, 5]):
            with self.assertRaisesRegex(ValueError, "Start polls"):
                run(1, polls)

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
