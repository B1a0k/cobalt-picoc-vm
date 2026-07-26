#!/usr/bin/env python3
"""Independent secondary verification for cvmc and cvmrun.

This suite deliberately mixes frozen semantic fixtures, generated properties,
negative contracts, runtime isolation checks, and black-box package mutation.
It never invokes PicoC or derives expected results from the VM under test.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import struct
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path


HEADER = struct.Struct("<IHHBBBBIIIIIIII")
SECTION = struct.Struct("<HHIIII")
INSTRUCTION = struct.Struct("<BBHii")
IMPORT = struct.Struct("<IIII")
SIGNATURE = struct.Struct("<IHBBI")
MAGIC = 0x314D5643
SECTION_STRINGS = 1
SECTION_DATA = 3
SECTION_FUNCTIONS = 5
SECTION_PARAMETERS = 6
SECTION_CODE = 7
SECTION_IMPORTS = 8
SECTION_SIGNATURES = 9
SECTION_SIGNATURE_PARAMETERS = 10
OP_JUMP = 44
OP_CALL = 47


@dataclass
class Result:
    layer: str
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


def normalized(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n")


def expected_for_target(expected: Path, target: str) -> Path:
    target_specific = expected.with_name(expected.name + f".{target}")
    return target_specific if target_specific.is_file() else expected


def compile_source(
    compiler: Path,
    source: Path,
    package: Path,
    cwd: Path,
) -> subprocess.CompletedProcess[bytes]:
    return run([str(compiler), str(source), str(package)], cwd)


def execute_package(
    runtime: Path,
    package: Path,
    cwd: Path,
    extra: list[str] | None = None,
    script_args: list[str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    command = [str(runtime)]
    if extra:
        command.extend(extra)
    command.append(str(package))
    if script_args:
        command.extend(script_args)
    return run(command, cwd)


def semantic_cases(
    compiler: Path,
    runtime: Path,
    root: Path,
    temporary: Path,
    target: str,
) -> tuple[list[Result], Path | None, dict[str, Path]]:
    results: list[Result] = []
    baseline: Path | None = None
    packages: dict[str, Path] = {}
    positive = root / "tests" / "extended" / "positive"
    for expected_path in sorted(positive.glob("*.expect")):
        name = expected_path.stem
        source = expected_path.with_suffix(".c")
        first = temporary / f"{name}-first.cvm"
        second = temporary / f"{name}-second.cvm"
        compiled = compile_source(compiler, source, first, root)
        if compiled.returncode != 0:
            results.append(
                Result(
                    "semantic",
                    name,
                    "compile_failed",
                    compiled.stderr.decode("utf-8", errors="replace").strip(),
                )
            )
            continue
        repeated = compile_source(compiler, source, second, root)
        if repeated.returncode != 0 or first.read_bytes() != second.read_bytes():
            results.append(
                Result(
                    "reproducibility",
                    name,
                    "failed",
                    "two compilations did not produce identical packages",
                )
            )
            continue
        package_bytes = first.read_bytes()
        if b"#include" in package_bytes or b"int main(" in package_bytes:
            results.append(
                Result(
                    "source_separation",
                    name,
                    "failed",
                    "source-language text was found in the package",
                )
            )
            continue
        args_path = expected_path.with_suffix(".args")
        script_args = (
            args_path.read_text(encoding="utf-8").splitlines()
            if args_path.is_file()
            else None
        )
        executed = execute_package(
            runtime,
            first,
            temporary,
            script_args=script_args,
        )
        if executed.returncode != 0:
            results.append(
                Result(
                    "semantic",
                    name,
                    "runtime_failed",
                    executed.stderr.decode("utf-8", errors="replace").strip(),
                )
            )
            continue
        expected = normalized(
            expected_for_target(expected_path, target).read_bytes()
        )
        actual = normalized(executed.stdout)
        if actual != expected:
            results.append(
                Result(
                    "semantic",
                    name,
                    "output_mismatch",
                    f"expected {expected!r}, got {actual!r}",
                )
            )
            continue
        results.append(Result("semantic", name, "passed"))
        results.append(Result("reproducibility", name, "passed"))
        results.append(Result("source_separation", name, "passed"))
        packages[name] = first
        if name == "07_functions_and_conversions" or baseline is None:
            baseline = first
    return results, baseline, packages


def read_c_string(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        raise ValueError("string offset is outside the table")
    end = table.find(b"\0", offset)
    if end < 0:
        raise ValueError("string is not terminated")
    return table[offset:end].decode("utf-8")


def import_metadata_contract(packages: dict[str, Path]) -> list[Result]:
    name = "09_beacon_style_buffer"
    package_path = packages.get(name)
    if package_path is None:
        return [
            Result(
                "import_metadata",
                name,
                "blocked",
                "business buffer package was not produced",
            )
        ]
    package = package_path.read_bytes()
    sections = {
        fields[0]: fields
        for _, fields in section_directory(package)
    }
    required = {
        SECTION_STRINGS,
        SECTION_IMPORTS,
        SECTION_SIGNATURES,
        SECTION_SIGNATURE_PARAMETERS,
    }
    if not required.issubset(sections):
        return [
            Result(
                "import_metadata",
                name,
                "failed",
                "required import metadata sections are missing",
            )
        ]
    strings_fields = sections[SECTION_STRINGS]
    strings = package[
        strings_fields[2] : strings_fields[2] + strings_fields[3]
    ]
    import_fields = sections[SECTION_IMPORTS]
    signature_fields = sections[SECTION_SIGNATURES]
    signature_parameter_fields = sections[
        SECTION_SIGNATURE_PARAMETERS
    ]
    signature_parameters = package[
        signature_parameter_fields[2] :
        signature_parameter_fields[2] + signature_parameter_fields[3]
    ]
    imports: list[tuple[str, str, int]] = []
    for offset in range(
        import_fields[2],
        import_fields[2] + import_fields[3],
        IMPORT.size,
    ):
        library_offset, symbol_offset, signature_index, flags = (
            IMPORT.unpack_from(package, offset)
        )
        imports.append(
            (
                read_c_string(strings, library_offset),
                read_c_string(strings, symbol_offset),
                signature_index,
            )
        )
    expected_symbols = {
        "memset",
        "memcpy",
        "sprintf",
        "printf",
        "memcmp",
    }
    actual_symbols = {symbol for _, symbol, _ in imports}
    if not expected_symbols.issubset(actual_symbols):
        return [
            Result(
                "import_metadata",
                name,
                "failed",
                f"missing symbols: {sorted(expected_symbols - actual_symbols)}",
            )
        ]
    signature_count = signature_fields[3] // SIGNATURE.size
    for library, symbol, signature_index in imports:
        if library != "PICOC" or signature_index >= signature_count:
            return [
                Result(
                    "import_metadata",
                    name,
                    "failed",
                    f"invalid import {library}${symbol}",
                )
            ]
        signature_offset = (
            signature_fields[2] +
            signature_index * SIGNATURE.size
        )
        first_parameter, parameter_count, return_type, convention, flags = (
            SIGNATURE.unpack_from(package, signature_offset)
        )
        if (
            convention != 1
            or return_type == 0
            or first_parameter + parameter_count >
                len(signature_parameters)
            or any(
                value == 0
                for value in signature_parameters[
                    first_parameter :
                    first_parameter + parameter_count
                ]
            )
        ):
            return [
                Result(
                    "import_metadata",
                    name,
                    "failed",
                    f"invalid signature for {symbol}",
                )
            ]
    return [
        Result(
            "import_metadata",
            name,
            "passed",
            f"{len(imports)} symbolic imports checked",
        )
    ]


def opcode_coverage_contract(packages: dict[str, Path]) -> list[Result]:
    covered: set[int] = set()
    for package_path in packages.values():
        package = package_path.read_bytes()
        sections = {
            fields[0]: fields
            for _, fields in section_directory(package)
        }
        code = sections.get(SECTION_CODE)
        if code is None:
            continue
        for offset in range(code[2], code[2] + code[3], INSTRUCTION.size):
            covered.add(package[offset])

    required = {
        2, 3, 4, 5, 6,          # stack/immediates
        8, 9, 11, 12,           # addresses and pointer operations
        13, 14, 15,             # load/store/copy
        *range(16, 28),          # numeric and bitwise operations
        *range(30, 50),          # shifts, compares, convert, flow, calls
    }
    missing = sorted(required - covered)
    if missing:
        return [
            Result(
                "opcode_coverage",
                "compiler_emittable_opcodes",
                "failed",
                f"missing opcode numbers: {missing}",
            )
        ]
    return [
        Result(
            "opcode_coverage",
            "compiler_emittable_opcodes",
            "passed",
            f"{len(required)} required opcodes covered",
        )
    ]


def native_differential(
    compiler: Path | None,
    root: Path,
    temporary: Path,
    target: str,
) -> list[Result]:
    if compiler is None:
        return []
    results: list[Result] = []
    positive = root / "tests" / "extended" / "positive"
    pico_specific = {
        "08_float_semantics",
        "10_arguments_and_foreign_memory",
    }
    for expected_path in sorted(positive.glob("*.expect")):
        name = expected_path.stem
        if name in pico_specific:
            continue
        source = expected_path.with_suffix(".c")
        executable = temporary / f"native-{name}.exe"
        compiled = run(
            [
                str(compiler),
                "-m32" if target == "x86" else "-m64",
                "-std=c11",
                "-Wno-constant-conversion",
                "-Wno-format",
                str(source),
                "-o",
                str(executable),
            ],
            root,
        )
        if compiled.returncode != 0:
            results.append(
                Result(
                    "native_differential",
                    name,
                    "compile_failed",
                    compiled.stderr.decode(
                        "utf-8", errors="replace"
                    ).strip(),
                )
            )
            continue
        args_path = expected_path.with_suffix(".args")
        script_args = (
            args_path.read_text(encoding="utf-8").splitlines()
            if args_path.is_file()
            else []
        )
        executed = run(
            [str(executable), *script_args],
            temporary,
        )
        expected = normalized(
            expected_for_target(expected_path, target).read_bytes()
        )
        if (
            executed.returncode != 0
            or normalized(executed.stdout) != expected
        ):
            results.append(
                Result(
                    "native_differential",
                    name,
                    "output_mismatch",
                    (
                        executed.stderr.decode(
                            "utf-8", errors="replace"
                        ).strip()
                        or repr(normalized(executed.stdout))
                    ),
                )
            )
        else:
            results.append(
                Result("native_differential", name, "passed")
            )
    return results


def compiler_contracts(
    compiler: Path,
    root: Path,
    temporary: Path,
) -> list[Result]:
    results: list[Result] = []
    for layer, folder in (
        ("negative_compile", "negative"),
        ("unsupported_contract", "unsupported"),
    ):
        directory = root / "tests" / "extended" / folder
        for pattern_path in sorted(directory.glob("*.error")):
            name = pattern_path.stem
            source = pattern_path.with_suffix(".c")
            package = temporary / f"{folder}-{name}.cvm"
            compiled = compile_source(compiler, source, package, root)
            pattern = pattern_path.read_text(encoding="utf-8").strip()
            diagnostic = compiled.stderr.decode(
                "utf-8", errors="replace"
            )
            if compiled.returncode == 0:
                results.append(
                    Result(layer, name, "unexpected_success")
                )
            elif re.search(pattern, diagnostic, re.IGNORECASE) is None:
                results.append(
                    Result(
                        layer,
                        name,
                        "wrong_diagnostic",
                        diagnostic.strip(),
                    )
                )
            else:
                results.append(Result(layer, name, "passed"))
    return results


def runtime_contracts(
    compiler: Path,
    runtime: Path,
    root: Path,
    temporary: Path,
) -> list[Result]:
    results: list[Result] = []
    directory = root / "tests" / "extended" / "runtime_failure"
    for pattern_path in sorted(directory.glob("*.error")):
        name = pattern_path.stem
        source = pattern_path.with_suffix(".c")
        package = temporary / f"runtime-{name}.cvm"
        compiled = compile_source(compiler, source, package, root)
        if compiled.returncode != 0:
            results.append(
                Result(
                    "runtime_contract",
                    name,
                    "compile_failed",
                    compiled.stderr.decode(
                        "utf-8", errors="replace"
                    ).strip(),
                )
            )
            continue
        options = (
            ["--instruction-budget", "1000"]
            if name == "03_instruction_limit"
            else None
        )
        executed = execute_package(runtime, package, temporary, options)
        pattern = pattern_path.read_text(encoding="utf-8").strip()
        diagnostic = executed.stderr.decode("utf-8", errors="replace")
        if executed.returncode == 0:
            results.append(
                Result("runtime_contract", name, "unexpected_success")
            )
        elif re.search(pattern, diagnostic, re.IGNORECASE) is None:
            results.append(
                Result(
                    "runtime_contract",
                    name,
                    "wrong_status",
                    diagnostic.strip(),
                )
            )
        else:
            results.append(Result("runtime_contract", name, "passed"))
    return results


def generated_properties(
    compiler: Path,
    runtime: Path,
    root: Path,
    temporary: Path,
) -> list[Result]:
    randomizer = random.Random(0xC0BA17)
    expressions: list[tuple[str, int]] = []
    for _ in range(24):
        a = randomizer.randrange(0, 1000)
        b = randomizer.randrange(1, 100)
        c = randomizer.randrange(0, 32)
        shift = randomizer.randrange(0, 5)
        expressions.extend(
            [
                (f"({a} + {b} * {c})", a + b * c),
                (f"(({a} - {b}) / {b})", int((a - b) / b)),
                (f"({a} % {b})", a % b),
                (f"(({a} << {shift}) ^ {c})", (a << shift) ^ c),
                (f"(({a} & 255) | {c})", (a & 255) | c),
                (f"({a} < {b})", int(a < b)),
                (f"({a} == {b} ? {c} : {shift})", c if a == b else shift),
            ]
        )

    source_lines = ["#include <stdio.h>", "int main(void) {"]
    expected_lines: list[str] = []
    for expression, expected in expressions:
        source_lines.append(f'    printf("%d\\n", {expression});')
        expected_lines.append(str(expected))
    source_lines.extend(["    return 0;", "}"])
    source = temporary / "generated-expressions.c"
    source.write_text("\n".join(source_lines) + "\n", encoding="utf-8")
    package = temporary / "generated-expressions.cvm"
    compiled = compile_source(compiler, source, package, root)
    if compiled.returncode != 0:
        return [
            Result(
                "generated_property",
                "expressions_seed_c0ba17",
                "compile_failed",
                compiled.stderr.decode("utf-8", errors="replace").strip(),
            )
        ]
    executed = execute_package(runtime, package, temporary)
    expected_output = ("\n".join(expected_lines) + "\n").encode()
    if executed.returncode != 0:
        return [
            Result(
                "generated_property",
                "expressions_seed_c0ba17",
                "runtime_failed",
                executed.stderr.decode("utf-8", errors="replace").strip(),
            )
        ]
    if normalized(executed.stdout) != expected_output:
        return [
            Result(
                "generated_property",
                "expressions_seed_c0ba17",
                "output_mismatch",
                "168 independently evaluated expressions diverged",
            )
        ]

    values = [randomizer.randrange(0, 100) for _ in range(48)]
    expected_sum = sum(
        value * (index + 1)
        for index, value in enumerate(values)
        if value % 3 != 0
    )
    array_source = temporary / "generated-array-loop.c"
    array_source.write_text(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        f"  int values[48] = {{{', '.join(map(str, values))}}};\n"
        "  int i; int total = 0;\n"
        "  for (i = 0; i < 48; i++) {\n"
        "    if (values[i] % 3 == 0) continue;\n"
        "    total += values[i] * (i + 1);\n"
        "  }\n"
        '  printf("%d\\n", total);\n'
        "  return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    array_package = temporary / "generated-array-loop.cvm"
    compiled = compile_source(
        compiler, array_source, array_package, root
    )
    if compiled.returncode != 0:
        return [
            Result(
                "generated_property",
                "array_loop_seed_c0ba17",
                "compile_failed",
                compiled.stderr.decode("utf-8", errors="replace").strip(),
            )
        ]
    executed = execute_package(runtime, array_package, temporary)
    if (
        executed.returncode != 0
        or normalized(executed.stdout)
        != f"{expected_sum}\n".encode()
    ):
        return [
            Result(
                "generated_property",
                "array_loop_seed_c0ba17",
                "failed",
                (
                    executed.stderr.decode("utf-8", errors="replace").strip()
                    or repr(normalized(executed.stdout))
                ),
            )
        ]
    return [
        Result(
            "generated_property",
            "expressions_seed_c0ba17",
            "passed",
            f"{len(expressions)} expressions",
        ),
        Result(
            "generated_property",
            "array_loop_seed_c0ba17",
            "passed",
            "48 generated values",
        ),
    ]


def crlf_preprocessor_contract(
    compiler: Path,
    runtime: Path,
    root: Path,
    temporary: Path,
) -> list[Result]:
    case_root = temporary / "crlf-preprocessor"
    case_root.mkdir()
    header = case_root / "macros.h"
    header.write_bytes(
        b"#define ADD_LINES(left, right) \\\r\n"
        b"    ((left) + \\\r\n"
        b"     (right))\r\n"
    )
    source = case_root / "main.c"
    source.write_bytes(
        b"#include <stdio.h>\r\n"
        b"#include \"macros.h\"\r\n"
        b"int main(void) {\r\n"
        b"  printf(\"%d\\n\", ADD_LINES(2, 3));\r\n"
        b"  return 0;\r\n"
        b"}\r\n"
    )
    package = case_root / "main.cvm"
    compiled = compile_source(compiler, source, package, root)
    if compiled.returncode != 0:
        return [
            Result(
                "preprocessor_contract",
                "crlf_line_continuation",
                "compile_failed",
                compiled.stderr.decode("utf-8", errors="replace").strip(),
            )
        ]
    executed = execute_package(runtime, package, temporary)
    if executed.returncode != 0 or normalized(executed.stdout) != b"5\n":
        return [
            Result(
                "preprocessor_contract",
                "crlf_line_continuation",
                "failed",
                (
                    executed.stderr.decode("utf-8", errors="replace").strip()
                    or repr(normalized(executed.stdout))
                ),
            )
        ]
    return [
        Result(
            "preprocessor_contract",
            "crlf_line_continuation",
            "passed",
        )
    ]


def section_directory(package: bytes) -> list[tuple[int, list[int]]]:
    header = HEADER.unpack_from(package, 0)
    section_count = header[8]
    result: list[tuple[int, list[int]]] = []
    for index in range(section_count):
        offset = HEADER.size + index * SECTION.size
        result.append((offset, list(SECTION.unpack_from(package, offset))))
    return result


def package_mutations(
    runtime: Path,
    baseline: Path | None,
    temporary: Path,
) -> list[Result]:
    if baseline is None:
        return [
            Result(
                "package_mutation",
                "baseline",
                "blocked",
                "no semantic package compiled",
            )
        ]
    original = baseline.read_bytes()
    directories = section_directory(original)
    by_kind = {fields[0]: (offset, fields) for offset, fields in directories}
    mutations: list[tuple[str, bytes]] = []

    changed = bytearray(original)
    struct.pack_into("<I", changed, 0, 0)
    mutations.append(("bad_magic", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<H", changed, 4, 0x7FFF)
    mutations.append(("bad_version", bytes(changed)))

    changed = bytearray(original)
    changed[9] = 3
    mutations.append(("bad_pointer_width", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, 24, len(changed) + 1)
    mutations.append(("bad_package_size", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, 20, 0x7FFFFFFF)
    mutations.append(("invalid_entry_function", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, 28, 5 * 1024 * 1024)
    mutations.append(("excessive_global_requirement", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, 32, 5000)
    mutations.append(("excessive_stack_requirement", bytes(changed)))

    changed = bytearray(original)
    first_section = HEADER.size
    struct.pack_into("<I", changed, first_section + 4, len(changed) + 64)
    mutations.append(("section_out_of_range", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<H", changed, first_section, 0x7FFF)
    struct.pack_into("<H", changed, first_section + 2, 1)
    mutations.append(("unknown_required_section", bytes(changed)))

    code_offset, code_fields = by_kind[SECTION_CODE]
    code_data = code_fields[2]
    changed = bytearray(original)
    changed[code_data] = 0xFF
    mutations.append(("unknown_opcode", bytes(changed)))

    changed = bytearray(original)
    changed[code_data] = 4
    mutations.append(("static_stack_underflow", bytes(changed)))

    changed = bytearray(original)
    INSTRUCTION.pack_into(changed, code_data, 8, 11, 0, 0x7FFFFFFF, 0)
    mutations.append(("local_address_out_of_range", bytes(changed)))

    changed = bytearray(original)
    INSTRUCTION.pack_into(changed, code_data, OP_JUMP, 0, 0, 0x7FFFFFFF, 0)
    mutations.append(("branch_outside_function", bytes(changed)))

    changed = bytearray(original)
    call_found = False
    for at in range(code_data, code_data + code_fields[3], INSTRUCTION.size):
        opcode, value_type, flags, operand_a, operand_b = (
            INSTRUCTION.unpack_from(changed, at)
        )
        if opcode == OP_CALL:
            INSTRUCTION.pack_into(
                changed,
                at,
                opcode,
                value_type,
                flags,
                0x7FFFFFFF,
                operand_b,
            )
            call_found = True
            break
    if call_found:
        mutations.append(("invalid_call_target", bytes(changed)))

    changed = bytearray(original)
    for at in range(code_data, code_data + code_fields[3], INSTRUCTION.size):
        opcode, value_type, flags, operand_a, operand_b = (
            INSTRUCTION.unpack_from(changed, at)
        )
        if opcode == OP_CALL:
            INSTRUCTION.pack_into(
                changed,
                at,
                opcode,
                value_type,
                flags,
                operand_a,
                operand_b + 1,
            )
            mutations.append(("call_arity_mismatch", bytes(changed)))
            break

    function_header, function_fields = by_kind[SECTION_FUNCTIONS]
    function_data = function_fields[2]
    changed = bytearray(original)
    changed[function_data + 18] = 0xFF
    mutations.append(("invalid_function_return_type", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, function_data + 8, 0x7FFFFFFF)
    mutations.append(("function_code_range", bytes(changed)))

    changed = bytearray(original)
    struct.pack_into("<I", changed, function_data + 20, 2 * 1024 * 1024)
    mutations.append(("excessive_function_locals", bytes(changed)))

    if SECTION_PARAMETERS in by_kind:
        parameter_header, parameter_fields = by_kind[SECTION_PARAMETERS]
        if parameter_fields[3] >= 12:
            parameter_data = parameter_fields[2]
            changed = bytearray(original)
            changed[parameter_data + 8] = 0xFF
            mutations.append(("invalid_parameter_type", bytes(changed)))

            changed = bytearray(original)
            struct.pack_into(
                "<I", changed, parameter_data + 4, 0x7FFFFFFF
            )
            mutations.append(("parameter_frame_out_of_range", bytes(changed)))

    if SECTION_IMPORTS in by_kind:
        import_header, import_fields = by_kind[SECTION_IMPORTS]
        if import_fields[3] >= 16:
            changed = bytearray(original)
            struct.pack_into(
                "<I", changed, import_fields[2] + 8, 0x7FFFFFFF
            )
            mutations.append(("invalid_import_signature", bytes(changed)))

    if SECTION_SIGNATURES in by_kind:
        signature_header, signature_fields = by_kind[SECTION_SIGNATURES]
        if signature_fields[3] >= 12:
            changed = bytearray(original)
            changed[signature_fields[2] + 6] = 0xFF
            mutations.append(("invalid_signature_return", bytes(changed)))

            changed = bytearray(original)
            struct.pack_into(
                "<I", changed, signature_fields[2], 0x7FFFFFFF
            )
            mutations.append(("signature_parameter_range", bytes(changed)))

    if SECTION_SIGNATURE_PARAMETERS in by_kind:
        signature_parameter_header, signature_parameter_fields = by_kind[
            SECTION_SIGNATURE_PARAMETERS
        ]
        if signature_parameter_fields[3] > 0:
            changed = bytearray(original)
            changed[signature_parameter_fields[2]] = 0xFF
            mutations.append(("invalid_signature_parameter", bytes(changed)))

    if SECTION_DATA in by_kind and SECTION_STRINGS in by_kind:
        data_header, data_fields = by_kind[SECTION_DATA]
        _, string_fields = by_kind[SECTION_STRINGS]
        if data_fields[3] != 0 and string_fields[3] != 0:
            changed = bytearray(original)
            struct.pack_into("<I", changed, data_header + 4, string_fields[2])
            mutations.append(("overlapping_sections", bytes(changed)))

    mutations.append(("truncated_package", original[:-7]))

    results: list[Result] = []
    for name, mutated in mutations:
        path = temporary / f"mutated-{name}.cvm"
        path.write_bytes(mutated)
        executed = execute_package(runtime, path, temporary)
        diagnostic = executed.stderr.decode(
            "utf-8", errors="replace"
        ).lower()
        load_rejected = (
            "invalid package" in diagnostic
            or "unsupported version" in diagnostic
            or "architecture mismatch" in diagnostic
            or "verification failed" in diagnostic
        )
        if executed.returncode == 0:
            results.append(
                Result(
                    "package_mutation",
                    name,
                    "unexpected_success",
                )
            )
        elif load_rejected:
            results.append(Result("package_mutation", name, "passed"))
        else:
            results.append(
                Result(
                    "package_mutation",
                    name,
                    "late_runtime_rejection",
                    diagnostic.strip(),
                )
            )
    return results


def write_markdown(report: dict, path: Path) -> None:
    lines = [
        "# Extended test report",
        "",
        f"- Total checks: {report['total']}",
        f"- Passed checks: {report['passed']}",
        f"- Failed checks: {report['failed']}",
        f"- Unsupported capability contracts: "
        f"{report['unsupported_contracts']}",
        "",
        "| Layer | Passed | Failed |",
        "|---|---:|---:|",
    ]
    for layer, counts in sorted(report["layers"].items()):
        lines.append(
            f"| {layer} | {counts.get('passed', 0)} | "
            f"{counts.get('failed', 0)} |"
        )
    failures = [
        result
        for result in report["results"]
        if result["status"] != "passed"
    ]
    if failures:
        lines.extend(["", "## Failures", ""])
        for failure in failures:
            lines.append(
                f"- `{failure['layer']}/{failure['name']}`: "
                f"{failure['status']} — {failure['detail']}"
            )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--build",
        type=Path,
        default=root / "build" / "x64",
    )
    parser.add_argument("--target", choices=("x86", "x64"))
    parser.add_argument(
        "--report",
        type=Path,
        default=root / "build" / "x64" / "extended-test-report.json",
    )
    parser.add_argument("--native-compiler", type=Path)
    arguments = parser.parse_args()
    target = arguments.target
    if target is None:
        target = (
            arguments.build.name
            if arguments.build.name in ("x86", "x64")
            else "x64"
        )
    compiler = arguments.build / "cvmc.exe"
    runtime = arguments.build / "cvmrun.exe"
    if not compiler.is_file() or not runtime.is_file():
        raise SystemExit(
            f"{arguments.build}/cvmc.exe and cvmrun.exe are required"
        )

    results: list[Result] = []
    with tempfile.TemporaryDirectory(prefix="cvm-extended-") as folder:
        temporary = Path(folder)
        semantic, baseline, packages = semantic_cases(
            compiler, runtime, root, temporary, target
        )
        results.extend(semantic)
        results.extend(import_metadata_contract(packages))
        results.extend(opcode_coverage_contract(packages))
        results.extend(
            native_differential(
                arguments.native_compiler,
                root,
                temporary,
                target,
            )
        )
        results.extend(compiler_contracts(compiler, root, temporary))
        results.extend(
            runtime_contracts(
                compiler, runtime, root, temporary
            )
        )
        results.extend(
            generated_properties(
                compiler, runtime, root, temporary
            )
        )
        results.extend(
            crlf_preprocessor_contract(
                compiler, runtime, root, temporary
            )
        )
        results.extend(package_mutations(runtime, baseline, temporary))

    layers: dict[str, dict[str, int]] = {}
    for result in results:
        counts = layers.setdefault(result.layer, {"passed": 0, "failed": 0})
        counts["passed" if result.status == "passed" else "failed"] += 1
    failed = sum(result.status != "passed" for result in results)
    report = {
        "seed": "0xC0BA17",
        "target": target,
        "total": len(results),
        "passed": len(results) - failed,
        "failed": failed,
        "unsupported_contracts": sum(
            result.layer == "unsupported_contract"
            for result in results
        ),
        "layers": layers,
        "results": [asdict(result) for result in results],
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    markdown = arguments.report.with_suffix(".md")
    write_markdown(report, markdown)
    print(
        f"extended: {report['passed']}/{report['total']} passed; "
        f"{report['failed']} failed"
    )
    print(f"report: {arguments.report}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
