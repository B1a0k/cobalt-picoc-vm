#!/usr/bin/env python3
"""Verify x86/x64 emission, layout, execution, and isolation as one contract."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path


HEADER = struct.Struct("<IHHBBBBIIIIIIII")
SECTION = struct.Struct("<HHIIII")
FUNCTION = struct.Struct("<IIIIHBBIHH")
PARAMETER = struct.Struct("<IIBBH")
SECTION_STRINGS = 1
SECTION_FUNCTIONS = 5
SECTION_PARAMETERS = 6


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


def sections(package: bytes) -> dict[int, tuple[int, int, int, int]]:
    header = HEADER.unpack_from(package)
    result: dict[int, tuple[int, int, int, int]] = {}
    for index in range(header[8]):
        at = HEADER.size + index * SECTION.size
        kind, _, offset, size, count, entry_size = SECTION.unpack_from(
            package, at
        )
        result[kind] = (offset, size, count, entry_size)
    return result


def c_string(data: bytes, offset: int) -> str:
    end = data.index(b"\0", offset)
    return data[offset:end].decode("utf-8")


def pe_machine(path: Path) -> int:
    image = path.read_bytes()
    if image[:2] != b"MZ" or len(image) < 0x40:
        raise ValueError(f"{path} is not a PE image")
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"{path} has no PE signature")
    return struct.unpack_from("<H", image, pe_offset + 4)[0]


def probe_parameter_offsets(package: bytes) -> tuple[list[int], int, int]:
    directory = sections(package)
    strings_at, strings_size, _, _ = directory[SECTION_STRINGS]
    strings = package[strings_at : strings_at + strings_size]
    function_at, _, function_count, function_size = directory[
        SECTION_FUNCTIONS
    ]
    parameter_at, _, _, parameter_size = directory[SECTION_PARAMETERS]
    if function_size != FUNCTION.size or parameter_size != PARAMETER.size:
        raise ValueError("unexpected descriptor size")
    for index in range(function_count):
        fields = FUNCTION.unpack_from(
            package, function_at + index * function_size
        )
        if c_string(strings, fields[0]) != "layout_probe":
            continue
        first_parameter = fields[3]
        parameter_count = fields[4]
        offsets: list[int] = []
        for parameter in range(parameter_count):
            descriptor = PARAMETER.unpack_from(
                package,
                parameter_at
                + (first_parameter + parameter) * parameter_size,
            )
            offsets.append(descriptor[1])
        return offsets, fields[7], fields[8]
    raise ValueError("layout_probe function was not found")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--build-root",
        type=Path,
        default=root / "build",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=root / "build" / "architecture-test-report.json",
    )
    arguments = parser.parse_args()

    source = root / "tests" / "architecture" / "target_layout.c"
    tools = {
        architecture: {
            "compiler": arguments.build_root / architecture / "cvmc.exe",
            "runtime": arguments.build_root / architecture / "cvmrun.exe",
            "smoke": arguments.build_root / architecture / "smoke_vm.exe",
        }
        for architecture in ("x86", "x64")
    }
    for architecture, paths in tools.items():
        for role, path in paths.items():
            if not path.is_file():
                raise SystemExit(
                    f"{architecture} {role} is missing: {path}"
                )

    expected = {
        "x86": {
            "arch": 1,
            "pointer": 4,
            "output": b"4 12 8 8 12\n",
            "parameters": [0, 4, 8],
            "local_bytes": 12,
            "alignment": 4,
            "machine": 0x014C,
        },
        "x64": {
            "arch": 2,
            "pointer": 8,
            "output": b"8 24 16 8 24\n",
            "parameters": [0, 8, 16],
            "local_bytes": 20,
            "alignment": 8,
            "machine": 0x8664,
        },
    }

    results: list[Result] = []
    with tempfile.TemporaryDirectory(prefix="cvm-architectures-") as folder:
        temporary = Path(folder)
        packages: dict[str, Path] = {}
        package_bytes: dict[str, bytes] = {}

        for architecture in ("x86", "x64"):
            model = expected[architecture]
            try:
                machines = {
                    role: pe_machine(path)
                    for role, path in tools[architecture].items()
                }
            except ValueError as error:
                results.append(
                    Result(
                        f"{architecture}_pe_binaries",
                        "failed",
                        str(error),
                    )
                )
            else:
                wrong = {
                    role: f"0x{machine:04x}"
                    for role, machine in machines.items()
                    if machine != model["machine"]
                }
                if wrong:
                    results.append(
                        Result(
                            f"{architecture}_pe_binaries",
                            "failed",
                            str(wrong),
                        )
                    )
                else:
                    results.append(
                        Result(f"{architecture}_pe_binaries", "passed")
                    )

            package = temporary / f"default-{architecture}.cvm"
            compiled = run(
                [str(tools[architecture]["compiler"]), str(source), str(package)],
                root,
            )
            if compiled.returncode != 0:
                results.append(
                    Result(
                        f"{architecture}_default_compile",
                        "failed",
                        compiled.stderr.decode(
                            "utf-8", errors="replace"
                        ).strip(),
                    )
                )
                continue
            results.append(
                Result(f"{architecture}_default_compile", "passed")
            )
            packages[architecture] = package
            package_bytes[architecture] = package.read_bytes()

            header = HEADER.unpack_from(package_bytes[architecture])
            if header[3] != model["arch"] or header[4] != model["pointer"]:
                results.append(
                    Result(
                        f"{architecture}_package_header",
                        "failed",
                        f"arch={header[3]} pointer={header[4]}",
                    )
                )
            else:
                results.append(
                    Result(f"{architecture}_package_header", "passed")
                )

            try:
                offsets, local_bytes, alignment = probe_parameter_offsets(
                    package_bytes[architecture]
                )
            except (KeyError, ValueError, struct.error) as error:
                results.append(
                    Result(
                        f"{architecture}_frame_layout",
                        "failed",
                        str(error),
                    )
                )
            else:
                if (
                    offsets != model["parameters"]
                    or local_bytes != model["local_bytes"]
                    or alignment != model["alignment"]
                ):
                    results.append(
                        Result(
                            f"{architecture}_frame_layout",
                            "failed",
                            (
                                f"offsets={offsets} local_bytes={local_bytes} "
                                f"alignment={alignment}"
                            ),
                        )
                    )
                else:
                    results.append(
                        Result(f"{architecture}_frame_layout", "passed")
                    )

            executed = run(
                [str(tools[architecture]["runtime"]), str(package)],
                temporary,
            )
            if (
                executed.returncode != 0
                or executed.stdout.replace(b"\r\n", b"\n")
                != model["output"]
            ):
                results.append(
                    Result(
                        f"{architecture}_layout_execution",
                        "failed",
                        (
                            executed.stderr.decode(
                                "utf-8", errors="replace"
                            ).strip()
                            or repr(executed.stdout)
                        ),
                    )
                )
            else:
                results.append(
                    Result(f"{architecture}_layout_execution", "passed")
                )

        for architecture, other in (("x86", "x64"), ("x64", "x86")):
            if architecture not in packages:
                continue
            rejected = run(
                [
                    str(tools[other]["runtime"]),
                    str(packages[architecture]),
                ],
                temporary,
            )
            diagnostic = rejected.stderr.decode(
                "utf-8", errors="replace"
            ).lower()
            if (
                rejected.returncode == 0
                or "architecture mismatch" not in diagnostic
            ):
                results.append(
                    Result(
                        f"{other}_rejects_{architecture}",
                        "failed",
                        diagnostic.strip(),
                    )
                )
            else:
                results.append(
                    Result(f"{other}_rejects_{architecture}", "passed")
                )

        for target, compiler_architecture in (
            ("x86", "x64"),
            ("x64", "x86"),
        ):
            if target not in package_bytes:
                continue
            package = temporary / f"cross-{compiler_architecture}-{target}.cvm"
            compiled = run(
                [
                    str(tools[compiler_architecture]["compiler"]),
                    "--target",
                    target,
                    str(source),
                    str(package),
                ],
                root,
            )
            if compiled.returncode != 0:
                results.append(
                    Result(
                        f"{compiler_architecture}_emits_{target}",
                        "failed",
                        compiled.stderr.decode(
                            "utf-8", errors="replace"
                        ).strip(),
                    )
                )
            elif package.read_bytes() != package_bytes[target]:
                results.append(
                    Result(
                        f"{compiler_architecture}_emits_{target}",
                        "failed",
                        "cross-target package differs from native-target package",
                    )
                )
            else:
                results.append(
                    Result(
                        f"{compiler_architecture}_emits_{target}",
                        "passed",
                    )
                )

        invalid_target = run(
            [
                str(tools["x64"]["compiler"]),
                "--target",
                "arm64",
                str(source),
                str(temporary / "invalid.cvm"),
            ],
            root,
        )
        if (
            invalid_target.returncode == 0
            or b"unknown target" not in invalid_target.stderr.lower()
        ):
            results.append(Result("invalid_target_rejected", "failed"))
        else:
            results.append(Result("invalid_target_rejected", "passed"))

    failed = sum(result.status != "passed" for result in results)
    report = {
        "total": len(results),
        "passed": len(results) - failed,
        "failed": failed,
        "results": [asdict(result) for result in results],
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(
        f"architectures: {report['passed']}/{report['total']} passed; "
        f"{report['failed']} failed"
    )
    print(f"report: {arguments.report}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
