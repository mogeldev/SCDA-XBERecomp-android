package io.github.mogeldev.scda;

import java.io.File;
import java.io.IOException;

// Only the separate test package exposes the private ADB import entry point.
public final class LocalSetupActivity extends OnDeviceSetupActivity {
    @Override protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        File external = getExternalFilesDir(null);
        File input = new File(getFilesDir(), "test-assets.zip");
        if (state == null && external != null && !new File(external, "game").exists()
                && getIntent().getBooleanExtra("test_import", false) && input.isFile()) {
            importZip(android.net.Uri.fromFile(input));
        }
    }

    @Override protected void compileGame(File external, GameImport.Progress progress) throws IOException {
        java.nio.file.Files.deleteIfExists(new File(getFilesDir(), "test-assets.zip").toPath());
        super.compileGame(external, progress);
    }
}
