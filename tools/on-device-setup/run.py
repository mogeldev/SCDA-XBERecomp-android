"""Drive the private assets-only import through the experimental setup UI (Windows)."""
import argparse
import hashlib
import io
import shlex
from pathlib import Path
import subprocess
import tarfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'out/on-device-setup'
PACKAGE = 'io.github.mogeldev.scda.localsetup'
COMPONENT = PACKAGE + '/io.github.mogeldev.scda.LocalSetupActivity'


def transfer_zip(device, archive_path, chunk_bytes=64 * 1024 * 1024):
    """Bound Windows adb stdin transfers; never append an unverified chunk."""
    with archive_path.open('rb') as stream:
        expected = hashlib.file_digest(stream, 'sha256').hexdigest()
    device('shell', 'run-as', PACKAGE, 'rm', '-f', 'files/test-assets.zip')
    with archive_path.open('rb') as stream:
        while chunk := stream.read(chunk_bytes):
            payload = io.BytesIO()
            with tarfile.open(fileobj=payload, mode='w') as archive:
                entry = tarfile.TarInfo('zip.part')
                entry.size = len(chunk)
                archive.addfile(entry, io.BytesIO(chunk))
            device('exec-in', 'run-as', PACKAGE, 'tar', '-xf', '-', '-C', 'files', input=payload.getvalue())
            chunk_hash = hashlib.sha256(chunk).hexdigest()
            for attempt in range(30):
                copied = device('shell', f'run-as {PACKAGE} sha256sum files/zip.part 2>/dev/null || true',
                                capture_output=True, text=True)
                if copied.stdout.split() and copied.stdout.split()[0] == chunk_hash: break
                time.sleep(1)
            else: raise SystemExit('Private ZIP chunk mismatch; setup not started')
            command = 'cat files/zip.part >> files/test-assets.zip && rm files/zip.part'
            device('shell', 'run-as ' + PACKAGE + ' sh -c ' + shlex.quote(command))
    deadline = time.monotonic() + 120
    while True:
        result = device('shell', f'run-as {PACKAGE} sha256sum files/test-assets.zip 2>/dev/null || true',
                        capture_output=True, text=True)
        if result.stdout.split() and result.stdout.split()[0] == expected: break
        if time.monotonic() >= deadline: raise SystemExit('Private ZIP transfer hash mismatch; setup not started')
        time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--allow-phone', action='store_true')
    parser.add_argument('--assets', type=Path, default=ROOT / 'assets')
    args = parser.parse_args()
    adb = args.sdk / 'platform-tools/adb.exe'

    def device(*argv, **kwargs):
        return subprocess.run([str(adb), '-s', args.serial, *argv], check=True, **kwargs)

    def prop(name):
        return device('shell', 'getprop', name, capture_output=True, text=True).stdout.strip()

    if prop('ro.product.cpu.abi') != 'arm64-v8a': parser.error('ARM64 required')
    if prop('ro.kernel.qemu') != '1' and not args.allow_phone: parser.error('Pass --allow-phone for the separate setup test app')
    xbe = args.assets / 'default.xbe'
    if hashlib.sha1(xbe.read_bytes()).hexdigest() != '91290baf93c2f6ae735a3106c48734665c826d84':
        parser.error('Unsupported default.xbe')
    archive_path = WORK / 'assets-only.zip'
    files = [xbe]
    for name in ('Default', 'Media'):
        children = sorted(p for p in (args.assets / name).rglob('*') if p.is_file())
        if not children: parser.error('Missing campaign assets: ' + name)
        files.extend(children)
    with zipfile.ZipFile(archive_path, 'w', compression=zipfile.ZIP_STORED) as archive:
        for path in files:
            if path.is_symlink(): raise ValueError('Refusing symlink: ' + str(path))
            archive.write(path, 'assets/' + path.relative_to(args.assets).as_posix())
    device('install', '-r', str(WORK / 'build/outputs/apk/debug/scda-on-device-setup-debug.apk'))
    external = '/sdcard/Android/data/' + PACKAGE + '/files/game'
    exists = device('shell', f'test -d {external} && echo exists || true', capture_output=True, text=True)
    if 'exists' in exists.stdout:
        raise SystemExit('Setup assets already exist; open SCDA Local Setup Test and tap Compile / resume. No files overwritten.')
    device('shell', 'am', 'force-stop', PACKAGE)
    device('shell', 'run-as', PACKAGE, 'mkdir', '-p', 'files')
    transfer_zip(device, archive_path)
    device('shell', 'am', 'start', '-n', COMPONENT, '--ez', 'test_import', 'true')
    print('Assets-only ZIP transferred/verified. Keep setup foreground; Back cancels, Compile / resume reuses verified work.')
    print('Observe: adb -s SERIAL logcat -s SCDABuild SCDAImport AndroidRuntime')


if __name__ == '__main__': main()
