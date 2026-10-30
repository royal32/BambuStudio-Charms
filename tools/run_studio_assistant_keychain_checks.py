#!/usr/bin/env python3
"""Native fake-credential checks; optional isolated login-Keychain partition test."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/keychain-native-checks'


def run(args, expected=0):
    started=time.monotonic()
    process=subprocess.run([str(x) for x in args],text=True,capture_output=True,timeout=8)
    if process.returncode!=expected:
        raise RuntimeError(f'{Path(str(args[0])).name}: expected exit {expected}, received {process.returncode}: {process.stderr.strip()}')
    return time.monotonic()-started


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--login-synthetic',action='store_true',help='Also test one UUID-named fake item in the login Keychain; never touch real credentials.')
    args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('charms_signing',ROOT/'tools/sign_macos_app.py')
    signing=importlib.util.module_from_spec(spec); spec.loader.exec_module(signing)
    signing.load() # Never generate or rotate a signing identity as part of tests.
    command=['clang++','-std=c++17','-Wno-deprecated-declarations','-Wno-deprecated-literal-operator','-I',str(ROOT/'src'),
             str(ROOT/'tests/studio_assistant_keychain.test.cpp'),str(ROOT/'src/slic3r/GUI/StudioAssistantKeychain.cpp'),
             '-framework','Security','-framework','CoreFoundation']
    first,second,foreign=[OUT/x for x in ('stable-probe','next-probe','foreign-probe')]
    run(command+['-o',first]); run(command+['-DKEYCHAIN_TEST_GENERATION=2','-o',second])
    signing.sign(first,'org.charmsstudio.keychain-test'); signing.sign(second,'org.charmsstudio.keychain-test')
    original=OUT/'original-probe';shutil.copy2(first,original)
    assert original.read_bytes()!=second.read_bytes()
    other_identifier=OUT/'other-identifier-probe';shutil.copy2(second,other_identifier)
    signing.sign(other_identifier,'org.charmsstudio.other-keychain-test')
    run([first,'--ipc-check']) # Signed self-child, anonymous pipes, service validation.
    run([first,'--ipc-check-reaped']) # Host SIGCHLD handling cannot break a valid response.
    run([first,'--ipc-cancel-check']) # Explicit authorization can stop without leaving a child/dialog.
    run([first,'--internal-charms-keychain'],77) # No trusted parent/pipes: no access.
    shutil.copy2(second,foreign)
    run(['/usr/bin/codesign','--force','--sign','-',foreign])
    keychain=OUT/(uuid.uuid4().hex+'.keychain-db')
    service='charms-keychain-test/'+str(uuid.uuid4())
    try:
        run([first,'create',keychain,service])
        for _ in range(3): run([first,'read',keychain,service])
        run([second,'read',keychain,service]) # Same identity at a different path.
        # Different bytes, same filename, same pinned certificate/identifier.
        replacement=OUT/'replacement-probe'
        shutil.copy2(second,replacement); os.replace(replacement,first)
        for _ in range(3): run([first,'read',keychain,service])
        run([first,'update',keychain,service])
        run([first,'read',keychain,service])
        run([first,'lock',keychain,service])
        assert run([first,'read',keychain,service],2)<3
        run([first,'unlock',keychain,service])
        assert run([foreign,'read',keychain,service],2)<3
        assert run([foreign,'update',keychain,service],2)<3
        assert run([foreign,'delete',keychain,service],2)<3
        assert run([other_identifier,'read',keychain,service],2)<3
        run([first,'read',keychain,service])
        valid=OUT/'valid-probe'; shutil.copy2(first,valid)
        try:
            shutil.copy2(foreign,replacement); os.replace(replacement,first) # Wrong signature, original pathname.
            assert run([first,'read',keychain,service],2)<3
        finally:
            shutil.copy2(valid,replacement); os.replace(replacement,first)
        run([first,'delete',keychain,service])
        # An older record keeps its original encrypted data while a verified,
        # signed replacement is created. Later reads never resurrect stale data.
        run([first,'create_legacy',keychain,service])
        assert run([foreign,'read',keychain,service],2)<3
        run([first,'read',keychain,service])
        run([first,'versions',keychain,service])
        run([first,'change_legacy',keychain,service])
        run([second,'read',keychain,service])
        run([first,'update',keychain,service])
        run([second,'read',keychain,service])
        run([first,'delete',keychain,service])
    finally:
        # Always remove the disposable Keychain, including on timeout/failure.
        # This never targets the login Keychain or a real account's credentials.
        if keychain.exists(): run(['/usr/bin/security','delete-keychain',keychain])
    print('PASS: restarts and a changed executable retain access with the same signing certificate.')
    print('PASS: authorized legacy migration preserves its original item, prefers signed storage, and sign-out removes both versions.')
    print('PASS: locked Keychain and foreign signature fail promptly without authorization UI.')
    print('PASS: failed foreign reads/writes/deletes preserve the original synthetic credential.')
    print('PASS: private self-child transport validates the signed parent and rejects invalid services/direct invocation.')
    print('PASS: disposable Keychain removed. No real account credential was read.')
    if args.login_synthetic:
        if not signing.load().get('apple_identity'):
            raise RuntimeError('The login-partition regression requires an explicitly configured Apple identity.')
        service='charms-keychain-test/'+str(uuid.uuid4())
        created=False
        try:
            run([original,'create','--login-synthetic-only',service]);created=True
            run([second,'read','--login-synthetic-only',service])
            run([second,'update','--login-synthetic-only',service])
            run([original,'read','--login-synthetic-only',service])
            assert run([foreign,'read','--login-synthetic-only',service],2)<3
            assert run([foreign,'update','--login-synthetic-only',service],2)<3
            assert run([foreign,'cleanup_login','--login-synthetic-only',service],2)<3
            assert run([other_identifier,'read','--login-synthetic-only',service],2)<3
            run([second,'read','--login-synthetic-only',service])
        finally:
            if created:run([original,'cleanup_login','--login-synthetic-only',service])
        print('PASS: real login-Keychain synthetic item survives changed signed bytes/paths; foreign signature and same-team wrong identifier denied; only that item removed.')


if __name__=='__main__':
    old=os.umask(0o077)
    try: main()
    finally: os.umask(old)
