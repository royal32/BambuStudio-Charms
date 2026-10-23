#!/usr/bin/env python3
"""Stable signing for private family builds; never fall back to ad-hoc signing.

The signing key is non-exportable in a dedicated Keychain. Its unlock password
is stored in the login Keychain for Apple's security tool. No signing secrets,
OAuth credentials, or private keys are stored in the repository or app bundle.
This does not install a trusted root, enable Gatekeeper exceptions, or notarize.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import shlex
import subprocess
import tempfile

CONFIG = Path.home() / 'Library/Application Support/CharmsStudioSigning/identity.json'
KEYCHAIN = Path.home() / 'Library/Keychains/CharmsStudioBuildSigning.keychain-db'
SERVICE = 'Charms Studio private build signing'
ACCOUNT = 'local-signing-keychain'


def run(args, *, data=None, private=False):
    result = subprocess.run(args, input=data, text=True, capture_output=True)
    if result.returncode:
        detail = '' if private else ': ' + result.stderr.strip()
        raise RuntimeError(Path(args[0]).name + ' failed' + detail)
    return result.stdout.strip()


def security_command(args):
    # Pass transient passwords through stdin, never in a process argument list.
    result = subprocess.run(['/usr/bin/security', '-i'], input=shlex.join(args)+'\n', text=True, capture_output=True)
    # The interactive command interpreter can exit 0 after a subcommand failed.
    errors = ('SecKeychain', 'SecItem', 'Error:', 'error:', 'failed', 'unknown command')
    if result.returncode or any(value in result.stderr for value in errors):
        raise RuntimeError('security '+args[0]+' failed; no credential values were logged.')
    return result.stdout.strip()


def login_unlocked():
    library = ctypes.CDLL('/System/Library/Frameworks/Security.framework/Security')
    core = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    library.SecKeychainCopyDefault.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    library.SecKeychainGetStatus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    core.CFRelease.argtypes = [ctypes.c_void_p]
    reference = ctypes.c_void_p()
    if library.SecKeychainCopyDefault(ctypes.byref(reference)):
        raise RuntimeError('No login Keychain is available; no password prompt was opened.')
    try:
        status = ctypes.c_uint32()
        if library.SecKeychainGetStatus(reference, ctypes.byref(status)) or not status.value & 1:
            raise RuntimeError('The login Keychain is locked; no password prompt was opened.')
    finally:
        core.CFRelease(reference)


def detach_signing_keychain():
    # create-keychain adds it to the user's search list. Signing addresses it
    # explicitly, so leave other apps searching exactly their existing Keychains.
    paths = shlex.split(run(['/usr/bin/security','list-keychains','-d','user']))
    filtered = [path for path in paths if Path(path) != KEYCHAIN]
    if len(filtered) != len(paths):
        run(['/usr/bin/security','list-keychains','-d','user','-s',*filtered])


def initialize():
    if CONFIG.exists():
        detach_signing_keychain()
        return load()
    if KEYCHAIN.exists():
        raise RuntimeError('The signing Keychain already exists without its identity record; refusing to replace it.')
    login_unlocked()
    CONFIG.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    password, transfer_password = secrets.token_hex(32), secrets.token_hex(32)
    with tempfile.TemporaryDirectory(prefix='charms-signing-') as directory:
        root = Path(directory)
        key, cert, bundle = root/'key.pem', root/'cert.pem', root/'identity.p12'
        run(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:3072', '-nodes',
             '-keyout', str(key), '-out', str(cert), '-days', '3650', '-sha256',
             '-subj', '/CN=Charms Studio Private Builds/O=Private family builds',
             '-addext', 'keyUsage=critical,digitalSignature',
             '-addext', 'extendedKeyUsage=critical,codeSigning'], private=True)
        # /usr/bin/openssl produces a PKCS#12 encoding supported by macOS 12.
        run(['/usr/bin/openssl', 'pkcs12', '-export', '-inkey', str(key), '-in', str(cert),
             '-out', str(bundle), '-passout', 'stdin'], data=transfer_password+'\n', private=True)
        fingerprint = run(['/usr/bin/openssl', 'x509', '-in', str(cert), '-noout', '-fingerprint', '-sha1']).split('=')[-1].replace(':','').strip().upper()
        if not re.fullmatch(r'[0-9A-F]{40}', fingerprint):
            raise RuntimeError('The signing certificate fingerprint could not be read.')
        security_command(['create-keychain', '-p', password, str(KEYCHAIN)])
        try:
            security_command(['import', str(bundle), '-k', str(KEYCHAIN), '-P', transfer_password,
                              '-x', '-T', '/usr/bin/codesign'])
            security_command(['set-key-partition-list', '-S', 'apple-tool:,apple:,codesign:', '-s',
                              '-k', password, str(KEYCHAIN)])
            security_command(['set-keychain-settings', '-l', '-u', '-t', '300', str(KEYCHAIN)])
            security_command(['add-generic-password', '-a', ACCOUNT, '-s', SERVICE, '-w', password,
                              '-T', '/usr/bin/security'])
            record = {'identity': fingerprint, 'keychain': str(KEYCHAIN), 'purpose': 'private family builds'}
            CONFIG.write_text(json.dumps(record, indent=2)+'\n')
            CONFIG.chmod(0o600)
        finally:
            security_command(['lock-keychain', str(KEYCHAIN)])
            detach_signing_keychain()
    return load()


def load():
    if not CONFIG.exists():
        raise RuntimeError('Stable signing has not been initialized. Run tools/sign_macos_app.py init once.')
    record = json.loads(CONFIG.read_text())
    if not re.fullmatch(r'[0-9A-F]{40}', record.get('identity','')) or Path(record.get('keychain','')) != KEYCHAIN:
        raise RuntimeError('The stable signing identity record is invalid; refusing ad-hoc signing.')
    return record


def sign(path, identifier=None):
    record = load()
    detach_signing_keychain()
    path = Path(path).resolve()
    if not path.exists():
        raise RuntimeError('The signing target does not exist.')
    if path.suffix == '.app':
        resources = path/'Contents/Resources'
        if resources.is_symlink():
            raise RuntimeError('Package real Resources before signing; the build-tree symlink is not distributable.')
        with (path/'Contents/Info.plist').open('rb') as stream:
            info = plistlib.load(stream)
        identifier = info['CFBundleIdentifier']
        executable = path/'Contents/MacOS'/info['CFBundleExecutable']
        running = run(['/bin/ps', '-axo', 'comm=']).splitlines()
        if str(executable) in (line.strip() for line in running):
            raise RuntimeError('Quit this copy of Studio before replacing or signing it.')
    if not identifier or not re.fullmatch(r'[A-Za-z0-9_.-]+',identifier):
        raise RuntimeError('A stable signing identifier is required.')
    login_unlocked()
    password = run(['/usr/bin/security', 'find-generic-password', '-a', ACCOUNT, '-s', SERVICE, '-w'], private=True)
    security_command(['unlock-keychain', '-p', password, str(KEYCHAIN)])
    try:
        # Pin the leaf certificate, not the changing executable CDHash. No
        # identifier-only or otherwise weakened designated requirement is used.
        requirement = f'designated => identifier "{identifier}" and certificate leaf = H"{record["identity"]}"'
        run(['/usr/bin/codesign', '--force', '--deep', '--sign', record['identity'],
             '--keychain', str(KEYCHAIN), '--identifier', identifier, '--timestamp=none',
             '--requirements', '='+requirement, str(path)])
        run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(path)])
        details = subprocess.run(['/usr/bin/codesign','-d','-r-',str(path)],text=True,capture_output=True)
        requirement_text = details.stdout+details.stderr
        if 'cdhash' in requirement_text.lower() or record['identity'].lower() not in requirement_text.lower():
            raise RuntimeError('The app did not retain the pinned stable signing identity.')
    finally:
        security_command(['lock-keychain', str(KEYCHAIN)])
    print('Signed and verified:', path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('init')
    signing = sub.add_parser('sign')
    signing.add_argument('path', type=Path)
    signing.add_argument('--identifier', help='Required for a standalone test executable; app IDs come from Info.plist.')
    args = parser.parse_args()
    previous = os.umask(0o077)
    try:
        if args.command == 'init':
            initialize()
            print('Stable private-build signing identity is ready. No system trust settings changed.')
        else:
            sign(args.path, args.identifier)
    except (RuntimeError, OSError, ValueError) as error:
        parser.exit(1, str(error)+'\n')
    finally:
        os.umask(previous)


if __name__ == '__main__':
    main()
