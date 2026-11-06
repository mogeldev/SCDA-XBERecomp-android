"""Build and run the isolated compiler probe on an explicitly selected emulator."""
import argparse
import io
import os
from pathlib import Path
import subprocess
import tarfile
import time

import prepare

PACKAGE = 'io.github.mogeldev.scda.compilerprobe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', default=os.environ.get('ANDROID_HOME'), help='Android SDK path')
    parser.add_argument('--serial', default='emulator-5554', help='x86-64 emulator adb serial')
    args = parser.parse_args()
    sdk = Path(args.sdk or '').resolve()
    adb = sdk / 'platform-tools/adb.exe'
    if not adb.is_file():
        parser.error('Pass --sdk pointing to the installed Android SDK (Windows host)')

    def device(*argv, **kwargs):
        return subprocess.run([str(adb), '-s', args.serial, *argv], check=True, **kwargs)

    def prop(name):
        return device('shell', 'getprop', name, capture_output=True, text=True).stdout.strip()

    if prop('ro.kernel.qemu') != '1' or prop('ro.product.cpu.abi') != 'x86_64':
        parser.error('This prototype only installs on an x86-64 Android emulator; no phone changes allowed')
    prepare.main()
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    subprocess.run([str(prepare.ROOT / 'android/gradlew.bat'), '-p', str(prepare.HERE),
                    '--project-cache-dir', str(prepare.WORK / 'gradle'), 'assembleDebug'],
                   env=env, check=True)
    apk = prepare.WORK / 'build/outputs/apk/debug/scda-compiler-probe-debug.apk'
    device('install', '-r', str(apk))
    device('shell', 'am', 'force-stop', PACKAGE)
    device('shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files/input')
    device('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/result.txt')
    # Stream private sources directly into app storage, never into shared /sdcard or /data/local/tmp.
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        for path in sorted((prepare.WORK / 'input').rglob('*')):
            if path.is_file():
                tar.add(path, arcname=path.relative_to(prepare.WORK / 'input').as_posix())
    device('shell', '-T', 'run-as', PACKAGE, 'tar', '-xf', '-', '-C', 'files/input', input=archive.getvalue())
    device('shell', 'am', 'start', '-n', PACKAGE + '/.ProbeActivity')
    deadline = time.monotonic() + 420
    while time.monotonic() < deadline:
        result = subprocess.run([str(adb), '-s', args.serial, 'shell', 'run-as', PACKAGE,
                                 'cat', 'files/result.txt'], capture_output=True, text=True)
        if result.returncode == 0:
            print(result.stdout)
            (prepare.WORK / 'result.txt').write_text(result.stdout)
            if not result.stdout.rstrip().endswith('\nPASS'):
                raise SystemExit('Compiler probe failed; see out/on-device-probe/result.txt')
            return
        time.sleep(2)
    raise SystemExit('Probe timed out; inspect adb logcat -s CompilerProbe AndroidRuntime')


if __name__ == '__main__':
    main()
