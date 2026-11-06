"""Build and run the isolated compiler probe on a selected emulator or opted-in phone."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import time
import zipfile

import prepare

PACKAGE = 'io.github.mogeldev.scda.compilerprobe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', default=os.environ.get('ANDROID_HOME'), help='Android SDK path')
    parser.add_argument('--serial', default='emulator-5554', help='Target adb serial (Wi-Fi IP:port is supported)')
    parser.add_argument('--allow-phone', action='store_true', help='Allow installation of the separate probe APK on a phone')
    parser.add_argument('--host-apk', type=Path, help='Full ARM64 build: use this matching game APK as link-only host input')
    args = parser.parse_args()
    sdk = Path(args.sdk or '').resolve()
    adb = sdk / 'platform-tools/adb.exe'
    if not adb.is_file():
        parser.error('Pass --sdk pointing to the installed Android SDK (Windows host)')

    def device(*argv, **kwargs):
        return subprocess.run([str(adb), '-s', args.serial, *argv], check=True, **kwargs)

    def prop(name):
        return device('shell', 'getprop', name, capture_output=True, text=True).stdout.strip()

    abi = prop('ro.product.cpu.abi')
    if abi not in ('x86_64', 'arm64-v8a'):
        parser.error(f'Unsupported device ABI: {abi}')
    if prop('ro.kernel.qemu') != '1' and not args.allow_phone:
        parser.error('Physical device: pass --allow-phone to install only the separate compiler-probe APK')
    if args.host_apk and abi != 'arm64-v8a':
        parser.error('--host-apk full build currently requires an ARM64 device')
    prepare.main(abi)
    if args.host_apk:
        with zipfile.ZipFile(args.host_apk) as game:
            host = game.read('lib/arm64-v8a/libscda_host.so')
        if host[:6] != b'\x7fELF\x02\x01' or host[18:20] != b'\xb7\x00':
            parser.error('Host APK does not contain an ARM64 ELF64 host')
        (prepare.WORK / 'input/libscda_host.so').write_bytes(host)
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    # Separate static-STL executable: do not replace the Termux compiler's newer shared libc++.
    cmake = sdk / 'cmake/3.31.6/bin/cmake.exe'
    recomp_build = prepare.WORK / 'recompiler' / abi
    subprocess.run([str(cmake), '-S', str(prepare.HERE / 'recompiler'), '-B', str(recomp_build),
                    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                    f'-DCMAKE_MAKE_PROGRAM={sdk / "cmake/3.31.6/bin/ninja.exe"}',
                    f'-DCMAKE_TOOLCHAIN_FILE={sdk / "ndk/29.0.14206865/build/cmake/android.toolchain.cmake"}',
                    f'-DANDROID_ABI={abi}', '-DANDROID_PLATFORM=android-30',
                    '-DANDROID_STL=c++_static'], check=True)
    subprocess.run([str(cmake), '--build', str(recomp_build), '--parallel', '4'], check=True)
    subprocess.run([str(prepare.ROOT / 'android/gradlew.bat'), '-p', str(prepare.HERE),
                    '--project-cache-dir', str(prepare.WORK / 'gradle'), f'-Pabi={abi}', 'assembleDebug'],
                   env=env, check=True)
    apk = prepare.WORK / 'build/outputs/apk/debug/scda-compiler-probe-debug.apk'
    device('install', '-r', str(apk))
    device('shell', 'am', 'force-stop', PACKAGE)
    device('shell', 'run-as', PACKAGE, 'rm', '-rf', 'files/input')
    device('shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files/input')
    device('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/result.txt')
    # Stream private sources directly into app storage, never into shared /sdcard or /data/local/tmp.
    archive = io.BytesIO()
    digests = {}
    with tarfile.open(fileobj=archive, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted((prepare.WORK / 'input').rglob('*')):
            if path.is_file():
                name = path.relative_to(prepare.WORK / 'input').as_posix()
                digests['files/input/' + name] = hashlib.sha256(path.read_bytes()).hexdigest()
                tar.add(path, arcname=name)
    # exec-in is binary-safe; OEM shell -T may corrupt tar. Extraction can finish after adb exits.
    device('exec-in', 'run-as', PACKAGE, 'tar', '-xf', '-', '-C', 'files/input', input=archive.getvalue())
    transfer_deadline = time.monotonic() + 30
    while True:
        copied = subprocess.run([str(adb), '-s', args.serial, 'shell', 'run-as', PACKAGE,
                                 'sha256sum', *digests], capture_output=True, text=True)
        actual = {name: digest for digest, name in (line.split() for line in copied.stdout.splitlines())}
        if actual == digests:
            break
        if time.monotonic() >= transfer_deadline:
            raise SystemExit('Private input transfer failed SHA-256 validation; probe not started')
        time.sleep(1)
    device('shell', 'am', 'start', '-n', PACKAGE + '/.ProbeActivity',
           '--ez', 'full_build', 'true' if args.host_apk else 'false')
    deadline = time.monotonic() + (7200 if args.host_apk else 660)
    while time.monotonic() < deadline:
        result = subprocess.run([str(adb), '-s', args.serial, 'shell', 'run-as', PACKAGE,
                                 'cat', 'files/result.txt'], capture_output=True, text=True)
        if result.returncode == 0:
            print(result.stdout)
            (prepare.WORK / 'result.txt').write_text(result.stdout)
            (prepare.WORK / f'result-{abi}.txt').write_text(result.stdout)
            if not result.stdout.rstrip().endswith('\nPASS'):
                raise SystemExit('Compiler probe failed; see out/on-device-probe/result.txt')
            if args.host_apk:
                output = prepare.WORK / 'libscda_code-phone.so'
                with output.open('wb') as stream:
                    device('exec-out', 'run-as', PACKAGE, 'cat', 'files/libscda_code.so', stdout=stream)
                remote = device('shell', 'run-as', PACKAGE, 'sha256sum', 'files/libscda_code.so',
                                capture_output=True, text=True).stdout.split()[0]
                if hashlib.sha256(output.read_bytes()).hexdigest() != remote:
                    raise SystemExit('Compiled library transfer failed SHA-256 validation')
                print(f'Private full library saved to {output}; not installed into the game')
            return
        time.sleep(2)
    raise SystemExit('Probe timed out; inspect adb logcat -s CompilerProbe AndroidRuntime')


if __name__ == '__main__':
    main()
