from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import prepare
import run


class RuntimeInputTest(unittest.TestCase):
    def apk(self, root, host=None, include_sdl=True):
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        elf[16:20] = b'\x03\x00\xb7\x00'
        apk = root / 'game.apk'
        with zipfile.ZipFile(apk, 'w') as archive:
            archive.writestr('lib/arm64-v8a/libscda_host.so', host if host is not None else elf)
            if include_sdl:
                archive.writestr('lib/arm64-v8a/libSDL3.so', elf)
            archive.writestr('lib/arm64-v8a/libc++_shared.so', elf)
            archive.writestr('lib/arm64-v8a/libmain.so', b'not a probe input')
        return apk

    def test_stage_only_required_runtime_outside_apk_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(prepare, 'WORK', root / 'work'):
                run.stage_host(self.apk(root))
                self.assertEqual({p.relative_to(prepare.WORK).as_posix()
                                  for p in prepare.WORK.rglob('*') if p.is_file()}, {
                    'input/runtime/libscda_host.so', 'input/runtime/libSDL3.so',
                    'input/runtime/libc++_shared.so'})

    def test_reject_non_arm64_shared_host(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for data in (b'', b'not ELF', b'\x7fELF\x02\x01' + bytes(58)):
                with self.subTest(data=data), patch.object(prepare, 'WORK', root / 'work'):
                    with self.assertRaisesRegex(ValueError, 'invalid ARM64 shared library'):
                        run.stage_host(self.apk(root, host=data))

    def test_reject_incomplete_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(prepare, 'WORK', root / 'work'):
                with self.assertRaises(KeyError):
                    run.stage_host(self.apk(root, include_sdl=False))


if __name__ == '__main__':
    unittest.main()
