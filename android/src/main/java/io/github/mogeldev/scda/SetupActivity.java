package io.github.mogeldev.scda;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

// Setup is separate from SDL so native game threads never start during import or the document picker.
public class SetupActivity extends Activity {
    private static final int PICK_ZIP = 1;
    private TextView status;
    private Button select;
    private ProgressBar progress;
    private Thread worker;
    private boolean picking;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        File files = getExternalFilesDir(null);
        if (files != null && ready(files)) {
            launchGame();
            return;
        }
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        int padding = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView title = new TextView(this);
        title.setText("Splinter Cell: Double Agent — Setup");
        title.setTextSize(24);
        layout.addView(title);
        TextView instructions = new TextView(this);
        instructions.setText("Import a ZIP made from your own Xbox game and matching player-builder output.\n\n"
            + "ZIP root: libscda_code.so and assets/\n"
            + "Inside assets/: default.xbe, Default/ and Media/\n\n"
            + "Keep this screen open during import. You need enough free space for the extracted game. "
            + "Saves and settings will not be changed. Gameplay requires a controller.");
        instructions.setTextSize(16);
        instructions.setPadding(0, padding, 0, padding);
        layout.addView(instructions);
        select = new Button(this);
        select.setText("Import game ZIP");
        select.setOnClickListener(view -> pickZip());
        layout.addView(select);
        progress = new ProgressBar(this);
        progress.setVisibility(android.view.View.GONE);
        layout.addView(progress);
        status = new TextView(this);
        status.setTextSize(16);
        status.setPadding(0, padding, 0, 0);
        layout.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(layout);
        setContentView(scroll);
        picking = state != null && state.getBoolean("picking");
        select.setEnabled(!picking && files != null);
        if (files == null) status.setText("App storage is unavailable. Check storage and restart SCDA.");
        else if (state != null && state.getBoolean("importing"))
            status.setText("The previous import was interrupted. Select the ZIP again to retry.");
        select.requestFocus();
    }

    private boolean ready(File files) {
        if (GameImport.isInstalled(files)) return true;
        // Keep existing USB/ADB installs and developer APKs working without an import marker.
        return GameImport.hasAssets(new File(files, "assets"))
            && (new File(files, "libscda_code.so").isFile()
                || new File(getFilesDir(), "libscda_code.so").isFile()
                || new File(getApplicationInfo().nativeLibraryDir, "libscda_code.so").isFile());
    }

    private void pickZip() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // Providers sometimes label ZIPs as application/octet-stream. The importer validates the contents.
        intent.setType("*/*");
        try {
            picking = true;
            select.setEnabled(false);
            startActivityForResult(intent, PICK_ZIP);
        } catch (ActivityNotFoundException e) {
            picking = false;
            select.setEnabled(true);
            status.setText("No system file picker is available on this device.");
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ZIP) return;
        picking = false;
        select.setEnabled(true);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            status.setText("No ZIP selected. You can try again.");
            return;
        }
        importZip(data.getData());
    }

    private void importZip(Uri uri) {
        select.setEnabled(false);
        progress.setVisibility(android.view.View.VISIBLE);
        status.setText("Opening ZIP…");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker = new Thread(() -> {
            String error = null;
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("Cannot open the selected ZIP.");
                File files = getExternalFilesDir(null);
                if (files == null) throw new IOException("App storage is unavailable.");
                // The setup and native runtime execute in the same ABI/process.
                int machine = android.os.Build.SUPPORTED_ABIS[0].equals("x86_64") ? 62 : 183;
                long[] lastUpdate = {0};
                GameImport.extract(input, files, machine, (name, bytes) -> {
                    long now = android.os.SystemClock.elapsedRealtime();
                    if (now - lastUpdate[0] < 250) return;
                    lastUpdate[0] = now;
                    String text = String.format(Locale.ROOT, "Extracted %.1f MiB\n%s", bytes / 1048576.0, name);
                    runOnUiThread(() -> {
                        if (!isDestroyed()) status.setText(text);
                    });
                });
            } catch (IOException | RuntimeException e) {
                error = e.getMessage() != null ? e.getMessage() : e.toString();
                android.util.Log.e("SCDAImport", "Import failed", e);
            }
            final String failure = error;
            runOnUiThread(() -> {
                worker = null;
                if (isDestroyed() || isFinishing()) return;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                progress.setVisibility(android.view.View.GONE);
                if (failure == null) launchGame();
                else {
                    status.setText("Import failed: " + failure + "\n\nYour saves and settings were not changed. Select a ZIP to retry.");
                    select.setEnabled(true);
                    select.requestFocus();
                }
            });
        }, "SCDA ZIP import");
        worker.start();
    }

    private void launchGame() {
        startActivity(new Intent(this, SDLActivity.class));
        finish();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("picking", picking);
        state.putBoolean("importing", worker != null);
        super.onSaveInstanceState(state);
    }

    @Override public void onBackPressed() {
        if (worker != null) {
            worker.interrupt();
            status.setText("Cancelling import…");
        } else super.onBackPressed();
    }

    @Override protected void onDestroy() {
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }
}
