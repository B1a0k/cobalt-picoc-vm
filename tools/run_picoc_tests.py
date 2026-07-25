#!/usr/bin/env python3
"""Run the vendored PicoC compatibility corpus through cvmc -> cvmrun.

The harness intentionally distinguishes compiler, verifier/runtime, and output
failures so progress cannot be hidden by a single pass count.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import re
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class Result:
    name: str
    status: str
    detail: str = ""


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def diff_b_normalize(data: bytes) -> bytes:
    """Normalize insignificant whitespace used by the legacy fixtures.

    The upstream 27_sizeof fixture has one more terminal blank line than the
    output produced by PicoC itself. Terminal blank lines are therefore
    treated like other trailing whitespace, while internal line structure
    remains significant.
    """
    text = data.replace(b"\r\n", b"\n")
    normalized = b"\n".join(
        re.sub(br"[ \t]+", b" ", line).rstrip(b" ")
        for line in text.split(b"\n")
    )
    return normalized.rstrip(b" \t\n")


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--suite",
        type=Path,
        default=project / "tests" / "upstream-picoc",
    )
    parser.add_argument(
        "--build",
        type=Path,
        default=project / "build",
    )
    parser.add_argument("--filter", default="")
    parser.add_argument(
        "--report",
        type=Path,
        default=project / "build" / "picoc-test-report.json",
    )
    arguments = parser.parse_args()

    compiler = arguments.build / "cvmc.exe"
    runtime = arguments.build / "cvmrun.exe"
    if not compiler.is_file() or not runtime.is_file():
        print("build/cvmc.exe and build/cvmrun.exe are required", file=sys.stderr)
        return 2

    tests = sorted(arguments.suite.glob("*.expect"))
    results: list[Result] = []
    with tempfile.TemporaryDirectory(prefix="cvm-tests-") as temporary:
        temporary_path = Path(temporary)
        for expected_path in tests:
            name = expected_path.stem
            if arguments.filter and arguments.filter not in name:
                continue
            source_path = expected_path.with_suffix(".c")
            if not source_path.is_file():
                results.append(Result(name, "harness_error", "source is missing"))
                continue

            bytecode_path = temporary_path / f"{name}.cvm"
            compiled = run(
                [str(compiler), str(source_path), str(bytecode_path)],
                project,
            )
            if compiled.returncode != 0:
                detail = compiled.stderr.decode("utf-8", errors="replace").strip()
                results.append(Result(name, "compile_failed", detail))
                continue

            runtime_command = [str(runtime), str(bytecode_path)]
            if name == "31_args":
                runtime_command.extend(["-", "arg1", "arg2", "arg3", "arg4"])
            # Programs using stdio write into the isolated test directory,
            # never into the source tree.
            executed = run(runtime_command, temporary_path)
            if executed.returncode != 0:
                detail = executed.stderr.decode("utf-8", errors="replace").strip()
                results.append(Result(name, "runtime_failed", detail))
                continue

            expected = expected_path.read_bytes().replace(b"\r\n", b"\n")
            actual = executed.stdout.replace(b"\r\n", b"\n")
            if diff_b_normalize(actual) != diff_b_normalize(expected):
                results.append(
                    Result(
                        name,
                        "output_mismatch",
                        f"expected {len(expected)} bytes, got {len(actual)} bytes",
                    )
                )
                continue
            results.append(Result(name, "passed"))

    totals: dict[str, int] = {}
    for result in results:
        totals[result.status] = totals.get(result.status, 0) + 1
    report = {
        "suite": str(arguments.suite.resolve()),
        "total": len(results),
        "totals": totals,
        "results": [asdict(result) for result in results],
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    for status, count in sorted(totals.items()):
        print(f"{status}: {count}")
    print(f"report: {arguments.report}")
    return 0 if totals.get("passed", 0) == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
