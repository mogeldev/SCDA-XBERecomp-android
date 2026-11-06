"""Build the isolated ARM64 setup integration; never install the production app."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = ROOT / 'out/on-device-setup'
PROBE = ROOT / 'tools/on-device-probe'
sys.path.insert(0, str(PROBE))
import prepare
import import_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--host-apk', required=True, type=Path)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    # Reuse pinned download/ELF relocation, without staging XBE or Windows reference outputs.
    prepare.main('arm64-v8a', stage_private=False)
    cmake = sdk / 'cmake/3.31.6/bin/cmake.exe'
    native = prepare.WORK / 'recompiler/arm64-v8a'
    subprocess.run([str(cmake), '-S', str(PROBE / 'recompiler'), '-B', str(native), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja.exe',
        f'-DCMAKE_TOOLCHAIN_FILE={sdk}/ndk/29.0.14206865/build/cmake/android.toolchain.cmake',
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=c++_static'], check=True)
    subprocess.run([str(cmake), '--build', str(native), '--parallel', '4'], check=True)
    checker = WORK / 'checker'
    subprocess.run([str(cmake), '-S', str(PROBE), '-B', str(checker), '-G', 'Ninja',
        f'-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja.exe',
        f'-DCMAKE_TOOLCHAIN_FILE={sdk}/ndk/29.0.14206865/build/cmake/android.toolchain.cmake',
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=none'], check=True)
    subprocess.run([str(cmake), '--build', str(checker), '--target', 'load_check'], check=True)
    dest = WORK / 'jniLibs/arm64-v8a'
    if dest.exists(): shutil.rmtree(dest)
    dest.mkdir(parents=True)
    # Native loader SONAME collision: keep Termux's C++ runtime under a distinct same-length name.
    for file in (prepare.WORK / 'jniLibs/arm64-v8a').glob('*.so'):
        if file.name == 'libprobe.so': continue
        data = prepare.relocate_elf(file.read_bytes(), {'libc++_shared.so': 'libc++_compil.so'})
        name = 'libc++_compil.so' if file.name == 'libc++_shared.so' else file.name
        (dest / name).write_bytes(data)
    with zipfile.ZipFile(args.host_apk) as archive:
        for name in ('libscda_host.so', 'libSDL3.so', 'libc++_shared.so', 'libmain.so'):
            data = archive.read('lib/arm64-v8a/' + name)
            if data[:6] != b'\x7fELF\x02\x01' or data[16:20] != b'\x03\x00\xb7\x00':
                raise ValueError('Invalid ARM64 runtime: ' + name)
            (dest / name).write_bytes(data)
    assets = WORK / 'assets/compiler'
    if assets.exists(): shutil.rmtree(assets)
    for name in ('scda_v2.toml', 'xdk_symbols.toml'):
        path = assets / 'input/config' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'config' / name, path)
    (assets / 'input/cpu').mkdir(parents=True)
    for name in ('x86_context.h', 'x86_ops.h'):
        shutil.copy2(ROOT / 'src/cpu' / name, assets / 'input/cpu' / name)
    shutil.copytree(ROOT / 'src/cpu/freestanding', assets / 'input/cpu/freestanding')
    import_tools.stage(sdk, 'arm64-v8a', dest, WORK / 'assets')
    policy = (PROBE / 'src/main/java/io/github/mogeldev/scda/compilerprobe/BuildJobs.java').read_text()
    java = WORK / 'policy/io/github/mogeldev/scda/BuildJobs.java'
    java.parent.mkdir(parents=True, exist_ok=True)
    java.write_text(policy.replace('package io.github.mogeldev.scda.compilerprobe;', 'package io.github.mogeldev.scda;'))
    # Invalidate all cached work when tools, inputs, flags/policy or compiler code change.
    digest = hashlib.sha256()
    sources = [*dest.glob('*.so'), *assets.rglob('*'), *HERE.glob('src/main/java/**/*.java'), java,
               ROOT / 'android/src/main/java/io/github/mogeldev/scda/GameImport.java']
    for path in sorted(p for p in sources if p.is_file()):
        digest.update(path.name.encode()); digest.update(path.read_bytes())
    (assets / 'version').write_text(digest.hexdigest())
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    subprocess.run([str(ROOT / 'android/gradlew.bat'), '-p', str(HERE), '--project-cache-dir',
        str(WORK / 'gradle'), 'assembleDebug'], env=env, check=True)
    apk = WORK / 'build/outputs/apk/debug/scda-on-device-setup-debug.apk'
    with zipfile.ZipFile(apk) as archive:
        assert not any(n.endswith(('.xbe', '.cpp', '.o', '/libscda_code.so')) for n in archive.namelist())
        assert len(archive.read('assets/compiler/version')) == 64
    print(f'Isolated setup APK: {apk}')


if __name__ == '__main__': main()
