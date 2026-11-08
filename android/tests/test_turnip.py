"""Pinned staging fails closed before replacing an installed build input."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('turnip_stage', ROOT / 'tools/turnip/stage.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

class TurnipStageTest(unittest.TestCase):
    def test_stage_and_reject_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / 'out/turnip/Turnip-710-720-722-v4.5.zip'
            archive.parent.mkdir(parents=True)
            lib = bytearray(64)
            lib[:6] = b'\x7fELF\x02\x01'
            lib[18:20] = b'\xb7\x00'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('libvulkan_freedreno.so', lib)
                z.writestr('../../escape', b'must not extract')
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            with patch.object(stage, 'ROOT', root), patch.object(stage, 'SHA256', digest):
                stage.stage()
                target = root / 'out/turnip/jniLibs/arm64-v8a/libvulkan_freedreno.so'
                self.assertEqual(target.read_bytes(), lib)
                self.assertFalse((root / 'escape').exists())
                archive.write_bytes(b'bad download')
                with self.assertRaisesRegex(RuntimeError, 'checksum'):
                    stage.stage()
                self.assertEqual(target.read_bytes(), lib)

    def test_wrong_abi_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / 'out/turnip/Turnip-710-720-722-v4.5.zip'
            archive.parent.mkdir(parents=True)
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('libvulkan_freedreno.so', b'not ARM64')
            with patch.object(stage, 'ROOT', root), patch.object(stage, 'SHA256', hashlib.sha256(archive.read_bytes()).hexdigest()):
                with self.assertRaisesRegex(RuntimeError, 'ELF64 ARM64'):
                    stage.stage()

if __name__ == '__main__':
    unittest.main()
