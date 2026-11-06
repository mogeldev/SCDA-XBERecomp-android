package io.github.mogeldev.scda;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;

// Emulator-only harness for the same import pipeline. Never packages game content.
public final class ImportTestActivity extends Activity {
    private Thread worker;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText("SCDA import test running; see SCDAImportTest logcat");
        setContentView(view);
        worker = new Thread(() -> {
            Path target = getFilesDir().toPath().resolve("result");
            Path source = getFilesDir().toPath().resolve("source.input");
            try {
                CompileCache.delete(target);
                try (InputStream input = Files.newInputStream(source)) {
                    GameSource.importFile(input, target.toFile(), 62,
                        (tool, in, out, progress) -> ExtractTools.extract(this, tool, in, out, progress),
                        (name, bytes) -> {
                            if (bytes < 0) Log.i("SCDAImportTest", name);
                            if (getIntent().getBooleanExtra("cancel", false) && name.startsWith("Extracting"))
                                Thread.currentThread().interrupt();
                        });
                }
                Log.i("SCDAImportTest", "IMPORT_PASS assets=" + GameImport.hasAssets(target.resolve("game/assets").toFile()));
            } catch (Exception e) {
                Log.e("SCDAImportTest", "IMPORT_FAIL published=" + Files.exists(target.resolve("game"))
                    + " staging=" + Files.exists(target.resolve(".game-import")), e);
            }
        });
        worker.start();
    }
    @Override public void onBackPressed() { if (worker != null) worker.interrupt(); }
    @Override public void onDestroy() { if (worker != null) worker.interrupt(); super.onDestroy(); }
}
