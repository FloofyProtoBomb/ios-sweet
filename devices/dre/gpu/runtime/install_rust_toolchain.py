#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install the pinned Rust toolchain with bounded official-download retries."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

RUST_DIST = "https://static.rust-lang.org"
MAX_ATTEMPTS = 3
DOWNLOAD_TIMEOUT = 240

# Exact rustup failure shape observed on the macOS runner. Require both
# reported URLs to name the official host and leave other errors terminal.
OFFICIAL_DNS_FAILURE = re.compile(
    r"^error: could not download file from 'https://static\.rust-lang\.org/[^']+' "
    r".*: error downloading file: error sending request for url "
    r"\(https://static\.rust-lang\.org/[^)]+\): client error \(Connect\): "
    r"dns error: error resolving DNS:"
)


def run_download(command: list[str], stage: str, out: Path) -> None:
    env = dict(os.environ, RUSTUP_DIST_SERVER=RUST_DIST)
    for attempt in range(1, MAX_ATTEMPTS + 1):
        log = out / f"{stage}-attempt-{attempt}.log"
        with log.open("w", encoding="utf-8") as stream:
            stream.write(json.dumps(command) + "\n")
            stream.flush()
            try:
                result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                    check=False, timeout=DOWNLOAD_TIMEOUT)
            except subprocess.TimeoutExpired as exc:
                stream.write(f"\nProcess timeout after {DOWNLOAD_TIMEOUT}s\n")
                raise RuntimeError(f"{stage} timed out. See {log}") from exc
        if result.returncode == 0:
            return
        output = log.read_text(encoding="utf-8", errors="replace").partition("\n")[2]
        errors = [line for line in output.splitlines() if line.startswith("error:")]
        retryable = len(errors) == 1 and OFFICIAL_DNS_FAILURE.match(errors[0])
        if not retryable or attempt == MAX_ATTEMPTS:
            print(output[-4000:], file=sys.stderr)
            raise RuntimeError(f"{stage} failed on attempt {attempt}. See {log}")
        delay = 3 * attempt
        print(f"{stage} DNS failure on attempt {attempt}. Retrying in {delay}s.", flush=True)
        time.sleep(delay)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustup", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        args.out.mkdir(parents=True)
        rustup = str(args.rustup)
        run_download([rustup, "toolchain", "install", "1.98.0", "--profile", "minimal"],
            "toolchain", args.out)
        run_download([rustup, "target", "add", "--toolchain", "1.98.0", "aarch64-apple-ios"],
            "ios-target", args.out)
    except (OSError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
