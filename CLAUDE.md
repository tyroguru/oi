# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Object Introspection (OI) is a memory profiling technology for C++ objects. It dynamically instruments running processes to capture precise memory occupancy of entire object hierarchies — with no code modification or recompilation. The primary tool is `oid` (Object Introspection Debugger).

## Build Commands

Requires a Nix environment for dependencies:

```bash
nix develop                                          # Enter dev shell
cmake -B build -G Ninja -DFORCE_BOOST_STATIC=Off    # Configure
ninja -C build                                       # Build
build/oid --help                                     # Verify build
```

For a pure environment (uncontaminated by host system):
```bash
nix develop -i
```

Build with Nix directly (does not pick up `extern/drgn` changes):
```bash
nix build
./result/bin/oid --help
```

## Testing

```bash
# Generate test config (required before first run)
./tools/config_gen.py -c clang++ build/testing.oid.toml

# Run all tests
ctest -j --test-dir build/test

# Run integration tests only
ctest --test-dir build/test/integration -j$(nproc)

# Run a single integration test with verbose output
build/test/integration/integration_test_runner --gtest_filter=OidIntegration.primitives_int --verbose

# Pass extra args to oid during integration tests
OID_TEST_ARGS="-fmy-new-feature" build/test/integration/integration_test_runner
```

## Formatting

```bash
nix fmt    # Formats Nix, C++, and Python code
```

LLVM/clang-format style: two-space indentation, brace-on-same-line.

## Architecture

### Core Data Flow

1. **Input**: `oid` attaches to a running process via ptrace
2. **Symbol resolution**: `SymbolService` + `drgn` (DWARF/debug info) extracts type information
3. **Type graph construction**: `DrgnParser` walks DWARF data and builds a `TypeGraph` — a DAG of typed nodes representing the object hierarchy
4. **Type graph passes**: A series of compiler-like passes transforms the graph:
   - `IdentifyContainers`, `AddChildren`, `AddPadding`, `AlignmentCalc`, `Flattener`, `TopoSorter`, etc.
5. **Code generation**: `CodeGen` / `OICodeGen` emits C++ introspection code using container-specific templates from `types/`
6. **JIT compilation**: `OICompiler` (wraps LLVM/Clang) compiles the generated code in-memory
7. **Execution**: The compiled code is injected into the target process and executed
8. **Result parsing**: `TreeBuilder` decodes the output data; `IntrospectionResult` / exporters present it

### Key Components

| Component | Files | Purpose |
|---|---|---|
| OID entry point | `oi/OID.cpp`, `oi/OIDebugger.cpp` | CLI, ptrace attach/detach, orchestration |
| Type graph | `oi/type_graph/` | IR representing C++ types; passes transform it |
| Code generation | `oi/CodeGen.cpp`, `oi/OICodeGen.cpp`, `oi/FuncGen.cpp` | Emit introspection C++ from type graph |
| Symbol service | `oi/SymbolService.cpp`, `oi/DrgnUtils.cpp` | DWARF/debug info lookup via drgn |
| JIT compiler | `oi/OICompiler.cpp` | LLVM/Clang in-process compilation |
| Tree builder | `oi/TreeBuilder.cpp` | Decode raw output; uses RocksDB for storage |
| OIL (library mode) | `oi/OILibrary*.cpp`, `include/oi/` | OI as a linkable library for static use |
| Container types | `types/` | Per-container introspection templates |
| Resources | `resources/` | Embedded header files for JIT-compiled code |

### Output Targets

- `oid` — debugger (attaches to running process)
- `oilgen` — OIL generator (static analysis, produces introspection libraries)
- `oip` — cache printer
- `oirp` — RocksDB output printer
- `oitb` — tree builder (post-processing)
- `liboil` / `liboil_jit` — OI as a library

### Features System

Features are toggled via `-f<feature>` / `-F<feature>` flags on `oid`. All features are listed in `oi/Features.h` using the `OI_FEATURE_LIST` X-macro.

### Integration Test Format

Tests live in `test/integration/` as TOML files. Each file defines shared type definitions and individual `[cases]` with:
- `param_types` — types to probe
- `setup` — C++ snippet returning test values
- `expect_json` — partial JSON to match against results

New `.toml` files in `test/integration/` are automatically discovered by CMake.

## Coding Style

- `CamelCase` for types, `lowerCamelCase` for functions/methods, `snake_case` for locals
- Include public headers from `include/oi/` before local headers
- Standard → third-party → project header ordering
- Do not edit files in `build/` or `resources/` (generated)
- `extern/` is read-only (third-party; drgn, elfutils)

## Commit Style

Conventional Commits: `type(scope): short description`

Examples: `fix(debugger): handle pidfd errors`, `feat(type-graph): add polymorphic inheritance pass`
