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
    private Button folder;
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
        instructions.setText(supportsLocalCompilation()
            ? "Import your own supported USA Xbox game: ISO/XISO, ZIP, 7z, RAR or TAR, "
                + "or select an extracted game folder containing default.xbe, Default/ and Media/.\n\n"
                + "Archives may contain one ISO or one extracted game folder. Encrypted and split archives are not supported.\n\n"
                + "Compilation runs on this device and may take several minutes. Keep this screen open. "
                + "Back cancels; completed compiler work can be resumed. A matching prebuilt-library ZIP also works. "
                + "Saves and settings will not be changed. Gameplay requires a controller."
            : "Import a ZIP made from your own Xbox game and matching player-builder output.\n\n"
            + "ZIP root: libscda_code.so and assets/\n"
            + "Inside assets/: default.xbe, Default/ and Media/\n\n"
            + "Keep this screen open during import. You need enough free space for the extracted game. "
            + "Saves and settings will not be changed. Gameplay requires a controller.");
        instructions.setTextSize(16);
        instructions.setPadding(0, padding, 0, padding);
        layout.addView(instructions);
        select = new Button(this);
        select.setText(pendingBuild(files) ? "Compile / resume" : importLabel());
        select.setOnClickListener(view -> {
            if (pendingBuild(getExternalFilesDir(null))) importZip(null);
            else pickZip();
        });
        layout.addView(select);
        if (supportsLocalCompilation()) {
            folder = new Button(this);
            folder.setText("Import extracted game folder");
            folder.setOnClickListener(view -> pickFolder());
            folder.setVisibility(pendingBuild(files) ? android.view.View.GONE : android.view.View.VISIBLE);
            layout.addView(folder);
            TextView licenses = new TextView(this);
            licenses.setText("ISO support: extract-xiso. This product includes software developed by in <in@fishtank.com>.\nOpen import-tool licenses");
            licenses.setPadding(0, padding / 2, 0, padding / 2);
            licenses.setFocusable(true);
            licenses.setOnClickListener(view -> showImportLicenses());
            layout.addView(licenses);
        }
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
        setSelectionEnabled(!picking && files != null);
        if (files == null) status.setText("App storage is unavailable. Check storage and restart SCDA.");
        else if (state != null && state.getBoolean("importing"))
            status.setText("The previous import was interrupted. Select the input again to retry.");
        select.requestFocus();
    }

    // Experimental setup opts in without changing release/Windows-builder import behavior.
    protected boolean supportsLocalCompilation() { return false; }
    protected void showImportLicenses() {}
    protected void compileGame(File files, GameImport.Progress progress) throws IOException {}
    protected void importSource(Uri uri, File files, int machine, GameImport.Progress update) throws IOException {
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("Cannot open the selected ZIP.");
            GameImport.extract(input, files, machine, supportsLocalCompilation(), update);
        }
    }

    private String importLabel() { return supportsLocalCompilation() ? "Import game file (ISO / archive)" : "Import game ZIP"; }
    private void setSelectionEnabled(boolean enabled) {
        select.setEnabled(enabled);
        if (folder != null) folder.setEnabled(enabled);
    }
    private void pickFolder() {
        try {
            picking = true;
            setSelectionEnabled(false);
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), 2);
        } catch (ActivityNotFoundException e) {
            picking = false;
            setSelectionEnabled(true);
            status.setText("No system folder picker is available on this device.");
        }
    }

    private boolean pendingBuild(File files) {
        return supportsLocalCompilation() && files != null
            && GameImport.hasAssets(new File(files, "game/assets")) && !GameImport.isInstalled(files);
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
            setSelectionEnabled(false);
            startActivityForResult(intent, PICK_ZIP);
        } catch (ActivityNotFoundException e) {
            picking = false;
            setSelectionEnabled(true);
            status.setText("No system file picker is available on this device.");
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ZIP && !(request == 2 && supportsLocalCompilation())) return;
        picking = false;
        setSelectionEnabled(true);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            status.setText("No input selected. You can try again.");
            return;
        }
        importZip(data.getData());
    }

    protected void importZip(Uri uri) {
        setSelectionEnabled(false);
        progress.setVisibility(android.view.View.VISIBLE);
        status.setText("Opening input…");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker = new Thread(() -> {
            String error = null;
            try {
                File files = getExternalFilesDir(null);
                if (files == null) throw new IOException("App storage is unavailable.");
                // The setup and native runtime execute in the same ABI/process.
                int machine = android.os.Build.SUPPORTED_ABIS[0].equals("x86_64") ? 62 : 183;
                long[] lastUpdate = {0};
                GameImport.Progress update = (name, bytes) -> {
                    long now = android.os.SystemClock.elapsedRealtime();
                    if (bytes >= 0 && now - lastUpdate[0] < 250) return;
                    lastUpdate[0] = now;
                    String text = bytes < 0 ? name : String.format(Locale.ROOT, "Extracted %.1f MiB\n%s", bytes / 1048576.0, name);
                    runOnUiThread(() -> {
                        if (!isDestroyed()) status.setText(text);
                    });
                };
                if (uri != null) importSource(uri, files, machine, update);
                if (pendingBuild(files)) compileGame(files, update);
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
                    status.setText("Setup stopped: " + failure + "\n\nYour saves and settings were not changed. Retry or resume below.");
                    boolean pending = pendingBuild(getExternalFilesDir(null));
                    select.setText(pending ? "Compile / resume" : importLabel());
                    if (folder != null) folder.setVisibility(pending ? android.view.View.GONE : android.view.View.VISIBLE);
                    setSelectionEnabled(true);
                    select.requestFocus();
                }
            });
        }, "SCDA game import");
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
            status.setText("Cancelling setup… Completed compiler work will be kept.");
        } else super.onBackPressed();
    }

    @Override protected void onDestroy() {
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }
}
