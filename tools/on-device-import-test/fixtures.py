"""Generate ignored private fixtures; only default.xbe is real, media/data are synthetic."""
import argparse
import binascii
import io
from pathlib import Path
import struct
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'out/on-device-import/fixtures'


def xiso(files, output, offset=0):
    sectors = [b'\0' * 2048 for _ in range(33)]
    tree = {}
    for name, data in files.items():
        node = tree
        parts = name.split('/')
        for part in parts[:-1]: node = node.setdefault(part, {})
        node[parts[-1]] = data
    def write(node):
        sector = len(sectors)
        sectors.append(b'\0' * 2048)
        records = []
        for name, data in sorted(node.items()):
            if isinstance(data, dict):
                start, size = write(data), 2048
                attr = 0x10
            else:
                start, size, attr = len(sectors), len(data), 0
                padded = data + b'\0' * (-len(data) % 2048)
                sectors.extend(padded[i:i+2048] for i in range(0, len(padded), 2048))
            encoded = name.encode()
            record = struct.pack('<HHIIBB', 0, 0, start, size, attr, len(encoded)) + encoded
            records.append(bytearray(record + b'\0' * (-len(record) % 4)))
        table = bytearray()
        for i, record in enumerate(records):
            if i + 1 < len(records): struct.pack_into('<H', record, 2, (len(table) + len(record)) // 4)
            table.extend(record)
        assert len(table) <= 2048
        sectors[sector] = bytes(table) + b'\0' * (2048 - len(table))
        return sector
    root = write(tree)
    header = bytearray(2048)
    header[:20] = b'MICROSOFT*XBOX*MEDIA'
    struct.pack_into('<II', header, 20, root, 2048)
    header[2028:] = b'MICROSOFT*XBOX*MEDIA'
    sectors[32] = bytes(header)
    with output.open('wb') as stream:
        stream.seek(offset)
        for sector in sectors: stream.write(sector)


def rar4(files, output):
    def block(kind, flags, payload):
        body = struct.pack('<BHH', kind, flags, 7 + len(payload)) + payload
        return struct.pack('<H', binascii.crc32(body) & 65535) + body
    with output.open('wb') as stream:
        stream.write(b'Rar!\x1a\x07\x00')
        stream.write(block(0x73, 0, b'\0' * 6))
        for name, data in files.items():
            encoded = name.encode()
            payload = struct.pack('<IIBIIBBHI', len(data), len(data), 3, binascii.crc32(data), 0, 20, 0x30, len(encoded), 0o100644) + encoded
            stream.write(block(0x74, 0x8000, payload))
            stream.write(data)
        stream.write(block(0x7b, 0, b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xbe', type=Path, default=ROOT / 'assets/default.xbe')
    parser.add_argument('--sevenzip', type=Path, default=Path('C:/Program Files/7-Zip/7z.exe'))
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    for path in WORK.glob('unsafe-*.zip'): path.unlink()
    files = {'default.xbe': args.xbe.read_bytes(), 'Default/data.bin': b'synthetic data', 'Media/intro.bin': b'synthetic media'}
    xiso(files, WORK / 'tiny.xiso.iso')
    xiso(files, WORK / 'tiny-retail.iso', 0x18300000)
    wrapped = {'wrapper/' + n: d for n, d in files.items()}
    wrapped['wrapper/readme.txt'] = b'ignored'
    with zipfile.ZipFile(WORK / 'folder.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in wrapped.items(): archive.writestr(name, data)
    for suffix, mode in [('tar', 'w'), ('tar.gz', 'w:gz'), ('tar.bz2', 'w:bz2'), ('tar.xz', 'w:xz')]:
        with tarfile.open(WORK / ('folder.' + suffix), mode) as archive:
            for name, data in wrapped.items():
                entry = tarfile.TarInfo('./' + name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
    rar4(wrapped, WORK / 'folder.rar')
    unpacked = WORK / 'source'
    for name, data in wrapped.items():
        p = unpacked / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    subprocess.run([str(args.sevenzip), 'a', '-t7z', str(WORK / 'folder.7z'), str(unpacked / '*')], check=True)
    with zipfile.ZipFile(WORK / 'image.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(WORK / 'tiny.xiso.iso', 'nested/game.iso')
        archive.writestr('readme.txt', 'ignored')
    for name in ('../escape', '/escape', 'Default/../../escape', 'a\\escape', 'a:escape'):
        with zipfile.ZipFile(WORK / ('unsafe-' + str(len(list(WORK.glob('unsafe-*')))) + '.zip'), 'w') as archive:
            archive.writestr(name, b'bad')
    with tarfile.open(WORK / 'symlink.tar', 'w') as archive:
        entry = tarfile.TarInfo('link'); entry.type = tarfile.SYMTYPE; entry.linkname = '../outside'
        archive.addfile(entry)
    with zipfile.ZipFile(WORK / 'case.zip', 'w') as archive:
        archive.writestr('Default/file', 'one'); archive.writestr('Default/FILE', 'two')
    (WORK / 'invalid.iso').write_bytes(b'not an iso')
    image = (WORK / 'tiny.xiso.iso').read_bytes()
    root_sector = struct.unpack_from('<I', image, 0x10000 + 20)[0]
    for name, change in [('path', lambda data: data.__setitem__(root_sector * 2048 + 14, ord('/'))),
                         ('nul', lambda data: data.__setitem__(root_sector * 2048 + 14, 0))]:
        data = bytearray(image); change(data)
        (WORK / ('bad-' + name + '.iso')).write_bytes(data)
    (WORK / 'bad-truncated.iso').write_bytes(image[:-2048])
    with zipfile.ZipFile(WORK / 'ambiguous.zip', 'w') as archive:
        for prefix in ('one', 'two'):
            for name, data in files.items(): archive.writestr(prefix + '/' + name, data)
    with zipfile.ZipFile(WORK / 'bad-xbe.zip', 'w') as archive:
        for name, data in files.items(): archive.writestr(name, b'XBEHunsupported' if name == 'default.xbe' else data)
    subprocess.run([str(args.sevenzip), 'a', '-t7z', '-pfixture-password', '-mhe=on',
                    str(WORK / 'encrypted.7z'), str(unpacked / '*')], check=True)
    print('Private fixtures:', WORK)


if __name__ == '__main__': main()
