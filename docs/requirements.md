# Consolidated requirements

## Product outcome

Accept C source plus controlled headers on the Teamserver, compile it into a
portable and validated intermediate bytecode package, transport that package,
and interpret it inside a 32-bit or 64-bit Beacon without generating native
executable code.

## Compiler requirements

- Independent command-line/library boundary; the Java Teamserver is not coupled
  to compiler internals.
- PicoC-compatible practical C frontend with deterministic diagnostics.
- Top-level statements compiled into an explicit package entry function.
- Target-aware Windows LLP64 type and aggregate layout.
- Includes and macros resolved at compile time; headers are not sent to Beacon.
- DFR declarations lowered into native import/signature metadata.
- Direct and typed-indirect native calls represented separately.
- All serialized references use IDs or offsets.
- Emit enough metadata for complete pre-execution verification.
- `picoc-compat` and `beacon` profiles share the same frontend and backend.

## Runtime requirements

- Standalone embeddable VM with no source parser.
- Interpreter only; bytecode remains non-executable data.
- Exact package validation before execution.
- Bounded operand stack, call depth, local/global storage, and instruction
  count.
- Target pointer-width enforcement.
- Stable host-import interface independent of BOF/Beacon implementation.
- No dependency on the Teamserver language or process.

## Compatibility requirements

- Run the normal `tests/upstream-picoc` corpus through
  `cvmc -> .cvm -> cvmrun`.
- Treat float-related tests as `picoc-compat`; ensure the Beacon profile rejects
  the same source with a clear diagnostic.
- Preserve stdout, stderr, exit behavior, and argument handling where the
  PicoC tests depend on them.
- Keep x86/x64 layout, architecture isolation, and malformed-bytecode tests
  passing before native Win32 integration.

## Current compatibility baseline

- All 67 root fixtures under `tests/upstream-picoc` pass through generated `.cvm`
  bytecode and the independent VM.
- The original PicoC executable may be used only as a differential oracle when
  investigating fixture behavior; it is never invoked by `cvmc`, `cvmrun`, or
  the normal test command.
- The compiler emits x86 and x64 packages. Native 32-bit and 64-bit VMs execute
  only matching packages, while either compiler binary may cross-emit either
  target. The format carries architecture, pointer width, profile, feature,
  calling-convention, import, and signature metadata.
- `CALL_NATIVE_INDIRECT` is implemented using compiler-produced,
  verifier-checked signatures. The checksum/debug/relocation section IDs remain
  reserved in the stable contract.
- Windows native ABI dispatch is a separate reusable adapter and is covered on
  x86 and x64; product-specific symbol policy remains outside the VM core.

## Integration boundary

- Java `ScriptCompileService` invokes the isolated compiler and receives
  bytecode plus structured diagnostics.
- Beacon embeds only the loader, verifier, VM, allocator adapter, and host-call
  adapter.
- Existing Beacon/BOF symbol resolution supplies native addresses.
- A dedicated dynamic-invoke layer handles cdecl, stdcall, Win64, native
  varargs, and typed indirect calls.
