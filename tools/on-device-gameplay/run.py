"""Install a separate gameplay app using a trusted matching APK and phone-built code."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import time
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = ROOT / 'out/on-device-gameplay'
PACKAGE = 'io.github.mogeldev.scda.phonebuilt'
LIBRARIES = ('libscda_host.so', 'libSDL3.so', 'libc++_shared.so', 'libmain.so')


def arm64_shared(data):
    return len(data) >= 64 and data[:6] == b'\x7fELF\x02\x01' and data[16:20] == b'\x03\x00\xb7\x00'


def stage_runtime(apk):
    dest = WORK / 'jniLibs/arm64-v8a'
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(apk) as archive:
        for name in LIBRARIES:
            data = archive.read('lib/arm64-v8a/' + name)
            if not arm64_shared(data):
                raise ValueError(f'Invalid ARM64 shared runtime: {name}')
            (dest / name).write_bytes(data)


def game_inputs(assets, code):
    if not arm64_shared(code.read_bytes()[:64]):
        raise ValueError('Phone-built code must be an ARM64 ELF64 shared library')
    if hashlib.sha1((assets / 'default.xbe').read_bytes()).hexdigest() != '91290baf93c2f6ae735a3106c48734665c826d84':
        raise ValueError('Unsupported default.xbe')
    files = [(code, 'libscda_code.so'), (assets / 'default.xbe', 'assets/default.xbe')]
    for directory in ('Default', 'Media'):
        children = sorted(p for p in (assets / directory).rglob('*') if p.is_file())
        if not children:
            raise ValueError(f'Missing campaign assets: {directory}')
        files.extend((p, 'assets/' + p.relative_to(assets).as_posix()) for p in children)
    for path, name in files:
        if path.is_symlink() or '\n' in name or '\r' in name or '\\' in name:
            raise ValueError(f'Unsupported input path: {path}')
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--allow-phone', action='store_true')
    parser.add_argument('--host-apk', required=True, type=Path)
    parser.add_argument('--code-library', required=True, type=Path)
    parser.add_argument('--assets', type=Path, default=ROOT / 'assets')
    args = parser.parse_args()
    adb = args.sdk / 'platform-tools/adb.exe'

    def device(*argv, **kwargs):
        return subprocess.run([str(adb), '-s', args.serial, *argv], check=True, **kwargs)

    def shell(command, **kwargs):
        return device('shell', command, **kwargs)

    def prop(name):
        return device('shell', 'getprop', name, capture_output=True, text=True).stdout.strip()

    if prop('ro.product.cpu.abi') != 'arm64-v8a':
        parser.error('Gameplay test requires an ARM64 device')
    if prop('ro.kernel.qemu') != '1' and not args.allow_phone:
        parser.error('Physical device requires --allow-phone')
    files = game_inputs(args.assets.resolve(), args.code_library.resolve())
    stage_runtime(args.host_apk)
    env = dict(os.environ, ANDROID_HOME=str(args.sdk.resolve()), ANDROID_SDK_ROOT=str(args.sdk.resolve()))
    subprocess.run([str(ROOT / 'android/gradlew.bat'), '-p', str(HERE),
                    '--project-cache-dir', str(WORK / 'gradle'), 'assembleDebug'], env=env, check=True)
    apk = WORK / 'build/outputs/apk/debug/scda-phone-built-test-debug.apk'
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        if any(n.endswith(('.xbe', '.cpp', '.o', '/libscda_code.so')) for n in names):
            raise ValueError('Refusing APK with embedded game code/assets')
        for name in LIBRARIES:
            if archive.read('lib/arm64-v8a/' + name) != (WORK / 'jniLibs/arm64-v8a' / name).read_bytes():
                raise ValueError(f'Packaged runtime differs from matching APK: {name}')
    device('install', '-r', str(apk))
    external = '/sdcard/Android/data/' + PACKAGE + '/files'
    # OEM FUSE denies run-as external storage; adb shell can write this app-specific folder.
    exists = shell('test -d ' + external + '/game && echo exists || true', capture_output=True, text=True)
    if 'exists' in exists.stdout:
        raise SystemExit('Test game folder exists; not overwritten. Launch the test app manually, or uninstall ONLY ' + PACKAGE + ' to start fresh.')
    # With no published game, setup only creates this app's external files directory.
    device('shell', 'am', 'start', '-n', PACKAGE + '/io.github.mogeldev.scda.SetupActivity')
    time.sleep(2)
    device('shell', 'am', 'force-stop', PACKAGE)
    size = sum(p.stat().st_size for p, _ in files)
    free = int(shell('df -k ' + external, capture_output=True, text=True).stdout.splitlines()[-1].split()[3]) * 1024
    if free < size + 512 * 1024 * 1024:
        raise SystemExit('Not enough free device storage for a separate game-data copy')
    archive_path = WORK / 'private-game.tar'
    manifest = WORK / 'SHA256SUMS'
    print(f'Preparing private transfer: {len(files)} files, {size / 1024**3:.2f} GiB', flush=True)
    hashes = []
    with tarfile.open(archive_path, 'w', format=tarfile.PAX_FORMAT) as archive:
        for path, name in files:
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            hashes.append(digest + '  ' + name + '\n')
            archive.add(path, arcname=name, recursive=False)
        manifest.write_text(''.join(hashes), encoding='utf-8', newline='\n')
        archive.add(manifest, arcname='SHA256SUMS')
    # Staging and atomic rename keep partial input from launching. Never touch other app IDs.
    shell('mkdir -p ' + external + '/.phone-built-stage')
    print('Transferring into separate test-app storage...', flush=True)
    with archive_path.open('rb') as stream:
        device('exec-in', 'tar', '-xf', '-', '-C', external + '/.phone-built-stage', stdin=stream)
    deadline = time.monotonic() + 180
    while True:
        checked = shell('cd ' + external + '/.phone-built-stage && sha256sum -c SHA256SUMS; echo HASH_EXIT:$?',
                      capture_output=True, text=True)
        if checked.stdout.count(': OK') == len(files) and 'HASH_EXIT:0' in checked.stdout and 'FAILED' not in checked.stdout:
            break
        if time.monotonic() >= deadline:
            raise SystemExit('Private input hash validation failed; game not launched')
        time.sleep(2)
    shell('mv ' + external + '/.phone-built-stage ' + external + '/game')
    archive_path.unlink()
    print('All private input hashes verified. Launching SCDA Phone-built Test; existing game untouched.', flush=True)
    device('shell', 'am', 'start', '-n', PACKAGE + '/io.github.mogeldev.scda.SetupActivity')


if __name__ == '__main__':
    main()
