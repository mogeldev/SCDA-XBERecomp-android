"""Guard the manual-only reporting boundary (no Android/device required)."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[2]

class DeviceReportTest(unittest.TestCase):
    def test_manual_share_only(self):
        s = (ROOT / 'android/src/main/java/io/github/mogeldev/scda/DeviceReport.java').read_text()
        for expected in ['Intent.ACTION_SEND', 'Intent.ACTION_VIEW', 'appendQueryParameter("body", report)',
                         'lastModified() >= updated', 'Nothing is sent automatically', 'setNegativeButton("Cancel"',
                         'Player-submitted report; not a compatibility certification']:
            self.assertIn(expected, s)
        for forbidden in ['HttpURLConnection', 'getSerial(', 'ANDROID_ID', 'getAccounts(', 'logcat', 'ACTION_SEND_MULTIPLE']:
            self.assertNotIn(forbidden, s)

    def test_startup_snapshot_is_minimal(self):
        s = (ROOT / 'src/android/vulkan.cpp').read_text()
        block = s[s.index('// Minimal local snapshot'):s.index('bool VulkanBackend::CreateSwapchain()')]
        for expected in ['GPU: ', 'Vulkan: ', 'Loaded driver: ', 'Driver version (raw): ']:
            self.assertIn(expected, block)
        self.assertNotIn('ExeDir()', block)
        self.assertIn('std::filesystem::rename', block)

if __name__ == '__main__': unittest.main()
