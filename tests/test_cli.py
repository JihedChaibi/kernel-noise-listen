"""End-to-end checks for the launcher, renderer and terminal controls."""

import array
import errno
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import subprocess
import tempfile
import termios
import time
import unittest
import wave


ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / ".build/kernel-noise"
LAUNCHER = ROOT / "listen.sh"


class DemoTests(unittest.TestCase):
    def run_command(self, args, **kwargs):
        return subprocess.run(
            args,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            **kwargs,
        )

    def test_recording_frame_count_and_level(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "cpu.wav"
            result = self.run_command([
                str(LAUNCHER), "--render", str(path),
                "--duration", ".3", "--mode", "cpu", "--no-ui",
            ])
            self.assertEqual(result.returncode, 0, result.stderr)

            with wave.open(str(path)) as wav:
                self.assertEqual(
                    (wav.getnframes(), wav.getframerate(),
                     wav.getnchannels(), wav.getsampwidth()),
                    (14400, 48000, 2, 2),
                )
                samples = array.array("h", wav.readframes(wav.getnframes()))

            self.assertGreater(max(map(abs, samples)), 0)
            self.assertLessEqual(max(map(abs, samples)), int(.151 * 32767))

    def test_each_mode_emits_live_pcm(self):
        for mode in ("cpu", "irq", "scheduler", "mix"):
            with self.subTest(mode=mode):
                result = self.run_command([
                    str(ENGINE), "--mode", mode, "--duration", "1", "--no-ui",
                ])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(len(result.stdout), 48000 * 4)

                # A quiet machine may have zero IRQ or scheduler deltas.
                if mode in ("cpu", "mix"):
                    self.assertTrue(any(result.stdout))

    def test_default_requires_no_player_for_file_output(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_command([
                str(LAUNCHER), "--render", str(Path(directory) / "take.wav"),
                "--duration", ".1", "--no-ui",
            ])
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_existing_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "keep.wav"
            path.write_bytes(b"original")
            result = self.run_command([
                str(ENGINE), "--render", str(path), "--duration", ".1",
            ])
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(path.read_bytes(), b"original")

    def test_invalid_options(self):
        options = (
            ["--volume", "nan"],
            ["--duration", "-1"],
            ["--volume", "1"],
            ["--mode", "panic"],
            ["--render", "take.wav"],
        )
        for args in options:
            with self.subTest(args=args):
                result = self.run_command([str(ENGINE), *args])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")

    def test_launcher_pcm_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "live.wav"
            result = self.run_command([
                str(LAUNCHER), "--backend", "null", "--record", str(path),
                "--duration", ".25", "--no-ui",
            ])
            self.assertEqual(result.returncode, 0, result.stderr)

            with wave.open(str(path)) as wav:
                self.assertEqual(wav.getnframes(), 12000)

    def test_player_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            fake_player = Path(directory) / "aplay"
            fake_player.write_text("#!/bin/sh\nexit 17\n")
            fake_player.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(directory) + ":" + env["PATH"]
            result = self.run_command([
                str(LAUNCHER), "--backend", "aplay",
                "--duration", "2", "--no-ui",
            ], env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Playback stopped with an error", result.stderr)

    @unittest.skipUnless(shutil.which("ffplay"), "FFplay is not installed")
    def test_ffplay_backend_if_available(self):
        # SDL's dummy backend tests PCM options, not physical speakers.
        env = os.environ.copy()
        env["SDL_AUDIODRIVER"] = "dummy"
        result = self.run_command([
            str(LAUNCHER), "--backend", "ffplay",
            "--duration", ".25", "--no-ui",
        ], env=env)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_terminal_controls_and_restore(self):
        pid, master = pty.fork()
        if pid == 0:
            try:
                os.chdir(ROOT)
                os.execv(str(LAUNCHER), [
                    str(LAUNCHER), "--backend", "null", "--duration", "8",
                ])
            except BaseException:
                os._exit(127)

        original = termios.tcgetattr(master)
        transcript = bytearray()

        def read_for(seconds):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                if not select.select([master], [], [], .03)[0]:
                    continue
                try:
                    data = os.read(master, 8192)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                transcript.extend(data)

        try:
            read_for(.4)
            self.assertIn(
                b"\x1b[1m4 mix\x1b[22m | +/- volume "
                b"\x1b[2m(38%)\x1b[22m",
                transcript,
            )

            os.write(master, b"1+")
            read_for(.3)
            self.assertIn(
                b"\x1b[1m1 cpu\x1b[22m  2 irq  3 scheduler  4 mix"
                b" | +/- volume \x1b[2m(44%)\x1b[22m",
                transcript,
            )

            for key, label in ((b"2", b"2 irq"),
                               (b"3", b"3 scheduler"),
                               (b"4", b"4 mix")):
                os.write(master, key)
                read_for(.12)
                self.assertIn(b"\x1b[1m" + label + b"\x1b[22m", transcript)

            # 'm' wraps from mix back to CPU; '-' restores the initial gain.
            position = len(transcript)
            os.write(master, b"m-")
            read_for(.2)
            self.assertIn(
                b"\x1b[1m1 cpu\x1b[22m  2 irq  3 scheduler  4 mix"
                b" | +/- volume \x1b[2m(38%)\x1b[22m",
                transcript[position:],
            )

            os.write(master, b" ")
            read_for(.3)
            self.assertIn(b"MUTED", transcript)

            os.write(master, b"q")
            read_for(.3)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                done, status = os.waitpid(pid, os.WNOHANG)
                if done:
                    self.assertEqual(os.waitstatus_to_exitcode(status), 0, transcript)
                    break
                time.sleep(.02)
            else:
                self.fail("q did not stop the pipeline")

            final = termios.tcgetattr(master)
            raw_flags = termios.ECHO | termios.ICANON
            self.assertEqual(final[3] & raw_flags, original[3] & raw_flags)
        finally:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            os.close(master)


if __name__ == "__main__":
    unittest.main()
