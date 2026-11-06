import hashlib
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import run


class TransferTest(unittest.TestCase):
    def device(self, corrupt=False, delay=False):
        self.output = bytearray()
        self.part = b''
        self.appended = 0
        self.delayed = False

        def device(*args, **kwargs):
            text = ''
            if args[0] == 'exec-in':
                with tarfile.open(fileobj=io.BytesIO(kwargs['input'])) as archive:
                    self.assertEqual(archive.getnames(), ['zip.part'])
                    self.part = archive.extractfile('zip.part').read()
            elif 'sha256sum files/zip.part' in args[-1]:
                if corrupt:
                    text = '0' * 64 + '  files/zip.part'
                elif delay and not self.delayed:
                    self.delayed = True
                else:
                    text = hashlib.sha256(self.part).hexdigest() + '  files/zip.part'
            elif 'cat files/zip.part >>' in args[-1]:
                self.output.extend(self.part)
                self.appended += 1
            elif 'sha256sum files/test-assets.zip' in args[-1]:
                text = hashlib.sha256(self.output).hexdigest() + '  files/test-assets.zip'
            return subprocess.CompletedProcess(args, 0, text, '')
        return device

    def test_chunked_transfer_and_delayed_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'private.zip'
            contents = bytes(range(256)) * 3
            path.write_bytes(contents)
            with patch.object(run.time, 'sleep'):
                run.transfer_zip(self.device(delay=True), path, chunk_bytes=300)
            self.assertEqual(self.output, contents)
            self.assertEqual(self.appended, 3)

    def test_bad_chunk_never_appended(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'private.zip'
            path.write_bytes(b'private test input')
            with patch.object(run.time, 'sleep'), self.assertRaisesRegex(SystemExit, 'chunk mismatch'):
                run.transfer_zip(self.device(corrupt=True), path, chunk_bytes=8)
            self.assertEqual(self.appended, 0)
            self.assertEqual(self.output, b'')


if __name__ == '__main__': unittest.main()
