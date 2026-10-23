#!/usr/bin/env python3
"""Build/run a synthetic native chat fixture; no Studio, accounts, or Keychain calls."""
import argparse
import os
import pathlib
import platform
import plistlib
import shlex
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch',choices=('arm64','x86_64'),default=platform.machine())
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--review',action='store_true',help='Leave the synthetic window visible after checks')
    args=parser.parse_args()
    root=pathlib.Path(__file__).resolve().parents[1]
    deps=root/'deps/build'/args.arch/'BambuStudio_deps/usr/local'
    wxconfig=deps/'bin/wx-config'
    output=root/'build'/('assistant-chat-checks-'+args.arch)
    output.mkdir(parents=True,exist_ok=True)
    def flags(option):
        return shlex.split(subprocess.check_output([str(wxconfig),*option],text=True))
    libraries=flags(['--libs','core,base'])
    # This bundled wx-config emits some bare filenames rather than linker flags.
    for i,library in enumerate(libraries):
        if library.endswith('.a') and not library.startswith('/'):
            libraries[i]=str(deps/'lib'/library)
        elif library.startswith('lib') and library.endswith('.tbd'):
            libraries[i]='-l'+library[3:-4]
    bundle=output/'AssistantChatChecks.app'
    contents=bundle/'Contents'
    (contents/'MacOS').mkdir(parents=True,exist_ok=True)
    binary=contents/'MacOS/AssistantChatChecks'
    next_binary=binary.with_suffix('.next')
    # Bundled wx is built without assertion/debug symbols, matching Studio.
    command=['clang++','-std=c++17','-DwxDEBUG_LEVEL=0','-arch',args.arch,'-mmacosx-version-min=12.0',
             '-I'+str(root/'src'),*flags(['--cxxflags']),
             str(root/'tests/studio_assistant_chat.test.cpp'),
             str(root/'src/slic3r/GUI/StudioAssistantAppearance.mm'),'-o',str(next_binary),*libraries]
    with (output/'build.log').open('w') as log:
        if subprocess.run(command,stdout=log,stderr=subprocess.STDOUT).returncode:
            raise SystemExit(f'Build failed; see {output / "build.log"}')
    # A review fixture may still be running. Never truncate its executable inode.
    os.replace(next_binary,binary)
    metadata={'CFBundleIdentifier':'org.charmsstudio.chat-checks',
              'CFBundleExecutable':binary.name,'CFBundleName':'Assistant Chat Checks',
              'CFBundlePackageType':'APPL','CFBundleInfoDictionaryVersion':'6.0',
              'CFBundleVersion':'1','CFBundleShortVersionString':'1.0',
              'LSMinimumSystemVersion':'12.0','NSHighResolutionCapable':True}
    next_plist=contents/'Info.plist.next'
    next_plist.write_bytes(plistlib.dumps(metadata))
    os.replace(next_plist,contents/'Info.plist')
    print(f'Built {bundle}',flush=True)
    if not args.build_only:
        subprocess.run([str(binary),*(['--review'] if args.review else [])],check=True,
                       timeout=None if args.review else 30)


if __name__=='__main__':
    main()
