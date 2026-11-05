#!/usr/bin/env python3
"""Exercise bounded retry behavior against recorded rustup error shapes."""
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from install_rust_toolchain import RUST_DIST, run_download

URL = RUST_DIST + "/dist/channel-rust-1.98.0.toml.sha256"
DNS_FAILURE = (
    f"error: could not download file from '{URL}' to '/tmp/rustup-fixture': "
    f"error downloading file: error sending request for url ({URL}): "
    "client error (Connect): dns error: error resolving DNS: failed to lookup address information"
)
COMMAND = ["rustup", "toolchain", "install", "1.98.0", "--profile", "minimal"]


class RustDownloadRetry(unittest.TestCase):
    def exercise(self, outcomes, *, expected_delays, should_fail):
        remaining = iter(outcomes)

        def attempt(command, *, env, stdout, **kwargs):
            self.assertEqual(command, COMMAND)
            self.assertEqual(env["RUSTUP_DIST_SERVER"], RUST_DIST)
            code, message = next(remaining)
            stdout.write(message + "\n")
            return subprocess.CompletedProcess(command, code)

        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            with (patch("install_rust_toolchain.subprocess.run", side_effect=attempt) as invoke,
                  patch("install_rust_toolchain.time.sleep") as sleep,
                  contextlib.redirect_stdout(io.StringIO()),
                  contextlib.redirect_stderr(io.StringIO())):
                if should_fail:
                    with self.assertRaises(RuntimeError):
                        run_download(COMMAND, "toolchain", out)
                else:
                    run_download(COMMAND, "toolchain", out)
            self.assertEqual(invoke.call_count, len(outcomes))
            self.assertEqual([call.args[0] for call in sleep.call_args_list], expected_delays)
            for index, (_, message) in enumerate(outcomes, 1):
                self.assertIn(message, (out / f"toolchain-attempt-{index}.log").read_text())

    def test_official_dns_error_then_success(self):
        self.exercise([(1, DNS_FAILURE), (0, "installed")], expected_delays=[3], should_fail=False)

    def test_three_dns_errors_exhaust_retries(self):
        self.exercise([(1, DNS_FAILURE)] * 3, expected_delays=[3, 6], should_fail=True)

    def test_hard_failure_stops_immediately(self):
        checksum = "error: checksum failed for downloaded component"
        for message in (checksum, DNS_FAILURE + "\n" + checksum,
                        DNS_FAILURE.replace("static.rust-lang.org", "mirror.example")):
            with self.subTest(message=message):
                self.exercise([(1, message)], expected_delays=[], should_fail=True)


if __name__ == "__main__":
    unittest.main()
