#!/usr/bin/env python3
"""Exercise native model imports/remapping using an already-built macOS Studio.

Uses the build's exact compiler and linker flags. Creates only ignored build
artifacts and temporary backup data; fixture projects are read, never modified.
"""
import argparse
import pathlib
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build/arm64"))
    parser.add_argument("--test-source",type=pathlib.Path,default=pathlib.Path("tests/studio_assistant_model.test.cpp"))
    parser.add_argument("--check-name",default="assistant-native-checks")
    parser.add_argument("--build-only",action="store_true",help="Build a native fixture for execution on another Mac of the target architecture")
    parser.add_argument("fixtures", type=pathlib.Path, nargs="*")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    output = build / args.check_name
    output.mkdir(exist_ok=True)

    def commands(target):
        return subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True).splitlines()

    compile_flags = shlex.split(commands("src/libslic3r/CMakeFiles/libslic3r.dir/Model.cpp.o")[-1])
    filtered = []
    index = 0
    while index < len(compile_flags):
        token = compile_flags[index]
        if token == "-Xclang" and compile_flags[index + 1] in ("-include-pch", "-include"):
            index += 4
            continue
        if token in ("-o", "-MF", "-MT"):
            index += 2
            continue
        if token in ("-MD", "-c") or token.endswith("/libslic3r/Model.cpp"):
            index += 1
            continue
        filtered.append(token)
        index += 1

    def macro(value):
        return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'

    obj = output / "model-checks.o"
    binary = output / "model-checks"
    filtered += ["-UNDEBUG", "-DPROJECT_RESOURCES=" + macro(root / "resources"),
                 "-DPROJECT_TEST_TMP=" + macro(output / "tmp"), "-c",
                 str(args.test_source.resolve()), "-o", str(obj)]
    with (output / "compile.log").open("w") as log:
        subprocess.run(filtered, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)

    target = "src/BambuStudio.app/Contents/MacOS/BambuStudio"
    command = next(line for line in reversed(commands(target)) if "-o " + target in line)
    link = shlex.split(command)
    start = next(i for i, value in enumerate(link) if pathlib.Path(value).name in ("c++", "clang++", "g++"))
    end = link.index("&&", start) if "&&" in link[start:] else len(link)
    link = link[start:end]
    replaced = False
    for index, value in enumerate(link):
        if value.endswith("/BambuStudio.cpp.o"):
            link[index] = str(obj)
            replaced = True
        if value == "-o":
            link[index + 1] = str(binary)
    if not replaced:
        raise RuntimeError("Could not identify Studio's entry-point object in the generated link command")
    with (output / "link.log").open("w") as log:
        subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    if args.build_only:
        print(f"Built native checks: {binary}")
        return
    with (output / "results.log").open("w") as log:
        subprocess.run([str(binary), *(str(p.resolve()) for p in args.fixtures)], cwd=root,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    for line in (output / "results.log").read_text().splitlines():
        if "passed." in line:
            print(line)
    print(f"Full log: {output / 'results.log'}")


if __name__ == "__main__":
    main()
