import json
import struct
import unittest

import prepare


class PrepareTest(unittest.TestCase):
    def test_invalid_archives(self):
        for data in (b'', b'!<arch>\n'):
            with self.assertRaises(ValueError):
                prepare.unpack_deb(data)

    def test_unpack_only_data_archive(self):
        payload = b'payload'
        # A well-formed ar header with an irrelevant member must not be accepted as data.tar.
        header = f'{"control.tar/":<16}{0:<12}{0:<6}{0:<6}{0:<8}{len(payload):<10}`\n'.encode()
        with self.assertRaises(ValueError):
            prepare.unpack_deb(b'!<arch>\n' + header + payload + b'\n')

    def test_relocate_dynamic_strings_preserves_layout(self):
        data = bytearray(512)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 2)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 4, 0, 0, 0, 512, 512, 8)
        struct.pack_into('<IIQQQQQQ', data, 120, 2, 4, 176, 176, 0, 80, 80, 8)
        strings = b'\0libz.so.1\0/data/data/com.termux/files/usr/lib\0'
        data[300:300 + len(strings)] = strings
        for offset, entry in zip(range(176, 256, 16), [(5, 300), (1, 1), (14, 1), (29, 11), (0, 0)]):
            struct.pack_into('<qQ', data, offset, *entry)
        relocated = prepare.relocate_elf(data)
        self.assertEqual(len(relocated), len(data))
        self.assertEqual(relocated[:300], data[:300])
        self.assertEqual(relocated[301:310], b'libtz.so\0')
        self.assertEqual(relocated[311:319], b'$ORIGIN\0')
        self.assertEqual(prepare.relocate_elf(relocated), relocated)

    def test_reject_non_elf64(self):
        with self.assertRaises(ValueError):
            prepare.relocate_elf(b'not an ELF')

    def test_pinned_packages_match_host_architecture(self):
        expected = {'clang', 'lld', 'libllvm', 'libc++', 'libffi', 'libiconv', 'libxml2', 'zlib', 'zstd'}
        for manifest, arch in [('packages.json', 'x86_64'), ('packages-arm64-v8a.json', 'aarch64')]:
            with self.subTest(architecture=arch):
                packages = json.loads((prepare.HERE / manifest).read_text())
                self.assertEqual({p['name'] for p in packages}, expected)
                self.assertEqual(len(packages), len(expected))
                for package in packages:
                    self.assertTrue(package['path'].endswith('_' + arch + '.deb'))
                    self.assertEqual(len(bytes.fromhex(package['sha256'])), 32)

    def test_reject_unsupported_abi_before_staging(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported ABI'):
            prepare.main('armeabi-v7a')


if __name__ == '__main__':
    unittest.main()
