"""Build an emulator-only APK exercising the shared importer without game/runtime binaries."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / 'tools/on-device-setup'
sys.path.insert(0, str(SETUP))
import import_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    args = parser.parse_args()
    work = ROOT / 'out/on-device-import'
    java = work / 'java/io/github/mogeldev/scda'
    java.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'android/src/main/java/io/github/mogeldev/scda/GameImport.java', java)
    for name in ('GameSource', 'CompileCache', 'ExtractTools'):
        shutil.copy2(ROOT / f'android/src/main/java/io/github/mogeldev/scda/{name}.java', java)
    import_tools.stage(args.sdk, 'x86_64', work / 'jniLibs/x86_64', work / 'assets')
    env = dict(os.environ, ANDROID_HOME=str(args.sdk), ANDROID_SDK_ROOT=str(args.sdk))
    subprocess.run([str(ROOT / 'android/gradlew.bat'), '-p', str(ROOT / 'tools/on-device-import-test'),
        '--project-cache-dir', str(work / 'gradle'), 'assembleDebug'], env=env, check=True)


if __name__ == '__main__': main()
