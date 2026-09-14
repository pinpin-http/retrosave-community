"""Run the server suite against disposable services, never a workstation database."""

from __future__ import annotations

import subprocess
import sys

from test_api_interop import ROOT, disposable_services


def main() -> int:
    environment: dict[str, str] = {}
    with disposable_services(environment=environment) as (_, _, _, directory):
        environment["RETROSAVE_TEST_DB"] = "1"
        return subprocess.run(
            [
                sys.executable,
                "-m",
                "pytest",
                "-c",
                str(ROOT / "pyproject.toml"),
                str(ROOT / "server/tests"),
                "-q",
            ],
            cwd=directory,
            env=environment,
            check=False,
        ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
