#!/usr/bin/env python3
"""Opt-in Mac plug-in signature checks; no plug-in loading or account access."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=('arm64','x86_64'),default=platform.machine())
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--plugin',type=Path,default=Path.home()/'Library/Application Support/BambuStudio/plugins/libbambu_networking.dylib')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    deps=root/'deps/build'/args.arch/'BambuStudio_deps/usr/local'
    output=root/'build/network-signature-checks'
    output.mkdir(parents=True,exist_ok=True)
    binary=output/('checks-'+args.arch)
    fresh=binary.with_suffix('.next')
    command=['clang++','-std=c++17','-arch',args.arch,'-mmacosx-version-min=12.0',
             '-I'+str(root/'src'),'-I'+str(deps/'include'),
             str(root/'tests/network_module_signature.test.cpp'),
             str(root/'src/slic3r/Utils/CertificateVerify.cpp'),'-o',str(fresh),
             '-L'+str(deps/'lib'),*('-lboost_'+name for name in
                 ('log','filesystem','thread','chrono','atomic','date_time')),
             '-framework','Security','-framework','CoreFoundation']
    with (output/('build-'+args.arch+'.log')).open('w') as log:
        subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    os.replace(fresh,binary)
    private=output/('private-'+args.arch)
    fresh=private.with_suffix('.next')
    shutil.copy2(binary,fresh)
    subprocess.run(['python3',str(root/'tools/sign_macos_app.py'),'sign',str(fresh),
                    '--identifier','libbambu_networking'],check=True)
    os.replace(fresh,private)
    if not args.build_only:
        with tempfile.TemporaryDirectory(prefix='charms-signature-') as folder:
            subprocess.run([str(binary),str(args.plugin),str(private),folder],check=True,timeout=30)
    print('Built signature fixtures:',output)


if __name__=='__main__':
    main()
