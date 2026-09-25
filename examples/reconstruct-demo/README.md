# reconstruct_demo

This directory holds three demos, in the order they were built:

- **`reconstruct_demo`** (below): a two-process demo proving byte-accurate
  capture and reconstruction of a non-cyclic object with a broad mix of
  member kinds, including real `std::shared_ptr` aliasing.
- **`cycle_breaking_demo`**: proves BreakCycles lets a genuinely
  self-referential, cyclic raw-pointer structure be *introspected* at all
  (`facebookexperimental/object-introspection#293` stage 3) - introspect-only,
  see its own header comment for why.
- **`cyclic_reconstruct_demo`**: the follow-up - proves such a structure can
  also be *reconstructed*, both when the cycle closes onto a non-root
  ancestor (succeeds, real values) and when it closes onto the
  reconstruction root itself (cleanly rejected) - see its own header comment.

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
member, a scalar, a `std::string`, a `std::vector<int32_t>`, a
`std::map<std::string, int32_t>`, a trivially-copyable C-style union
(`IPv4Address` - the classic "same 4 bytes as either one `uint32_t` or 4
octets" trick, populated with `8.8.8.8` so the reconstructed value is
immediately recognizable), a nested (non-union) struct (`Version{major,
minor, patch}`, populated with `2.1.0`) - reconstructed recursively,
member-by-member, the same way `DemoObject` itself is - a
`std::unique_ptr<ContactInfo>` (a non-null owned nested struct, populated
with a made-up support email/extension), reconstructed via the same
present/absent decoding used for any `std::unique_ptr`, recursing into
`ContactInfo`'s own members exactly like the plain nested struct case -
and two `std::shared_ptr<int32_t>` members, `priority` and
`priorityAlias`, that deliberately **alias each other** (point at the
exact same underlying `int`, not just equal values) - proving OI's
address→object registry resolves a repeated captured pointer address to
one shared instance rather than reconstructing a second, independent
copy. This only works because the pointee (a plain `int32_t`) isn't
self-referential - a genuine *cycle* still isn't supported (see "Known
limitations" below) - and a non-null raw pointer to a nested struct
(`Location* location`, populated with the Statue of Liberty's
coordinates), heap-allocated fresh on the reconstructing side and
**deliberately never freed** (a raw pointer carries no destruction
machinery to hook into, unlike `std::unique_ptr`/`std::shared_ptr` -
see "Known limitations").

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
   `tools/config_gen.py`) with the `capture-bytes` and `chase-raw-pointers`
   features enabled - the two things this target's config needs that the
   project's general test config doesn't turn on by default.
   `chase-raw-pointers` is only safe to enable unconditionally here because
   `DemoObject` has no self-referential types - see
   `PrependCaptureBytesFeature.cmake` for why this isn't a project-wide
   default.

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
  address = 8.8.8.8 (same bytes as raw=0x8080808)
  version = 2.1.0
  contact = support@example.com x4242
  priority = 5
  priorityAlias = 5 (same object as priority: true, use_count=2)
  location = 40.6892, -74.0445
client: captured 284 bytes, sending via transport=local
client: done
```

Within 5 seconds, the server's next poll picks up the file and finishes:

```
server: received 284 bytes, reconstructing...
server: reconstructed object:
  status = ACTIVE
  id = 42
  label = "Hello from the OI byte-accurate reconstruction demo!"
  scores = [10, 20, 30, 40, 50]
  attributes = {alpha: 1, beta: 2, gamma: 3}
  address = 8.8.8.8 (same bytes as raw=0x8080808)
  version = 2.1.0
  contact = support@example.com x4242
  priority = 5
  priorityAlias = 5 (same object as priority: true, use_count=2)
  location = 40.6892, -74.0445
```

The `priorityAlias` line matching `true`/`use_count=2` on the
reconstructed side is the interesting part: the server process never
shared memory with the client, yet it correctly rebuilt `priority` and
`priorityAlias` as two owners of one shared object, purely from the
address information in the byte stream - not two independent copies
that merely happen to hold equal values. The `location` line matching is
the raw-pointer equivalent proof point: the server allocates a brand-new
`Location` on its own heap and fills it purely from captured bytes, with
no memory shared with the client at all.

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
  sticks to what's supported today (enum, scalar, string, vector, map,
  trivially-copyable union, nested non-union struct, `std::unique_ptr`,
  aliased `std::shared_ptr`, non-aliased raw pointer). `std::weak_ptr`
  isn't yet, and neither is an *aliased* raw pointer (two raw pointers to
  the same object) - that throws a clear error rather than reconstructing
  wrong data, the same as an aliased `std::shared_ptr` would before its
  own registry support landed.
- **`location` is heap-allocated and deliberately never freed.** Unlike
  `std::unique_ptr`/`std::shared_ptr` (which have no leak problem at all -
  the reconstructed smart pointer's own destructor handles cleanup
  normally), a raw pointer carries no destruction machinery, and
  reconstruction has no way to know whether the original program even
  considered this pointer "owning" in the first place. Heap-allocating and
  never freeing is the only defensible default given that constraint - see
  `CodeGen::emitReconstructPointerValue`'s own comment.
- **`DemoObject` itself still has no self-referential member.** Genuine
  cycles (a raw pointer, since `chase-raw-pointers` is the only edge kind
  BreakCycles currently rewrites) are now both capturable and
  reconstructable - see `cyclic_reconstruct_demo` above, and
  `docs/object-capture-initial-thoughts.md` (not part of this repo) - but
  reconstructing a cycle that closes directly onto the literal
  `oi::reconstruct<T>()` root is still deliberately rejected with a clear
  error rather than attempted: `T` is returned by value, so a self-pointer
  into that value can't be represented until a caller-owns-the-storage
  entry point (a planned `oi::reconstructInto<T>(T&, bytes)`) exists.
  `priority`/`priorityAlias` and `location` are safe here specifically
  because their pointee types aren't self-referential - that was a
  simplification of this particular demo, not a limitation of aliasing
  itself.
