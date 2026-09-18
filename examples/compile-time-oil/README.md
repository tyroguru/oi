# compile-time-oil

A minimal, runnable example of OIL's **Ahead-of-Time (AOT)** introspection
path, as opposed to the JIT path shown in
[`examples/jit-oil/trivial_jit_shared_example`](../jit-oil/trivial_jit_shared_example).

## JIT vs. AOT, in short

Both paths let you write the same code:

```cpp
#include <oi/oi.h>

struct Foo { std::vector<std::string> strings; };

Foo foo = ...;
auto result = oi::introspect(foo);  // or oi::setupAndIntrospect(foo, opts) for JIT
```

- **JIT** (`oi::setupAndIntrospect`) generates and compiles the introspection
  code for `Foo` the first time your process actually calls it, using drgn to
  read `Foo`'s layout back out of your binary's own debug info. Nothing extra
  needs to happen at build time, but the first call pays a real compile-time
  cost, and your binary needs to carry `-g` debug info and an OIL config
  pointing at your toolchain's headers/containers at runtime.

- **AOT** (`oi::introspect`, this example) generates and compiles the
  introspection code for `Foo` once, at *build* time, via the `oilgen` tool.
  `oilgen` never attaches to a process or reads DWARF - it parses your
  source file's Clang AST directly (the same way clangd or clang-tidy would)
  to see exactly which types you called `oi::introspect<T>()` for, and emits
  an object file defining them. There's no first-call cost, no drgn/DWARF
  dependency at runtime, and no config needed once your binary is built - but
  it does mean an extra build step, and the type you introspect must have
  external linkage (see the comment in `OilVectorOfStrings.cpp`).

## How the pieces fit together

`include/oi/oi.h` declares (but does not define) a *weak* function template
for AOT mode:

```cpp
template <class T, Feature... Fs>
IntrospectionResult __attribute__((weak)) introspectImpl(const T& objectAddr);
```

`oi::introspect<T>()` checks that this weak symbol actually resolved to
something before calling it, and throws if not. So building this example is
a two-pass process over the *same* source file:

1. Compile `OilVectorOfStrings.cpp` normally, with `-DOIL_AOT_COMPILATION=1`.
   This produces an object file where `introspectImpl<Foo>` is referenced
   but not defined (a weak/null symbol).
2. Run `oilgen` over that same source file. It finds the `oi::introspect<Foo>()`
   call, builds a type graph for `Foo` from the Clang AST (via the same
   type-graph/codegen pipeline the rest of OI uses), and compiles a *second*
   object file that defines the real `introspectImpl<Foo>`.

Link both object files together (plus `liboil`, for the
`IntrospectionResult`/exporters runtime support code that the generated code
calls into) and the weak symbol resolves to the real implementation.

This is also exactly what
[`test/oilgen/CMakeLists.txt`](../../test/oilgen/CMakeLists.txt) automates as
a CI-checked regression test - if you're integrating `oilgen` into your own
build system (Bazel, Buck, a different flavour of CMake, ...) rather than
following the Makefile here, that's the other reference to look at.

## Usage

`oilgen` isn't currently part of OIL's installable CMake package (`find_package(oil)`
only exports `liboil`/`liboil_jit`), so the only way to get it today is to
build this monorepo:

```sh
# From the repo root, inside `nix develop`:
cmake -B build -G Ninja -DFORCE_BOOST_STATIC=Off
ninja -C build oid oilgen

# Then, from this directory:
make run
```

`make` will generate `build/testing.oid.toml` for you if it doesn't already
exist (see `tools/config_gen.py` - it only needs to know your compiler, to
find its header search paths and OIL's container definitions).

Expected output:

```
a0 static=24 exclusive=0
strings static=24 exclusive=56
[] static=32 exclusive=49
[] static=32 exclusive=32
[] static=32 exclusive=60
elements=5
```

(exact `static`/`exclusive` byte counts may vary by platform/ABI.)
