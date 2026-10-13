#!/usr/bin/env python3
"""Check a distributable app's architecture, deployment targets and dependencies.

Run on macOS after packaging, before sending the app to another computer.
This is read-only and needs only Python 3 and Apple's command-line tools.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re
import subprocess


MACH_MAGIC = {
    b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe',
    b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe',
    b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca',
    b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca',
}


def version(value):
    parts = tuple(int(p) for p in value.split('.'))
    return (parts + (0, 0, 0))[:3]


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def load_commands(text):
    return re.split(r'\nLoad command \d+\n', '\n' + text)[1:]


def inside(path, root):
    try:
        path.resolve().relative_to(root)
        return True
    except ValueError:
        return False


def audit(app, architecture, maximum_os):
    app = app.resolve()
    executable_dir = app / 'Contents/MacOS'
    errors, binaries = [], []
    with (app / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    minimum = info.get('LSMinimumSystemVersion')
    if not minimum or version(minimum) > version(maximum_os):
        errors.append('Info.plist must declare a minimum macOS no newer than ' + maximum_os)
    main = executable_dir / info['CFBundleExecutable']
    if not main.is_file():
        errors.append('The bundle executable is missing')

    def local_path(value, binary):
        if value.startswith('@loader_path/'):
            return binary.parent / value[len('@loader_path/'):]
        if value.startswith('@executable_path/'):
            return executable_dir / value[len('@executable_path/'):]
        return None

    for path in sorted(app.rglob('*')):
        if path.is_symlink() and not inside(path, app):
            errors.append(str(path.relative_to(app)) + ': symlink points outside the app')
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as stream:
            if stream.read(4) not in MACH_MAGIC:
                continue
        label = str(path.relative_to(app))
        architectures = command('/usr/bin/lipo', '-archs', str(path)).split()
        if architecture not in architectures:
            errors.append(label + ': missing ' + architecture)
            continue
        blocks = load_commands(command('/usr/bin/otool', '-arch', architecture, '-l', str(path)))
        targets, dependencies, rpaths = [], [], []
        for block in blocks:
            kind = re.search(r'^\s*cmd (\S+)', block, re.M)
            if not kind:
                continue
            kind = kind[1]
            if kind in ('LC_BUILD_VERSION', 'LC_VERSION_MIN_MACOSX'):
                match = re.search(r'^\s*(?:minos|version) ([\d.]+)', block, re.M)
                if match:
                    targets.append(match[1])
            elif kind == 'LC_RPATH':
                rpaths.append(re.search(r'^\s*path (.*?) \(offset', block, re.M)[1])
            elif kind in ('LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB', 'LC_LOAD_UPWARD_DYLIB'):
                dependencies.append(re.search(r'^\s*name (.*?) \(offset', block, re.M)[1])
        if not targets or any(version(t) > version(maximum_os) for t in targets):
            errors.append(label + ': incompatible deployment target ' + repr(targets))
        resolved_rpaths = []
        for value in rpaths:
            resolved = local_path(value, path)
            if resolved is None or not inside(resolved, app):
                errors.append(label + ': external runtime search path ' + value)
            else:
                resolved_rpaths.append(resolved)
        # Dylibs can inherit the executable's Frameworks search path.
        resolved_rpaths.append(app / 'Contents/Frameworks')
        for dependency in dependencies:
            if dependency.startswith(('/System/Library/', '/usr/lib/')):
                continue
            resolved = local_path(dependency, path)
            candidates = [resolved] if resolved else []
            if dependency.startswith('@rpath/'):
                candidates += [p / dependency[len('@rpath/'):] for p in resolved_rpaths]
            if not any(p.is_file() and inside(p, app) for p in candidates):
                errors.append(label + ': dependency is not bundled: ' + dependency)
        binaries.append({'path': label, 'architectures': architectures, 'minimum_macos': targets})
    if not binaries:
        errors.append('No Mach-O binaries found')
    return {'app': str(app), 'architecture': architecture, 'maximum_macos': maximum_os,
            'binaries': binaries, 'errors': errors, 'passed': not errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--arch', required=True, choices=('x86_64', 'arm64'))
    parser.add_argument('--macos', required=True, help='Oldest supported machine, e.g. 12.6')
    args = parser.parse_args()
    report = audit(args.app, args.arch, args.macos)
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
