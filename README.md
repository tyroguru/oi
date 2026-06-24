# object-introspection

[![Matrix Chat](https://img.shields.io/matrix/object-introspection:matrix.org.svg)](https://matrix.to/#/#object-introspection:matrix.org)
[![CppCon 2023 Presentation](https://img.shields.io/youtube/views/6IlTs8YRne0?label=CppCon%202023)](https://youtu.be/6IlTs8YRne0)

![OI Logo](/website/static/img/OIBrandmark.svg)

Object Introspection is a memory profiling technology for C++ objects. It provides the ability to dynamically instrument applications to capture the precise memory occupancy of entire object hierarchies including all containers and dynamic allocations. All this with no code modification or recompilation!

For more information on the technology and how to get started applying it  to your applications please check out the [Object Introspection](https://objectintrospection.org/) website.

## Join the Object Introspection community
See the [CONTRIBUTING](CONTRIBUTING.md) file for how to help out.

## License
Object Introspection is licensed under the [Apache 2.0 License](LICENSE).

## Getting started with Nix

Nix is the easiest way to get started with `oid` as it is non-trivial to build otherwise. Explicit Nix support for Object Introspection as a Library will come down the line, but Nix can currently provide you a reproducible development environment in which to build it.

These examples expect you to have `nix` installed and available with no other dependencies required. Find the installation guide at https://nixos.org/download.html.

We also required flake support. To enable flakes globally run:

    $ mkdir -p ~/.config/nix
    $ echo "experimental-features = nix-command flakes" >> ~/.config/nix/nix.conf

Or suffix every `nix` command with `nix --extra-experimental-features 'nix-command flakes'`.

### Run upstream OID without modifying the source

    $ nix run github:facebookexperimental/object-introspection -- --help

This will download the latest source into your Nix store along with all of its dependencies, running help afterwards.

### Build OID locally

    $ git clone https://github.com/facebookexperimental/object-introspection
    $ nix build
    $ ./result/bin/oid --help

This will build OID from your local sources. Please note that this will NOT pick up changes to `extern/drgn` or `extern/drgn/libdrgn/velfutils`.

### Get a development environment

    $ nix develop
    $ cmake -B build -G Ninja -DFORCE_BOOST_STATIC=Off
    $ ninja -C build
    $ build/oid --help

This command provides a development shell with all the required dependencies. This is the most flexible option and will pick up source changes as CMake normally would.

Sometimes this developer environment can be polluted by things installed on your normal system. If this is an issue, use:

    $ nix develop -i

This removes the environment from your host system and makes the build pure.

### Run the tests

    $ nix develop
    $ cmake -B build -G Ninja -DFORCE_BOOST_STATIC=Off
    $ ninja -C build
    $ ./tools/config_gen.py -c clang++ build/testing.oid.toml
    $ ctest -j --test-dir build/test

Running tests under `nix` is new to the project and may take some time to mature. The CI is the source of truth for now.

### Install an OIL runtime bundle

Applications that link against `liboil.so` or `liboil_jit.so` should not need the OI development tree or development shell at runtime. Enable `OIL_INSTALL_RUNTIME_BUNDLE` to install OIL's non-system shared library dependencies beside the OIL libraries and rewrite bundled ELF rpaths to look in that same directory.

The bundle step currently targets Linux ELF installs and requires `patchelf`:

    $ nix develop
    $ cmake -B build -G Ninja \
        -DCMAKE_INSTALL_PREFIX=/tmp/oil-sdk \
        -DFORCE_BOOST_STATIC=Off \
        -DOIL_INSTALL_RUNTIME_BUNDLE=ON
    $ ninja -C build install

The installed prefix contains the exported CMake package, OIL headers, `liboil.so`, `liboil_jit.so`, and copied runtime dependencies under `lib/oil-runtime`. The installed OIL shared libraries use `$ORIGIN:$ORIGIN/oil-runtime` as their runtime search path, so the bundle can be relocated as a unit without putting bundled third-party libraries in the consumer application rpath.

The bundle excludes the dynamic loader and core libc libraries by default. Adjust `OIL_RUNTIME_BUNDLE_PRE_EXCLUDE_REGEXES` if a packaging target needs a different policy. If reinstalling over an older flat bundle, use a clean install prefix or remove previously copied third-party `.so` files from `${prefix}/lib` so they do not appear in consumer application rpaths.

### Format source

    $ nix fmt

This formats the Nix, C++, and Python code in the repository.
