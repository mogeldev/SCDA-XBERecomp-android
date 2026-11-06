import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import run


def elf():
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    data[16:20] = b'\x03\x00\xb7\x00'
    return bytes(data)


class GameplayTest(unittest.TestCase):
    def test_elf_boundary(self):
        self.assertTrue(run.arm64_shared(elf()))
        for data in (b'', elf()[:20], b'not elf' + bytes(64), elf()[:16] + bytes(48)):
            self.assertFalse(run.arm64_shared(data))

    def test_only_matching_runtime_staged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            apk = root / 'host.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                for name in run.LIBRARIES:
                    archive.writestr('lib/arm64-v8a/' + name, elf())
                archive.writestr('lib/arm64-v8a/libscda_code.so', b'must not be packaged')
            with patch.object(run, 'WORK', root / 'work'):
                run.stage_runtime(apk)
                self.assertEqual({p.name for p in (run.WORK / 'jniLibs/arm64-v8a').iterdir()}, set(run.LIBRARIES))

    def test_only_campaign_and_phone_code_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = root / 'phone.so'
            code.write_bytes(elf())
            assets = root / 'assets'
            for name in ('Default/test.bin', 'Media/test.bin', 'adv/not-campaign.bin', 'sc4_adv.xbe'):
                path = assets / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'private fixture')
            xbe = assets / 'default.xbe'
            xbe.write_bytes(b'bad xbe')
            with self.assertRaisesRegex(ValueError, 'Unsupported default.xbe'):
                run.game_inputs(assets, code)
            with patch.object(hashlib, 'sha1') as sha1:
                sha1.return_value.hexdigest.return_value = '91290baf93c2f6ae735a3106c48734665c826d84'
                files = run.game_inputs(assets, code)
                self.assertEqual({name for _, name in files}, {'libscda_code.so', 'assets/default.xbe',
                                  'assets/Default/test.bin', 'assets/Media/test.bin'})
                (assets / 'Media/test.bin').unlink()
                with self.assertRaisesRegex(ValueError, 'Missing campaign assets'):
                    run.game_inputs(assets, code)

    def test_isolated_package_and_shared_production_sources(self):
        text = (run.HERE / 'build.gradle').read_text()
        self.assertIn("applicationId = '" + run.PACKAGE + "'", text)
        self.assertNotEqual(run.PACKAGE, 'io.github.mogeldev.scda')
        self.assertIn('../../android/src/main/java', text)
        self.assertIn('../../android/src/main/AndroidManifest.xml', text)


if __name__ == '__main__':
    unittest.main()
