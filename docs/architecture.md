# Architecture

## 1. Non-negotiable boundary

```text
C source and headers
        |
        v
  cvmc compiler
  - include resolver
  - preprocessor
  - lexer/parser
  - target data layout
  - semantic analysis
  - bytecode emitter
  - package writer
        |
        v
  versioned .cvm package
        |
        v
  cvm runtime
  - structural loader
  - semantic verifier
  - resource accounting
  - bytecode interpreter
  - host import boundary
```

The runtime never receives C source, header text, PicoC `ParseState`, host
pointers from the compiler, or an unserialized AST.

## 2. Compatibility profiles

The compiler and runtime share one architecture but expose feature profiles.

### `picoc-compat`

Used to run the upstream PicoC regression suite. It may enable floating point
and the PicoC standard-library compatibility surface.

### `beacon`

Matches the documented C-script constraints:

- integer and pointer operations;
- x86 and x64 target layouts;
- no script floating point;
- no aggregate by-value calls/returns;
- no script-defined native callbacks;
- native calls only through validated signatures;
- finite stack, call depth, globals, locals, and instruction budget.

Profiles gate semantic features. They do not select different parsers or
different VM implementations.

## 3. Target model

Every package declares:

- byte order;
- target architecture;
- target pointer size;
- language/VM feature bits;
- bytecode format major/minor version.

The VM execution cell is always 64 bits. C object layout remains target
specific:

| C type | x86 target | x64 target |
|---|---:|---:|
| `char` | 1 | 1 |
| `short` | 2 | 2 |
| `int` | 4 | 4 |
| `long` | 4 | 4 |
| `long long` | 8 | 8 |
| pointer | 4 | 8 |
| `size_t` | 4 | 8 |

This preserves Windows LLP64 while keeping the operand stack simple.

## 4. Memory and pointer model

The package contains only section-relative offsets and numeric identifiers.
At load time the runtime creates:

- immutable constant data;
- mutable global data;
- bounded call-frame/local storage;
- an operand stack.

An lvalue is compiled as an address operation followed by a typed load/store.
Examples:

```text
a              ADDR_LOCAL a; LOAD_I32
&a             ADDR_LOCAL a
*p             LOAD_LOCAL_PTR p; LOAD_I32
p[i]           LOAD p; LOAD i; PTR_INDEX sizeof(*p); LOAD_<type>
object.field   ADDR object; PTR_ADD field_offset; LOAD_<type>
```

The standalone VM validates VM-owned object ranges. Foreign native pointers
remain inaccessible unless the host explicitly approves the exact range
through `CvmHost.memory_access`. The command-line host uses this for `argv`;
a Beacon integration can use the same capability for Beacon-owned RW buffers
without enabling unrestricted native memory access.

## 5. Execution model

Top-level statements are compiled into a synthetic entry function. User
functions have table entries containing:

- code range;
- return type;
- parameter descriptors;
- local-frame byte size and alignment;
- maximum operand-stack contribution;
- flags.

The interpreter is a conventional stack machine:

```text
PUSH 2
PUSH 3
CALL add

add:
  ENTER
  LOAD_ARG 0
  LOAD_ARG 1
  ADD_I32
  STORE_LOCAL c
  LOAD_LOCAL c
  RET_I32
```

Control-flow syntax is lowered by the compiler:

- `if`, loops, and `goto` become validated branches;
- `switch` becomes comparisons or a jump-table section;
- short-circuit expressions become conditional branches;
- declarations and typedefs produce no runtime instruction unless storage or
  initialization is required.

## 6. Package sections

The format uses a fixed header followed by a section directory. Defined and
reserved section kinds are:

- strings;
- constants;
- data initialization;
- global descriptors;
- function descriptors;
- parameter/type descriptors;
- code;
- native imports;
- native signatures;
- relocations;
- source/debug mapping.

Unknown optional sections may be skipped. Unknown required sections reject the
package. All ranges, counts, alignments, branch targets, indices, and resource
claims are verified before execution.

## 7. Native-call boundary

Native calls are not ordinary VM instructions with embedded addresses.

Direct calls use:

```text
CALL_IMPORT import_id
```

An import record contains symbolic library/function names and a signature ID.
The runtime resolver supplies the address.

Typed indirect calls use:

```text
CALL_NATIVE_INDIRECT signature_id
```

The address is a runtime value while the signature is compiler-produced,
validated package metadata. This supports the documented
`GetProcAddress`-to-typed-function-pointer pattern without serializing an
address.

The initial standalone host surface implements PicoC-compatible library
functions. Beacon/Win32 resolution and ABI call gates are separate adapters and
do not change the bytecode format.

## 8. Compiler organization

The frontend is derived from PicoC semantics but is not a line-for-line port of
its evaluator. The current implementation is a typed recursive parser that
lowers directly to bytecode with relocation/fixup tables; it does not retain a
large AST. Its logical boundaries are:

1. preprocessing and include resolution;
2. lexical tokens with source spans;
3. declarations and a canonical type graph;
4. typed expression and declaration lowering;
5. control-flow lowering;
6. bytecode emission and fixups;
7. package serialization.

PicoC's immediate operations such as:

```c
ResultInt = BottomInt + TopInt;
```

become typed IR/bytecode emission:

```text
emit ADD_I32
```

No compile-time value is substituted unless ordinary C constant-expression
rules permit it.

## 9. Verification and resource model

Before execution the current verifier checks:

- package version, endianness, pointer width, and total size;
- required sections and in-range section layouts;
- known opcode/type combinations;
- valid function/import/signature/string indices;
- branch targets on instruction boundaries;
- function/code/parameter ranges and branch containment;
- operand-stack underflow, declared maximum depth, and control-flow join depth;
- local/global resource bounds;
- call argument/return compatibility;
- declared resource limits.

Runtime limits include:

- instruction budget;
- operand stack cells;
- call depth;
- total local bytes;
- global bytes;
- native-call argument count.

The format reserves a checksum field and debug/relocation sections. Checksum
enforcement and typed stack-shape analysis beyond depth remain
integration-hardening work. x86/x64 target emission, native VM builds, and
cross-architecture rejection are implemented. Depth analysis and malformed
control-flow rejection are covered by package mutations.

## 10. Testing strategy

The project uses complementary test layers:

1. frozen language/host semantic cases;
2. native Clang differential checks;
3. deterministic generated properties;
4. negative compiler and runtime contracts;
5. package format/verifier mutations;
6. opcode and symbolic-import metadata coverage;
7. deterministic package and source-separation checks;
8. the vendored upstream `tests/upstream-picoc` compatibility corpus.

The harness records compile, verify, execute, output, and exit-status failures
separately. The PicoC executable may be used as a differential oracle, but its
output is never embedded into generated bytecode.
