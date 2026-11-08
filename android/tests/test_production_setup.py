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

    def test_experimental_android10_wiring(self):
        self.assertIn('minSdk = 29', (ROOT / 'android/build.gradle').read_text())
        setup = (JAVA / 'SetupActivity.java').read_text()
        self.assertIn('Build.VERSION.SDK_INT >= 30', setup)
        self.assertIn('setup.setFitsSystemWindows(true)', setup)
        self.assertIn('ANDROID 10 — EXPERIMENTAL', (JAVA / 'SetupView.java').read_text(encoding='utf-8'))
        local = (JAVA / 'OnDeviceSetupActivity.java').read_text()
        self.assertIn('aarch64-linux-android29', local)
        self.assertIn('copyAssets("compiler/libunwind.a"', local)
        self.assertIn('"@objects.rsp", "libunwind.a"', local)
        for file in ('tools/on-device-setup/build.py', 'tools/on-device-setup/import_tools.py'):
            self.assertIn('-DANDROID_PLATFORM=android-29', (ROOT / file).read_text())
        builder = (ROOT / 'tools/android-builder/build.ps1').read_text()
        self.assertIn('aarch64-linux-android29', builder)
        self.assertIn('x86_64-linux-android29', builder)
        self.assertIn('@$out.rsp ../compiler/$abi/libunwind.a', builder)
        self.assertIn('compiler/$abi/libunwind.a', (ROOT / 'tools/android-builder/package.ps1').read_text())

    def test_release_abis_and_universal_builder(self):
        gradle = (ROOT / 'android/build.gradle').read_text()
        self.assertIn("['arm64-v8a', 'x86_64']", gradle)
        package = (ROOT / 'tools/android-builder/package-game.ps1').read_text()
        self.assertIn("foreach ($abi in 'arm64-v8a', 'x86_64')", package)
        self.assertIn('"lib/$abi/libscda_code.so"', package)
        self.assertIn('Path.Combine(tools, "lib")', (ROOT / 'tools/android-builder/Builder.cs').read_text())

    def test_debug_only_signing_for_both_variants(self):
        gradle = (ROOT / 'android/build.gradle').read_text()
        self.assertIn('debug { signingConfig = signingConfigs.debug }', gradle)
        self.assertIn('release { signingConfig = signingConfigs.debug }', gradle)
        self.assertEqual(gradle.count('signingConfig ='), 2)

    def test_debug_import_hook_not_in_production(self):
        for path in JAVA.glob('*.java'):
            self.assertNotIn('test_import', path.read_text(encoding='utf-8'), str(path))
        test = ROOT / 'tools/on-device-setup/src/main/java/io/github/mogeldev/scda/LocalSetupActivity.java'
        self.assertIn('extends OnDeviceSetupActivity', test.read_text())
        self.assertIn('test_import', test.read_text())

    def test_legacy_installs_and_prebuilt_fallback_retained(self):
        setup = (JAVA / 'SetupActivity.java').read_text()
        self.assertIn('boolean installed = files != null && ready(files)', setup)
        self.assertIn('if (installed) setup.showReady()', setup)
        self.assertIn('GameImport.isInstalled(files)', setup)
        self.assertIn('new File(files, "assets")', setup)
        alias = ET.parse(ROOT / 'android/src/main/AndroidManifest.xml').find('.//activity-alias')
        self.assertEqual(alias.get(ANDROID + 'name'), 'io.github.mogeldev.scda.SetupActivity')
        self.assertEqual(alias.get(ANDROID + 'targetActivity'), 'io.github.mogeldev.scda.OnDeviceSetupActivity')
        local = (JAVA / 'OnDeviceSetupActivity.java').read_text()
        self.assertIn('super.importSource(uri, external, machine, progress)', local)
        self.assertIn('Build.SUPPORTED_ABIS[0].equals("arm64-v8a")', local)
        self.assertIn('CompileCache.hash(nativeDir.resolve(name))', local)

    def test_setup_layout_constraints(self):
        view = (JAVA / 'SetupView.java').read_text(encoding='utf-8')
        self.assertIn('available * (fontScale > 1.2f ? 0.94f : 0.8f)', view)
        self.assertIn('extends LinearLayout', view)
        self.assertIn('new ScrollView', view)
        self.assertIn('SCDA - Android', view)
        self.assertIn('boolean back()', view)
        self.assertIn('void browse(', view)
        self.assertIn('void driverPage(', view)
        self.assertIn('fontScale', view)
        self.assertIn('state_focused', view)
        self.assertIn('setMinHeight(dp(48))', view)
        self.assertIn('ACCESSIBILITY_LIVE_REGION_POLITE', view)
        self.assertIn('workProgress', view)
        self.assertIn('percent.setText(value + "%")', view)
        self.assertIn('showWorkPage', view)
        setup = (JAVA / 'SetupActivity.java').read_text(encoding='utf-8')
        self.assertIn('WindowInsets.Type.displayCutout()', setup)
        self.assertIn('this::showRequirements', setup)
        self.assertIn('worker.interrupt()', setup)
        self.assertIn('updatePercent', (JAVA / 'GameImport.java').read_text(encoding='utf-8'))
        self.assertIn('done * 100 / 129', (JAVA / 'OnDeviceSetupActivity.java').read_text(encoding='utf-8'))

    def test_setup_starts_in_fixed_landscape(self):
        for path, activity in [
            ('android/src/main/AndroidManifest.xml', 'OnDeviceSetupActivity'),
            ('tools/on-device-setup/src/debug/AndroidManifest.xml', 'LocalSetupActivity'),
            ('tools/on-device-setup-ui/src/main/AndroidManifest.xml', 'SetupPreviewActivity'),
        ]:
            nodes = ET.parse(ROOT / path).findall('.//activity')
            node = next(n for n in nodes if n.get(ANDROID + 'name') == 'io.github.mogeldev.scda.' + activity)
            self.assertEqual(node.get(ANDROID + 'screenOrientation'), 'landscape', path)

    def test_policy_matches_probe(self):
        probe = ROOT / 'tools/on-device-probe/src/main/java/io/github/mogeldev/scda/compilerprobe/BuildJobs.java'
        expected = probe.read_text().replace('package io.github.mogeldev.scda.compilerprobe;', 'package io.github.mogeldev.scda;')
        self.assertEqual((JAVA / 'BuildJobs.java').read_text(), expected)


if __name__ == '__main__': unittest.main()
