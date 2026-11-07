"""Run only the isolated import APK, explicitly on an emulator (no phone installation)."""
import argparse
import hashlib
import shlex
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/on-device-setup'))
import uuid

PACKAGE = 'io.github.mogeldev.scda.importtest'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--expect-failure', action='store_true')
    parser.add_argument('--cancel', action='store_true')
    parser.add_argument('--reuse-input', action='store_true', help='Use an already transferred input after a harness interruption')
    parser.add_argument('--reference', type=Path, help='Privately compare every imported campaign file to this extracted game folder')
    args = parser.parse_args()
    adb = args.sdk / 'platform-tools/adb.exe'
    def device(*argv, **kwargs):
        # Shared binary-safe transfer is hardcoded to the setup package; scope it to this test package.
        argv = [a.replace('io.github.mogeldev.scda.localsetup', PACKAGE) for a in argv]
        return subprocess.run([str(adb), '-s', args.serial, *argv], check=True, **kwargs)
    if device('shell', 'getprop', 'ro.kernel.qemu', capture_output=True, text=True).stdout.strip() != '1':
        parser.error('This harness is emulator-only; phone validation belongs to the final setup test')
    boot_deadline = time.monotonic() + 180
    while device('shell', 'getprop', 'sys.boot_completed', capture_output=True, text=True).stdout.strip() != '1':
        if time.monotonic() >= boot_deadline: raise SystemExit('Emulator did not finish booting')
        time.sleep(2)
    device('install', '-r', str(ROOT / 'out/on-device-import/build/outputs/apk/debug/scda-import-test-debug.apk'))
    device('shell', 'am', 'force-stop', PACKAGE)
    device('shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files')
    if not args.reuse_input:
        temporary = '/data/local/tmp/scda-import-' + uuid.uuid4().hex
        try:
            device('push', str(args.source), temporary)
            device('shell', 'chmod', '644', temporary)
            device('shell', 'run-as', PACKAGE, 'cp', temporary, 'files/source.input')
        finally:
            device('shell', 'rm', '-f', temporary)
    with args.source.open('rb') as stream: expected_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    copied = device('shell', 'run-as', PACKAGE, 'sha256sum', 'files/source.input', capture_output=True, text=True).stdout
    if not copied.split() or copied.split()[0] != expected_hash: raise SystemExit('Private input hash mismatch')
    device('logcat', '-c')
    device('shell', 'am', 'start', '-n', PACKAGE + '/io.github.mogeldev.scda.ImportTestActivity',
           '--ez', 'cancel', str(args.cancel).lower())
    deadline = time.monotonic() + 1800
    while time.monotonic() < deadline:
        log = device('logcat', '-d', '-s', 'SCDAImportTest', 'SCDAImport', 'AndroidRuntime', capture_output=True, text=True).stdout
        if 'IMPORT_PASS' in log or 'IMPORT_FAIL' in log:
            print(log)
            expected = 'IMPORT_FAIL published=false staging=false' if args.expect_failure or args.cancel else 'IMPORT_PASS assets=true'
            if expected not in log: raise SystemExit('Unexpected import result')
            report = ROOT / 'out/on-device-import' / (args.source.name + ('.cancel' if args.cancel else '') + '.log')
            report.write_text(log)
            if args.reference and not (args.cancel or args.expect_failure):
                command = "cd files/result/game/assets && find . -type f -exec sha256sum '{}' ';'"
                output = device('shell', 'run-as ' + PACKAGE + ' sh -c ' + shlex.quote(command), capture_output=True, text=True).stdout
                actual = {line[66:].removeprefix('./'): line[:64] for line in output.splitlines()}
                expected = {}
                paths = [args.reference / 'default.xbe',
                         *[p for name in ('Default', 'Media') for p in (args.reference / name).rglob('*') if p.is_file()]]
                for path in paths:
                    with path.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                    expected[path.relative_to(args.reference).as_posix()] = digest
                if actual != expected: raise SystemExit('Imported campaign hashes differ from private reference')
                print(f'PASS: all {len(expected)} imported campaign file hashes match', flush=True)
                with report.open('a') as stream: stream.write(f'All {len(expected)} campaign SHA-256 hashes match private reference.\\n')
            return
        time.sleep(2)
    raise SystemExit('Timed out waiting for import result')


if __name__ == '__main__': main()
