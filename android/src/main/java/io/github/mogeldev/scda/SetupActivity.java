package io.github.mogeldev.scda;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ProgressBar;
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
    private SetupView setup;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        File files = getExternalFilesDir(null);
        if (files != null && ready(files)) {
            launchGame();
            return;
        }
        setup = new SetupView(this, supportsLocalCompilation(), this::showRequirements, this::showImportLicenses, this::cancelSetup);
        select = setup.select;
        folder = setup.folder;
        progress = setup.progress;
        status = setup.status;
        select.setText(pendingBuild(files) ? "Compile / resume" : importLabel());
        select.setOnClickListener(view -> {
            if (pendingBuild(getExternalFilesDir(null))) importZip(null);
            else pickZip();
        });
        if (folder != null) {
            folder.setOnClickListener(view -> pickFolder());
            folder.setVisibility(pendingBuild(files) ? android.view.View.GONE : android.view.View.VISIBLE);
        }
        if (pendingBuild(files)) setup.phase("02  COMPILE / RESUME");
        // Keep content inside system bars/cutouts before applying the 80% width limit.
        getWindow().setDecorFitsSystemWindows(false);
        setup.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.displayCutout());
            view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
            return insets;
        });
        setContentView(setup);
        setup.requestApplyInsets();
        picking = state != null && state.getBoolean("picking");
        setSelectionEnabled(!picking && files != null);
        if (files == null) status.setText("App storage is unavailable. Check storage and restart SCDA.");
        else if (state != null && state.getBoolean("importing"))
            status.setText("The previous import was interrupted. Select the input again to retry.");
        select.requestFocus();
    }

    private void cancelSetup() {
        if (worker != null) {
            worker.interrupt();
            setup.workProgress("Cancelling… Verified compiler work will be retained.", -1);
        } else if (pendingBuild(getExternalFilesDir(null))) importZip(null);
        else {
            setup.showImportPage();
            setSelectionEnabled(true);
        }
    }

    private void showRequirements() {
        String message = supportsLocalCompilation()
            ? "Use your own supported original Xbox game, not PC or Xbox 360.\n\n"
                + "Import ISO/XISO, ZIP, 7z, RAR or TAR, or an extracted folder with default.xbe, Default/ and Media/. "
                + "Archives may contain one ISO or one game folder. Encrypted/split archives and CHD/CSO/CCI are unsupported.\n\n"
                + "Allow about 3.5 GB for campaign files, plus the input copy, temporary extraction and at least 512 MiB for compilation. "
                + "Compilation took 6–8 minutes on the tested phone, excluding import. Keep setup open; Back cancels.\n\n"
                + "Matching prebuilt-library ZIPs also work. Saves and settings are not changed. Controller required."
            : "Use a ZIP from the matching Windows builder. ZIP root: libscda_code.so and assets/. "
                + "Inside assets/: default.xbe, Default/ and Media/.\n\n"
                + "Allow space for the extracted game in addition to the ZIP. Keep setup open; Back cancels. "
                + "Saves and settings are not changed. Controller required.";
        new android.app.AlertDialog.Builder(this).setTitle("Requirements").setMessage(message)
            .setPositiveButton("Close", null).show();
    }

    // Builds without installed compiler tools retain the Windows-builder ZIP path.
    protected boolean supportsLocalCompilation() { return false; }
    protected void showImportLicenses() {}
    protected void compileGame(File files, GameImport.Progress progress) throws IOException {}
    protected void importSource(Uri uri, File files, int machine, GameImport.Progress update) throws IOException {
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("Cannot open the selected ZIP.");
            GameImport.extract(input, files, machine, supportsLocalCompilation(), update);
        }
    }

    private String importLabel() { return supportsLocalCompilation() ? "Import game file\nISO / XISO · ZIP · 7z · RAR · TAR" : "Import game ZIP"; }
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
        boolean pending = pendingBuild(getExternalFilesDir(null));
        setup.showWorkPage(pending || uri == null, uri == null ? "Preparing to resume…" : "Opening input…", true);
        setup.phase(pending || uri == null ? "02  COMPILE" : "01  IMPORT");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        worker = new Thread(() -> {
            String error = null;
            try {
                File files = getExternalFilesDir(null);
                if (files == null) throw new IOException("App storage is unavailable.");
                // The setup and native runtime execute in the same ABI/process.
                int machine = android.os.Build.SUPPORTED_ABIS[0].equals("x86_64") ? 62 : 183;
                long[] lastUpdate = {0};
                GameImport.Progress update = new GameImport.Progress() {
                    @Override public void update(String name, long bytes) { publish(name, bytes, -1); }
                    @Override public void updatePercent(String name, int value) { publish(name, -1, value); }
                    private void publish(String name, long bytes, int value) {
                        long now = android.os.SystemClock.elapsedRealtime();
                        if (bytes >= 0 && now - lastUpdate[0] < 250) return;
                        lastUpdate[0] = now;
                        String text = bytes < 0 ? name : String.format(Locale.ROOT, "Processed %.1f MiB · %s", bytes / 1048576.0, name);
                        runOnUiThread(() -> { if (!isDestroyed()) setup.workProgress(text, value); });
                    }
                };
                if (uri != null) importSource(uri, files, machine, update);
                if (pendingBuild(files)) {
                    runOnUiThread(() -> {
                        if (!isDestroyed()) {
                            setup.phase("02  COMPILE");
                            setup.showWorkPage(true, "Starting compilation…", true);
                        }
                    });
                    compileGame(files, update);
                }
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
                    boolean failedBuild = pendingBuild(getExternalFilesDir(null));
                    setup.showWorkPage(failedBuild, "Setup stopped: " + failure
                        + "\n\nYour saves and settings were not changed. "
                        + (pending ? "Verified objects are retained. Resume when ready." : "Choose another file to retry."), false);
                    setSelectionEnabled(true);
                    if (failedBuild) setup.cancel.setOnClickListener(view -> importZip(null));
                    else setup.cancel.setOnClickListener(view -> {
                        setup.showImportPage();
                        setSelectionEnabled(true);
                    });
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
        if (worker != null) cancelSetup();
        else if (setup != null && setup.isWorkPageVisible()) cancelSetup();
        else super.onBackPressed();
    }

    @Override protected void onDestroy() {
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }
}
