"""Stage on-device compiler/import tools; optionally build the isolated setup APK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PROBE = ROOT / 'tools/on-device-probe'
sys.path.insert(0, str(PROBE))
import prepare
import import_tools


def stage(sdk, work, host_apk=None):
    # No XBE/reference/generated code is staged. Production builds its own matching host via CMake.
    prepare.main('arm64-v8a', stage_private=False)
    cmake = sdk / 'cmake/3.31.6/bin/cmake.exe'
    for source, native, targets, stl in [
        (PROBE / 'recompiler', prepare.WORK / 'recompiler/arm64-v8a', [], 'c++_static'),
        (PROBE, work / 'checker', ['--target', 'load_check'], 'none'),
    ]:
        subprocess.run([str(cmake), '-S', str(source), '-B', str(native), '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja.exe',
            f'-DCMAKE_TOOLCHAIN_FILE={sdk}/ndk/29.0.14206865/build/cmake/android.toolchain.cmake',
            '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=' + stl], check=True)
        subprocess.run([str(cmake), '--build', str(native), *targets, '--parallel', '4'], check=True)
    dest = work / 'jniLibs/arm64-v8a'
    if dest.exists(): shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for file in (prepare.WORK / 'jniLibs/arm64-v8a').glob('*.so'):
        if file.name in ('libprobe.so', 'libprobe_recomp.so', 'libprobe_load.so'): continue
        data = prepare.relocate_elf(file.read_bytes(), {'libc++_shared.so': 'libc++_compil.so'})
        name = 'libc++_compil.so' if file.name == 'libc++_shared.so' else file.name
        (dest / name).write_bytes(data)
    for name in ('libprobe_recomp.so', 'libprobe_load.so'):
        shutil.copy2(prepare.WORK / 'jniLibs/arm64-v8a' / name, dest)
    if host_apk:
        with zipfile.ZipFile(host_apk) as archive:
            for name in ('libscda_host.so', 'libSDL3.so', 'libc++_shared.so', 'libmain.so'):
                data = archive.read('lib/arm64-v8a/' + name)
                if data[:6] != b'\x7fELF\x02\x01' or data[16:20] != b'\x03\x00\xb7\x00':
                    raise ValueError('Invalid ARM64 runtime: ' + name)
                (dest / name).write_bytes(data)
    assets = work / 'assets'
    if assets.exists(): shutil.rmtree(assets)
    compiler = assets / 'compiler'
    for name in ('scda_v2.toml', 'xdk_symbols.toml'):
        path = compiler / 'input/config' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'config' / name, path)
    (compiler / 'input/cpu').mkdir(parents=True)
    for name in ('x86_context.h', 'x86_ops.h'):
        shutil.copy2(ROOT / 'src/cpu' / name, compiler / 'input/cpu' / name)
    shutil.copytree(ROOT / 'src/cpu/freestanding', compiler / 'input/cpu/freestanding')
    import_tools.stage(sdk, 'arm64-v8a', dest, assets)
    licenses = assets / 'licenses'
    for path in (ROOT / 'android/licenses').glob('*.txt'): shutil.copy2(path, licenses / path.name)
    shutil.copy2(ROOT / 'tools/LLVM-LICENSE.txt', licenses / 'LLVM.txt')
    shutil.copy2(ROOT / 'android/TOOLCHAIN-NOTICES.md', licenses / 'TOOLCHAIN-NOTICES.md')
    deps = prepare.WORK / 'recompiler/arm64-v8a/_deps'
    for name, path in [('Zydis', deps / 'zydis-src/LICENSE'),
                       ('Zycore', deps / 'zydis-src/dependencies/zycore/LICENSE'),
                       ('tomlplusplus', deps / 'tomlplusplus-src/LICENSE')]:
        shutil.copy2(path, licenses / (name + '.txt'))
    packages = json.loads((PROBE / 'packages-arm64-v8a.json').read_text())
    for package in packages:
        with prepare.unpack_deb((prepare.WORK / Path(package['path']).name).read_bytes()) as archive:
            for member in archive:
                if member.isfile() and 'share/doc/' in member.name and any(
                        word in Path(member.name).name.lower() for word in ('copyright', 'license', 'copying')):
                    name = Path(package['path']).name + '-' + Path(member.name).name + '.txt'
                    (licenses / name).write_bytes(archive.extractfile(member).read())
    # Runtime binaries are also fingerprinted on the device, so a new host never reuses stale objects.
    digest = hashlib.sha256()
    sources = [*dest.glob('*.so'), *compiler.rglob('*'),
               *ROOT.glob('android/src/main/java/io/github/mogeldev/scda/*.java')]
    for path in sorted(p for p in sources if p.is_file()):
        digest.update(path.name.encode()); digest.update(path.read_bytes())
    (compiler / 'version').write_text(digest.hexdigest())
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--host-apk', type=Path, help='Matching runtime for the isolated setup APK only')
    parser.add_argument('--production', action='store_true', help='Stage tools for android/; runtime comes from its own CMake build')
    parser.add_argument('--stage-only', action='store_true')
    args = parser.parse_args()
    if not args.production and not args.host_apk: parser.error('--host-apk is required for the isolated setup app')
    if args.production and args.host_apk: parser.error('Production must build its own runtime, not copy another APK')
    sdk = args.sdk.resolve()
    work = stage(sdk, ROOT / ('out/android-tools' if args.production else 'out/on-device-setup'), args.host_apk)
    if args.stage_only: return
    project = ROOT / 'android' if args.production else HERE
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    subprocess.run([str(ROOT / 'android/gradlew.bat'), '-p', str(project), '--project-cache-dir',
        str(work / 'gradle'), 'assembleDebug', *(['-PtoolsStaged', '-Pabi=arm64-v8a'] if args.production else [])], env=env, check=True)
    apk = ROOT / 'android/build/outputs/apk/debug/scda-debug.apk' if args.production else work / 'build/outputs/apk/debug/scda-on-device-setup-debug.apk'
    with zipfile.ZipFile(apk) as archive:
        assert not any(n.endswith(('.xbe', '.iso', '.cpp', '.o', '/libscda_code.so')) for n in archive.namelist())
        assert len(archive.read('assets/compiler/version')) == 64
    print(f'Built APK (not installed): {apk}')


if __name__ == '__main__': main()
