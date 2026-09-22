# reconstruct_demo

A two-process demo proving byte-accurate object capture on one process and
reconstruction on another: one executable, two modes.

- **client** builds a fixed, human-recognizable `DemoObject`, captures it via
  `oi::introspect()`, and writes the raw captured byte stream to a known path
  under `/tmp`.
- **server** polls that path every 5 seconds until it sees data, then
  reconstructs a `DemoObject` via `oi::reconstruct<DemoObject>()` from those
  bytes alone and prints it.

The server process never shares memory with the client - it only ever sees a
flat byte stream on disk - so a matching printout on both sides demonstrates
the round trip actually works. `DemoObject` (defined in `ReconstructDemo.cpp`)
covers every member kind reconstruction currently supports: a nested-enum
member, a scalar, a `std::string`, a `std::vector<int32_t>`, and a
`std::map<std::string, int32_t>`.

Only `--transport local` is implemented today. `--transport remote` is
accepted on the command line but rejected at runtime - it's a placeholder for
a real network transport later.

## Prerequisites

This project builds inside its Nix dev shell, which provides every
dependency (Boost, Clang/LLVM, RocksDB, folly, etc.) - there is nothing extra
to install for this demo specifically. From the repository root:

```bash
nix develop
```

(Use `nix develop -i` instead for a pure environment, uncontaminated by the
host system - see the top-level project instructions.)

## Build

Configure and build the whole project once, the normal way:

```bash
cmake -B build -G Ninja -DFORCE_BOOST_STATIC=Off
ninja -C build reconstruct_demo
```

`reconstruct_demo` is built by default whenever `WITH_TESTS` is on (the
project default) - `ninja -C build` with no target also builds it, along with
everything else. Building just the named target is faster if you only care
about this demo.

The build has two steps under the hood, wired up in this directory's
`CMakeLists.txt`:

1. `ReconstructDemo.cpp` is compiled normally (`-DOIL_AOT_COMPILATION=1`).
2. The in-tree `oilgen` tool parses that same source file and generates the
   missing `introspectImpl<DemoObject>`/`reconstructImpl<DemoObject>`
   definitions, using a config generated specifically for this target (via
   `tools/config_gen.py`) with the `capture-bytes` feature enabled - this is
   the one thing this target's config needs that the project's general test
   config doesn't turn on by default.

Both are then linked together against `liboil.so` into one executable at
`build/examples/reconstruct-demo/bin/reconstruct_demo`.

### Runtime dependencies

Nothing extra to install or configure to *run* the binary from the build
tree: it's linked with an `RPATH` pointing directly at `build/`, so
`liboil.so` resolves automatically wherever you run it from. You do not need
`OIL_INSTALL_RUNTIME_BUNDLE` or any other install step for this - that CMake
option only matters if you want to copy the binary out of the build tree
(e.g. to package or ship it elsewhere) and need its shared-library
dependencies bundled alongside it. Running it in place, straight out of
`build/`, just works.

## Run

Open two terminals (both need the same Nix dev shell active, or at least the
same environment the build used).

**Terminal 1 - start the server first** (it will wait):

```bash
build/examples/reconstruct-demo/bin/reconstruct_demo --mode server
```

```
server: waiting for /tmp/oi_reconstruct_demo.bytes ...
```

**Terminal 2 - run the client:**

```bash
build/examples/reconstruct-demo/bin/reconstruct_demo --mode client
```

```
client: original object:
  status = ACTIVE
  id = 42
  label = "Hello from the OI byte-accurate reconstruction demo!"
  scores = [10, 20, 30, 40, 50]
  attributes = {alpha: 1, beta: 2, gamma: 3}
client: captured 189 bytes, sending via transport=local
client: done
```

Within 5 seconds, the server's next poll picks up the file and finishes:

```
server: received 189 bytes, reconstructing...
server: reconstructed object:
  status = ACTIVE
  id = 42
  label = "Hello from the OI byte-accurate reconstruction demo!"
  scores = [10, 20, 30, 40, 50]
  attributes = {alpha: 1, beta: 2, gamma: 3}
```

The two printouts matching, despite the server process never having touched
the client's memory, is the whole demo.

You can also run the client first, then the server - the server checks the
path immediately on its first poll, so if the bytes are already there it
reconstructs right away instead of waiting.

### Resetting between runs

Each run's captured bytes are written to `/tmp/oi_reconstruct_demo.bytes`
(via a temp file plus `rename()`, so the server never reads a half-written
file mid-poll). Nothing deletes this file automatically, so a server started
after a previous run will immediately reconstruct the *old* object instead of
waiting for a new one. Remove it first if you want a clean run:

```bash
rm -f /tmp/oi_reconstruct_demo.bytes
```

## Known limitations

This is a research prototype for proving a specific mechanism, not a general
transport:

- **Local filesystem transport only.** `--transport remote` is a recognized
  but unimplemented placeholder.
- **The server's poll loop is deliberately simple and racy** - "wake up every
  5 seconds, check whether the file is non-empty" - by design, to keep the
  demo minimal. It reconstructs once and exits; it does not loop to handle
  multiple captures.
- **Only same-machine, same-binary, same-architecture transport is proven.**
  Nothing here validates cross-architecture or cross-endianness
  reconstruction.
- **Not every member type is reconstructable yet.** `DemoObject` deliberately
  sticks to what's supported today (enum, scalar, string, vector, map).
  Nested class/struct members and pointer members aren't yet.
