#!/usr/bin/env python3
#
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import getpass
import os
import pathlib
import re
import shutil
import subprocess
import typing

import toml

STUB_CODE = b"""
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <sys/ioctl.h>
#include <cassert>
#include <fstream>
#include <string>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <stddef.h>
#include <pthread.h>
#include <linux/sched.h>
"""

# Parts of the end of include paths to strip (eg: #include sys/param.h)
END_PATH_EXCLUDE = frozenset(("bits", "backward", "debug", "types", "sys", "gnu"))


def generate_compiler_preprocessed(compiler: str) -> str:
    """Execute compiler to grab preprocessor output"""
    out = subprocess.check_output([compiler, "-E", "-x", "c++", "-"], input=STUB_CODE)
    return out.decode()


def generate_compiler_commands(compiler: str) -> str:
    """Execute compiler to grab preprocessor output"""
    out = subprocess.check_output(
        [compiler, "-E", "-x", "c++", "-v", "-", "-o", "/dev/null"],
        stderr=subprocess.STDOUT,
        input=STUB_CODE,
    )
    return out.decode()


def filter_includes_preprocessor(
    preprocessor_output: str,
) -> typing.Tuple[typing.Iterable[str], typing.Iterable[str]]:
    """Filter preprocessor output and extract include paths from it

    We are expecting lines like this, for user:
     # 1 "/usr/include/c++/8/iostream" 1 3

     where,
     lineComps[0] - '#'
     lineComps[1] - 'line number
     lineComps[2] - 'file path
     lineComps[3] - '1' (Entering new header file)
     lineComps[4] - '3' (SrcMgr::C_System)

     see lib/Frontend/PrintPreprocessedOutput.cpp in clang source.

    Or like this, for system:
     # 1 "/usr/include/features.h" 1 3 4

     This time the consecutive [3 4] pattern indicate a
     SrcMgr::C_ExternCSystem file.
    """
    is_system, is_user = False, False
    system_paths: typing.List[pathlib.Path] = []
    user_paths: typing.List[pathlib.Path] = []
    for line in preprocessor_output.splitlines():
        s = line.split()
        if len(s) == 5 and s[3] == "1":
            is_user = True
        elif len(s) == 6 and s[3] == "1" and s[4] == "3" and s[5] == "4":
            is_system = True
        else:
            continue
        path = pathlib.Path(s[2].strip('"'))

        if not path.exists():
            continue
        path = path.resolve().parent
        while path.name in END_PATH_EXCLUDE:
            path = path.parent

        if is_system and path not in system_paths:
            system_paths.append(path)
        elif is_user and path not in user_paths:
            user_paths.append(path)

    # TODO the order of paths matter here - what's the right way to sort these lists?
    # My best guess is that we need most specific to least specific, so let's just try the depth of the paths
    system_paths.sort(key=lambda k: len(k.parents), reverse=True)
    user_paths.sort(key=lambda k: len(k.parents), reverse=True)

    return (map(str, system_paths), map(str, user_paths))


def filter_includes_commands(output: str) -> typing.Iterable[str]:
    """Filter -v compiler output and retrieve include paths if possible.

    Unfortunately relies on non-standardized behavior...

    Example output we are parsing, to obtain the directories in order
        ignoring nonexistent directory "/include"
        #include "..." search starts here:
        #include <...> search starts here:
        /usr/local/include
        /usr/lib64/clang/12.0.1/include
        /usr/include
        End of search list.
        # 1 "<stdin>"
        # 1 "<built-in>" 1
        # 1 "<built-in>" 3
        # 341 "<built-in>" 3
        # 1 "<command line>" 1
        # 1 "<built-in>" 2
        # 1 "<stdin>" 2

        End of search list
    """
    collecting = False
    paths: typing.List[pathlib.Path] = []
    for line in output.splitlines():
        if collecting:
            if line == "End of search list.":
                break
            # Just a backup - ideally this should never trigger
            if line.startswith("#"):
                continue
            path = pathlib.Path(line.strip())
            path = path.resolve()
            if path not in paths and path.exists():
                paths.append(path)

        if line.startswith("#include <...> search starts here:"):
            collecting = True
            continue
    return map(str, paths)


def pull_base_toml() -> typing.Dict:
    script = pathlib.Path(__file__)
    repo_path = script.parent.parent
    script = repo_path / "dev.oid.toml"
    if not script.exists():
        raise RuntimeError(
            "Base file dev.oid.toml not found, either replace it, or skip types."
        )
    with open(script, "r") as f:
        base = toml.load(f)

    # Now, we need to replace any placeholders that might be present in the base toml file with the real verisons.
    user = getpass.getuser()
    if "IN_NIX_SHELL" in os.environ and "src" in os.environ:
        pwd = os.environ['src']
    else:
        pwd = str(repo_path.resolve())

    container_list = base.get("types", {}).get("containers")
    if container_list:
        for idx, c in enumerate(container_list):
            container_list[idx] = c.replace("PWD", pwd).replace("USER", user)

    return base


def generate_toml(
    system_paths: typing.Iterable[str],
    user_paths: typing.Iterable[str],
    base_object: typing.Dict,
    output_file: str,
):
    base_object.update(
        {
            "headers": {
                "system_paths": list(system_paths),
                "user_paths": list(user_paths),
            }
        }
    )

    output_path = pathlib.Path(output_file)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        toml.dump(base_object, f)


