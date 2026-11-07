import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('harden', Path(__file__).with_name('harden.py'))
harden = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harden)


class HardeningTest(unittest.TestCase):
    def test_refuses_unexpected_upstream(self):
        with self.assertRaisesRegex(ValueError, 'Pinned extract-xiso source changed'):
            harden.harden('not the pinned source')

    def test_all_guards_applied(self):
        source = Path(__file__).resolve().parents[3] / 'out/extract-xiso-source/extract-xiso.c'
        if not source.exists(): self.skipTest('Pinned upstream source not downloaded')
        result = harden.harden(source.read_text())
        for guard in ('scda_entry(in_xiso)', 'scda_name(dir->filename', 'scda_extent(in_xiso',
                      'scda_space(read_size)', 'O_EXCL | O_NOFOLLOW', '--scda_depth',
                      'scda_fail("Truncated ISO file")', 'int upstream_main('):
            self.assertIn(guard, result)


if __name__ == '__main__': unittest.main()
