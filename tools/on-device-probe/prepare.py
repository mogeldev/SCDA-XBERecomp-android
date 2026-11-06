"""Stage private compiler-probe inputs. No game sources are put into the APK."""
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = ROOT / 'out/on-device-probe'
BASE = 'https://packages.termux.dev/apt/termux-main/'


def unpack_deb(data):
    if data[:8] != b'!<arch>\n':
        raise ValueError('Not a Debian archive')
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        size = int(header[48:58])
        name = header[:16].decode().strip().rstrip('/')
        payload = data[offset + 60:offset + 60 + size]
        if name.startswith('data.tar'):
            return tarfile.open(fileobj=io.BytesIO(payload), mode='r:*')
        offset += 60 + size + size % 2
    raise ValueError('Missing data archive')


def fetch(package):
    archive = WORK / Path(package['path']).name
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != package['sha256']:
        data = urllib.request.urlopen(BASE + package['path'], timeout=120).read()
        if hashlib.sha256(data).hexdigest() != package['sha256']:
            raise ValueError(f'Checksum mismatch: {archive.name}')
        archive.write_bytes(data)
    return archive


# APK native libraries must end in .so. Keep replacements no longer than originals.
RENAMES = {'libz.so.1': 'libtz.so', 'libzstd.so.1': 'libzstd.so', 'libxml2.so.16': 'libxml2.so'}
KEEP = {'libLLVM.so', 'libclang-cpp.so', 'libc++_shared.so', 'libffi.so',
        'libiconv.so', *RENAMES.values()}


def relocate_elf(data):
    """Relocate only ELF64 dynamic strings, without changing section offsets."""
    data = bytearray(data)
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phnum)]
    dynamic = next(p for p in segments if p[0] == 2)
    entries = [struct.unpack_from('<qQ', data, offset)
               for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16)]
    straddr = next(value for tag, value in entries if tag == 5)
    load = next(p for p in segments if p[0] == 1 and p[3] <= straddr < p[3] + p[5])
    strbase = load[2] + straddr - load[3]
    for tag, value in entries:
        if tag not in (1, 14, 29):  # NEEDED, SONAME, RUNPATH
            continue
        start = strbase + value
        end = data.index(0, start)
        old = data[start:end].decode()
        new = '$ORIGIN' if tag == 29 else RENAMES.get(old, old)
        if len(new) > len(old):
            raise ValueError(f'Replacement too long: {old}')
        data[start:end] = new.encode().ljust(end - start, b'\0')
    return bytes(data)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    packages = json.loads((HERE / 'packages.json').read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(fetch, packages))
    dest = WORK / 'jniLibs/x86_64'
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    # Only real ELF files; links and package scripts are never executed/extracted.
    for archive in archives:
        with unpack_deb(archive.read_bytes()) as tar:
            for member in tar:
                if not member.isfile():
                    continue
                name = Path(member.name).name
                output = {'clang-21': 'libprobe_clang.so', 'lld': 'libprobe_lld.so'}.get(name)
                if '/usr/lib/' in member.name and '.so' in name:
                    # Package real versioned files under the relocated SONAME.
                    name = next((new for old, new in RENAMES.items() if name.startswith(old)), name)
                    if name in KEEP:
                        output = name
                if output:
                    data = tar.extractfile(member).read()
                    if data[:4] == b'\x7fELF':
                        (dest / output).write_bytes(relocate_elf(data))
    missing = (KEEP | {'libprobe_clang.so', 'libprobe_lld.so'}) - {p.name for p in dest.iterdir()}
    if missing:
        raise ValueError(f'Missing compiler libraries: {sorted(missing)}')
    # Copy private input only into the work directory; adb sends it separately.
    source = ROOT / 'generated/game/x86_recomp.0.cpp'
    inputs = WORK / 'input'
    inputs.mkdir(exist_ok=True)
    shutil.copy2(source, inputs / source.name)
    shutil.copy2(ROOT / 'generated/game/x86_recomp_shared.h', inputs)
    shutil.copytree(ROOT / 'src/cpu', inputs / 'cpu', dirs_exist_ok=True)
    print(f'Compiler files: {dest}\nPrivate test input: {inputs}')


if __name__ == '__main__':
    main()