def safe_component(value: pathlib.Path) -> str:
    name = value.name or "root"
    return re.sub(r"[^A-Za-z0-9._+-]+", "_", name)


def replace_directory(path: pathlib.Path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def copy_directory_contents(source: pathlib.Path, destination: pathlib.Path):
    replace_directory(destination)
    # Preserve symlinks in compiler include roots. Some system include trees
    # contain directory symlink cycles, such as ncurses -> ., which recurse
    # indefinitely if copytree follows links.
    shutil.copytree(source, destination, symlinks=True, dirs_exist_ok=True)


def relative_to_config(path: pathlib.Path, config_directory: pathlib.Path) -> str:
    return pathlib.Path(os.path.relpath(path, config_directory)).as_posix()


def copy_container_configs(
    base_object: typing.Dict,
    types_directory: pathlib.Path,
    config_directory: pathlib.Path,
):
    container_list = base_object.get("types", {}).get("containers")
    if not container_list:
        return

    replace_directory(types_directory)
    copied_containers = []
    for idx, container in enumerate(container_list):
        source = pathlib.Path(container)
        if not source.exists():
            raise RuntimeError(f"Container config not found: {source}")

        destination = types_directory / f"{idx:02d}-{source.name}"
        shutil.copy2(source, destination)
        copied_containers.append(relative_to_config(destination, config_directory))

    base_object["types"]["containers"] = copied_containers


def copy_header_paths(
    paths: typing.Iterable[str],
    headers_directory: pathlib.Path,
    config_directory: pathlib.Path,
) -> typing.List[str]:
    replace_directory(headers_directory)

    copied_paths = []
    for idx, path in enumerate(paths):
        source = pathlib.Path(path).resolve()
        if not source.exists():
            continue
        if not source.is_dir():
            raise RuntimeError(f"Header path is not a directory: {source}")

        destination = headers_directory / f"{idx:02d}-{safe_component(source)}"
        copy_directory_contents(source, destination)
        copied_paths.append(relative_to_config(destination, config_directory))

    return copied_paths


def generate_sdk_toml(
    system_paths: typing.Iterable[str],
    user_paths: typing.Iterable[str],
    base_object: typing.Dict,
    output_file: str,
    sdk_root: str,
    sdk_share_dir: str,
    sdk_types_dir: str,
    sdk_headers_dir: str,
):
    sdk_root_path = pathlib.Path(sdk_root).resolve()
    share_directory = sdk_root_path / sdk_share_dir
    output_path = pathlib.Path(output_file)
    if not output_path.is_absolute():
        output_path = share_directory / output_path
    output_path.parent.mkdir(parents=True, exist_ok=True)

    config_directory = output_path.parent.resolve()
    types_directory = share_directory / sdk_types_dir
    headers_directory = share_directory / sdk_headers_dir

    copy_container_configs(base_object, types_directory, config_directory)
    copied_system_paths = copy_header_paths(
        system_paths, headers_directory / "system", config_directory
    )
    copied_user_paths = copy_header_paths(
        user_paths, headers_directory / "user", config_directory
    )

    generate_toml(
        copied_system_paths,
        copied_user_paths,
        base_object,
        str(output_path),
    )


def main():
    parser = argparse.ArgumentParser(
        description="Run a c/c++ compiler and attempt to generate an oi config file from the results"
    )
    parser.add_argument(
        "-c",
        "--compiler",
        default="clang++",
        help="The compiler binary used to generate headers from.",
    )
    parser.add_argument(
        "--skip-types",
        action="store_true",
        help="Whether to skip pulling types from dev.oid.toml in addition to generating include headers.",
    )
    parser.add_argument(
        "--include-mode",
        choices=("preprocessor", "commands"),
        default="commands",
        help="Which strategy to use for generating includes. Right now choose between using -E (preprocessor) or -v (verbose commands)",
    )
    parser.add_argument(
        "--sdk-root",
        help="Copy container configs and discovered include roots under this SDK prefix and emit relocatable paths.",
    )
    parser.add_argument(
        "--sdk-share-dir",
        default="share/oil",
        help="Directory under --sdk-root for SDK config assets.",
    )
    parser.add_argument(
        "--sdk-types-dir",
        default="types",
        help="Directory under --sdk-share-dir for copied container TOML files.",
    )
    parser.add_argument(
        "--sdk-headers-dir",
        default="headers",
        help="Directory under --sdk-share-dir for copied compiler include roots.",
    )
    parser.add_argument(
        "output_file", help="Toml file to output finished config file to."
    )
    args = parser.parse_args()

    if args.include_mode == "preprocessor":
        preprocessed = generate_compiler_preprocessed(args.compiler)
        system_includes, user_includes = filter_includes_preprocessor(preprocessed)
    elif args.include_mode == "commands":
        output = generate_compiler_commands(args.compiler)
        system_includes = filter_includes_commands(output)
        user_includes = []
    else:
        raise ValueError("Invalid include mode provided!")

    system_includes = list(system_includes)
    user_includes = list(user_includes)

    if args.skip_types:
        base = {}
    else:
        base = pull_base_toml()

    if args.sdk_root:
        generate_sdk_toml(
            system_includes,
            user_includes,
            base,
            args.output_file,
            args.sdk_root,
            args.sdk_share_dir,
            args.sdk_types_dir,
            args.sdk_headers_dir,
        )
    else:
        generate_toml(system_includes, user_includes, base, args.output_file)


if __name__ == "__main__":
    main()
