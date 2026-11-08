"""Build pinned, installed native import tools for the requested Android ABI."""
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def build(sdk, abi):
    work = ROOT / 'out/on-device-import/native' / abi
    cmake = sdk / 'cmake/3.31.6/bin/cmake.exe'
    subprocess.run([str(cmake), '-S', str(HERE / 'extract-xiso'), '-B', str(work), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_MAKE_PROGRAM={sdk}/cmake/3.31.6/bin/ninja.exe',
        f'-DCMAKE_TOOLCHAIN_FILE={sdk}/ndk/29.0.14206865/build/cmake/android.toolchain.cmake',
        '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=none'], check=True)
    subprocess.run([str(cmake), '--build', str(work), '--target', 'extract_xiso', 'game_archive', '--parallel', '6'], check=True)
    return work


def stage(sdk, abi, native, assets):
    work = build(sdk, abi)
    native.mkdir(parents=True, exist_ok=True)
    for name in ('libscda_xiso.so', 'libscda_archive.so'):
        shutil.copy2(work / name, native / name)
    licenses = assets / 'licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    for name in ('extract-xiso', 'libarchive', 'xz', 'xz-0BSD', 'bzip2'):
        shutil.copy2(work / (name + '.txt'), licenses / (name + '.txt'))
    # COPYING summarizes per-file notices; retain those notices too for the linked readers.
    cache = (work / 'CMakeCache.txt').read_text()
    source = next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('libarchive_SOURCE_DIR:'))
    notices = []
    for path in sorted((Path(source) / 'libarchive').glob('*.[ch]')):
        text = path.read_text(errors='replace')
        start = text.find('/*')
        end = text.find('*/', start)
        if start >= 0 and end >= 0:
            notices.append(path.name + '\n' + text[start:end + 2])
    (licenses / 'libarchive-source-notices.txt').write_text('\n\n'.join(notices))
