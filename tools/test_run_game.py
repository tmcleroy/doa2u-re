# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for the disc view used by boot probes."""
from pathlib import Path
import tempfile
import unittest
import json
import os
import subprocess
from unittest.mock import patch

from run_game import prepare_disc, run


class DiscTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "WinForms launcher requires Windows")
    def test_settings_dialog_saves_resolution_and_hands_choices_to_runner(self):
        script = r'''
$ErrorActionPreference = 'Stop'
$source = Get-Content (Join-Path $env:RECOMP_LAUNCHER_TEST_SOURCE 'tools/launcher.ps1') -Raw
$source = $source -replace '(?m)^trap [^\r\n]*', ''
$source = $source.Replace('$root = Split-Path $PSScriptRoot -Parent', '$root = $env:RECOMP_LAUNCHER_TEST_SOURCE')
$head = $source.Substring(0, $source.IndexOf('if ($form.ShowDialog()'))
$tail = $source.Substring($source.IndexOf('$height = $heights'))
$tail = $tail.Substring(0, $tail.LastIndexOf('exit $LASTEXITCODE'))
function python { $script:launched = @($args); $global:LASTEXITCODE = 0 }
foreach ($enabled in @($true, $false)) {
    Invoke-Expression $head
    if ($resolution.Items.Count -ne 5 -or $msaa.Items.Count -ne 6) { throw 'Missing graphics choices' }
    $settingsPath = Join-Path $env:RECOMP_LAUNCHER_TEST_ROOT 'launcher.json'
    $resolution.SelectedIndex = if ($enabled) { 4 } else { 0 }
    $msaa.SelectedIndex = if ($enabled) { 5 } else { 4 }
    $smaa.Checked = $widescreen.Checked = $enabled
    Invoke-Expression $tail
    $saved = Get-Content $settingsPath -Raw | ConvertFrom-Json
    $height = if ($enabled) { 2160 } else { 480 }
    $count = if ($enabled) { 32 } else { 16 }
    if ($saved.height -ne $height -or $saved.msaa -ne $count -or
        $saved.smaa -ne $enabled -or $saved.widescreen -ne $enabled) { throw 'Wrong saved graphics settings' }
    $wide = if ($enabled) { '--widescreen' } else { '--no-widescreen' }
    $smaaOption = if ($enabled) { '--smaa' } else { '--no-smaa' }
    $expected = "--play --resolution $height --msaa $count $wide $smaaOption"
    if (($launched[1..($launched.Count - 1)] -join ' ') -ne $expected) { throw 'Wrong runner options' }
}
'''
        with tempfile.TemporaryDirectory() as folder:
            env = dict(os.environ, RECOMP_LAUNCHER_TEST_ROOT=folder,
                       RECOMP_LAUNCHER_TEST_SOURCE=str(Path(__file__).resolve().parents[1]))
            result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-STA",
                                     "-ExecutionPolicy", "Bypass", "-Command", script],
                                    env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

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
                for height in (480, 720, 1080, 1440, 2160):
                    self.assertEqual(run(None, widescreen=True, msaa=32, smaa=True,
                                         resolution=height), 0)
                    env = execute.call_args.kwargs["env"]
                    self.assertEqual([env[name] for name in (
                        "RECOMP_D3D_WIDESCREEN", "RECOMP_D3D_MSAA", "RECOMP_D3D_SMAA")], ["1", "32", "1"])
                    self.assertEqual(float(env["RECOMP_D3D_SCALE"]), height / 480)
                self.assertIsNone(execute.call_args.kwargs["timeout"])
                self.assertEqual(execute.call_args.args[0], [
                    str(root / "build/recomp-program/Release/recomp_program_runner.exe"),
                    "--xbe", str(disc / selected.name), "--disc-root", str(disc)])
            self.assertEqual(save.read_text(), "user save")
            logs = list((root / "private/play-logs").glob("*.log"))
            self.assertEqual(len(logs), 5)
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
                                     analog_polls=[(4500, 3), (4600, 1)],
                                     widescreen=False, msaa=1, smaa=False), 0)
                env = execute.call_args.kwargs["env"]
                self.assertEqual([env[name] for name in (
                    "RECOMP_D3D_WIDESCREEN", "RECOMP_D3D_MSAA", "RECOMP_D3D_SMAA")], ["0", "1", "0"])
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

    def test_rejects_invalid_msaa_before_reading_inputs(self):
        for samples in (0, -1, 3, 64):
            with self.assertRaisesRegex(ValueError, "MSAA"):
                run(1, msaa=samples)

    def test_rejects_invalid_resolution_before_reading_inputs(self):
        for height in (0, -1, 481, 4320):
            with self.assertRaisesRegex(ValueError, "Resolution"):
                run(1, resolution=height)

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
