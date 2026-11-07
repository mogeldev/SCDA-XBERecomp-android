"""Production wiring regressions; no Android device or game content needed."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'android/src/main/java/io/github/mogeldev/scda'
ANDROID = '{http://schemas.android.com/apk/res/android}'


class ProductionSetupTest(unittest.TestCase):
    def test_production_launcher_and_identity(self):
        manifest = ET.parse(ROOT / 'android/src/main/AndroidManifest.xml')
        launchers = [a for a in manifest.findall('.//activity') if a.find('.//category') is not None]
        self.assertEqual([a.get(ANDROID + 'name') for a in launchers], ['io.github.mogeldev.scda.OnDeviceSetupActivity'])
        gradle = (ROOT / 'android/build.gradle').read_text()
        self.assertIn("applicationId = 'io.github.mogeldev.scda'", gradle)
        self.assertIn('useLegacyPackaging = true', gradle)
        self.assertIn("dependsOn(stageOnDeviceTools)", gradle)
        self.assertIn('../out/android-tools/jniLibs', gradle)

    def test_debug_import_hook_not_in_production(self):
        for path in JAVA.glob('*.java'):
            self.assertNotIn('test_import', path.read_text(), str(path))
        test = ROOT / 'tools/on-device-setup/src/main/java/io/github/mogeldev/scda/LocalSetupActivity.java'
        self.assertIn('extends OnDeviceSetupActivity', test.read_text())
        self.assertIn('test_import', test.read_text())

    def test_legacy_installs_and_prebuilt_fallback_retained(self):
        setup = (JAVA / 'SetupActivity.java').read_text()
        self.assertIn('if (files != null && ready(files))', setup)
        self.assertIn('GameImport.isInstalled(files)', setup)
        self.assertIn('new File(files, "assets")', setup)
        alias = ET.parse(ROOT / 'android/src/main/AndroidManifest.xml').find('.//activity-alias')
        self.assertEqual(alias.get(ANDROID + 'name'), 'io.github.mogeldev.scda.SetupActivity')
        self.assertEqual(alias.get(ANDROID + 'targetActivity'), 'io.github.mogeldev.scda.OnDeviceSetupActivity')
        local = (JAVA / 'OnDeviceSetupActivity.java').read_text()
        self.assertIn('super.importSource(uri, external, machine, progress)', local)
        self.assertIn('Build.SUPPORTED_ABIS[0].equals("arm64-v8a")', local)
        self.assertIn('CompileCache.hash(nativeDir.resolve(name))', local)

    def test_policy_matches_probe(self):
        probe = ROOT / 'tools/on-device-probe/src/main/java/io/github/mogeldev/scda/compilerprobe/BuildJobs.java'
        expected = probe.read_text().replace('package io.github.mogeldev.scda.compilerprobe;', 'package io.github.mogeldev.scda;')
        self.assertEqual((JAVA / 'BuildJobs.java').read_text(), expected)


if __name__ == '__main__': unittest.main()
