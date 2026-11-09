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
        try {
            GameImport.appVersion = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            GameImport.appVersion = null;
        }
        File files = getExternalFilesDir(null);
        // The launcher always opens first; an installed game (also from older USB/ADB layouts) shows Play.
        boolean installed = files != null && ready(files);
        // The app icon while the game runs in the background returns to it; only a fresh start shows the launcher.
        if (installed && SDLActivity.mCurrentNativeState != null) {
            launchGame();
            return;
        }
        setup = new SetupView(this, supportsLocalCompilation(), this::showRequirements, this::showImportLicenses, this::cancelSetup);
        setup.version(GameImport.appVersion != null ? GameImport.appVersion : "unknown version");
        DriverChoice.attach(setup.driver);
        setup.report.setOnClickListener(view -> DeviceReport.show(this));
        setup.update.setOnClickListener(view -> checkUpdates(false));
        setup.play.setOnClickListener(view -> launchGame());
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
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            setup.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                    | android.view.WindowInsets.Type.displayCutout());
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
                return insets;
            });
        } else {
            setup.setFitsSystemWindows(true);
        }
        setContentView(setup);
        setup.requestApplyInsets();
        picking = state != null && state.getBoolean("picking");
        setSelectionEnabled(!picking && files != null);
        if (files == null) status.setText("App storage is unavailable. Check storage and restart SCDA.");
        else if (state != null && state.getBoolean("importing"))
            status.setText("The previous import was interrupted. Select the input again to retry.");
        select.requestFocus();
        if (installed) setup.showReady();
        // every fresh start asks quietly; only a newer release is shown
        if (state == null) checkUpdates(true);
    }

    // Asks GitHub for the newest release (pre-releases included) and offers its page. Quiet (app start): no button
    // state, and nothing shown unless a newer version is available.
    private void checkUpdates(boolean quiet) {
        if (!quiet) {
            setup.update.setEnabled(false);
            setup.update.setText("Checking…");
        }
        String current = GameImport.appVersion;
        new Thread(() -> {
            String message, newest = null;
            try {
                String tag = UpdateCheck.newest(UpdateCheck.fetch(current));
                if (tag != null && UpdateCheck.isNewer(tag, current)) {
                    newest = tag;
                    message = "Version " + tag.replaceFirst("^v", "") + " is available. You have " + current + ".\n\n"
                        + "Download the APK of that version from its release page. "
                        + "Install the APK over this app; do not uninstall it, or saves and settings are deleted.";
                } else message = "You have the newest version (" + current + ").";
            } catch (IOException | RuntimeException e) {
                android.util.Log.w("SCDAUpdate", "Update check failed", e);
                message = "Could not reach GitHub. Check the internet connection and try again.";
            }
            String text = message, tag = newest;
            runOnUiThread(() -> {
                if (isDestroyed() || isFinishing()) return;
                if (quiet && (tag == null || worker != null || picking)) return;  // not over a running import
                if (!quiet) {
                    setup.update.setEnabled(true);
                    setup.update.setText("Check for updates");
                }
                setup.message(tag != null ? "Update available" : "Updates", text,
                    tag != null ? "Open release page" : null, () -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(UpdateCheck.page(tag))));
                    } catch (ActivityNotFoundException e) {
                        status.setText("No browser available. Open " + UpdateCheck.page(tag));
                    }
                });
            });
        }, "SCDA update check").start();
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
                + "Compilation took 6–8 minutes on the tested ARM64 phone, excluding import; other devices may differ. Keep setup open; Back cancels.\n\n"
                + "Saves and settings are not changed. Controller recommended; touch controls are available."
            : "Use a ZIP from the matching Windows builder. ZIP root: lib/arm64-v8a/libscda_code.so, lib/x86_64/libscda_code.so and assets/. "
                + "Inside assets/: default.xbe, Default/ and Media/.\n\n"
                + "Allow space for the extracted game in addition to the ZIP. Keep setup open; Back cancels. "
                + "Saves and settings are not changed. Controller recommended; touch controls are available.";
        String[] labels = {"Game", "Formats / setup", "Storage", "Device", "Controls", "Your data"};
        String[] details = {
            "Your own supported original Xbox copy is required. Not PC or Xbox 360. The importer checks the executable hash. No game content is included.",
            message,
            "Allow about 3.5 GB for campaign files, plus the input copy and temporary extraction. Local compilation requires at least 512 MiB free. A working installation does not need reimporting after a normal app update.",
            "Android 10 or newer (Android 10 experimental). Vulkan 1.1. ARM64 and x86_64 compile the game on the device. Compatibility depends on the GPU and driver.",
            "Controller recommended; touch controls are available. Optional phone gyro aiming is configured in the in-game controller settings.",
            "Install updates over the existing app with the same signing certificate. Do not uninstall or clear app data: saves and settings would be deleted."
        };
        setup.browse("Requirements", labels, i -> details[i], false);
    }

    // Builds without installed compiler tools retain the Windows-builder ZIP path.
    protected boolean supportsLocalCompilation() { return false; }
    protected void showImportLicenses() {
        try {
            String[] names = getAssets().list("licenses");
            if (names == null || names.length == 0) throw new IOException("No packaged notices found.");
            java.util.Arrays.sort(names);
            setup.browse("Licenses", names, i -> {
                try (java.io.Reader reader = new java.io.InputStreamReader(getAssets().open("licenses/" + names[i]),
                        java.nio.charset.StandardCharsets.UTF_8)) {
                    StringBuilder text = new StringBuilder();
                    char[] buffer = new char[4096];
                    for (int n; (n = reader.read(buffer)) != -1;) text.append(buffer, 0, n);
                    return text.toString();
                } catch (IOException e) { return "Cannot read notice: " + e.getMessage(); }
            }, true);
        } catch (IOException e) { setup.message("Licenses", e.getMessage(), null, null); }
    }
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
        else if (setup != null && setup.back()) return;
        else if (setup != null && setup.isWorkPageVisible()) cancelSetup();
        else super.onBackPressed();
    }

    @Override protected void onDestroy() {
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }
}
