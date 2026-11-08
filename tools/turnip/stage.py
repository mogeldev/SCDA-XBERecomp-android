"""Stage the pinned, unmodified universal Turnip binary (never game content)."""
from pathlib import Path
import hashlib
import io
import urllib.request
import zipfile

URL = 'https://github.com/Vauzi-17/710/releases/download/4.5/Turnip-710-720-722-v4.5.zip'
SHA256 = 'b8a9884a5e3726f021f05b1e75f75ab206b37a98aefdebf922583f13e8957360'
ROOT = Path(__file__).resolve().parents[2]

def stage():
    archive = ROOT / 'out/turnip/Turnip-710-720-722-v4.5.zip'
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        data = urllib.request.urlopen(URL, timeout=120).read()
    else:
        data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError('Turnip download checksum mismatch')
    archive.write_bytes(data)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        lib = z.read('libvulkan_freedreno.so')
    if lib[:5] != b'\x7fELF\x02' or lib[18:20] != b'\xb7\x00':
        raise RuntimeError('Turnip is not ELF64 ARM64')
    target = ROOT / 'out/turnip/jniLibs/arm64-v8a/libvulkan_freedreno.so'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_bytes() != lib:
        target.write_bytes(lib)
    print('Staged pinned Turnip universal v4.5 (ARM64)')

if __name__ == '__main__':
    stage()
