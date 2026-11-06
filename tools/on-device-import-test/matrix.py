"""Run generated format/security fixtures through the emulator app."""
import argparse
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    fixtures = ROOT / 'out/on-device-import/fixtures'
    names = ['tiny.xiso.iso', 'tiny-retail.iso', 'folder.zip', 'folder.7z', 'folder.rar', 'folder.tar',
             'folder.tar.gz', 'folder.tar.bz2', 'folder.tar.xz', 'image.zip', 'invalid.iso', 'symlink.tar',
             'case.zip', 'bad-path.iso', 'bad-nul.iso', 'bad-truncated.iso', 'ambiguous.zip',
             'bad-xbe.zip', 'encrypted.7z', *[p.name for p in sorted(fixtures.glob('unsafe-*.zip'))]]
    for name in names:
        print('\nTEST', name, flush=True)
        command = [sys.executable, str(HERE / 'run.py'), '--sdk', args.sdk, '--serial', args.serial,
                   '--source', str(fixtures / name)]
        if name.startswith(('unsafe', 'invalid', 'symlink', 'case', 'bad-', 'ambiguous', 'encrypted')): command.append('--expect-failure')
        subprocess.run(command, check=True)
    subprocess.run([sys.executable, str(HERE / 'run.py'), '--sdk', args.sdk, '--serial', args.serial,
                   '--source', str(fixtures / 'tiny.xiso.iso'), '--cancel'], check=True)
    print(f'PASS: {len(names) + 1} emulator import cases', flush=True)


if __name__ == '__main__': main()
