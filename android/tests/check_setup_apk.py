"""Inspect a production APK without installing it or reading any private game inputs."""
import argparse
from pathlib import Path
import subprocess
import zipfile


def check(apk, sdk):
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        assert not any(n.endswith(('.xbe', '.iso', '.cpp', '.o', '/libscda_code.so')) for n in names), 'Game content in APK'
        for name in ('libprobe_clang.so', 'libprobe_lld.so', 'libprobe_recomp.so', 'libprobe_load.so',
                     'libscda_xiso.so', 'libscda_archive.so', 'libc++_compil.so', 'libscda_host.so',
                     'libSDL3.so', 'libmain.so', 'libc++_shared.so'):
            data = archive.read('lib/arm64-v8a/' + name)
            assert data[:6] == b'\x7fELF\x02\x01' and data[18:20] == b'\xb7\x00', name
        assert len(archive.read('assets/compiler/version')) == 64
        for license in ('LLVM.txt', 'LGPL-2.1-only.txt', 'GPL-3.0-only.txt', 'GPL-2.0-only.txt', 'NCSA.txt',
                        'TOOLCHAIN-NOTICES.md', 'extract-xiso.txt', 'libarchive.txt', 'Zydis.txt', 'Zycore.txt', 'tomlplusplus.txt'):
            assert archive.read('assets/licenses/' + license), license
        dex = b''.join(archive.read(n) for n in names if n.endswith('.dex'))
        assert b'test_import' not in dex and b'LocalSetupActivity' not in dex, 'Private test hook in production'
    aapt = sdk / 'build-tools/36.0.0/aapt.exe'
    metadata = subprocess.run([str(aapt), 'dump', 'badging', str(apk)], check=True, capture_output=True, text=True).stdout
    assert "package: name='io.github.mogeldev.scda'" in metadata
    assert "launchable-activity: name='io.github.mogeldev.scda.OnDeviceSetupActivity'" in metadata
    print(f'PASS: production identity/launcher, ARM64 tools/runtime, notices, no game content/debug import hook ({apk.stat().st_size / 1048576:.1f} MiB)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--sdk', required=True, type=Path)
    args = parser.parse_args()
    check(args.apk, args.sdk)
