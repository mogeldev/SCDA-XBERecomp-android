package io.github.mogeldev.scda;

// UI-only emulator harness. No game data or importer. Explicit simulated progress, never production extras.
public final class SetupPreviewActivity extends android.app.Activity {
    private SetupView view;
    @Override public void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        view = new SetupView(this, !getIntent().getBooleanExtra("prebuilt", false),
            () -> view.browse("Requirements", new String[]{"Game", "Storage", "Device"},
                i -> new String[]{"UI preview only. No game files are read.", "Game files plus input and temporary extraction.",
                    "Android 10+, Vulkan 1.1. ARM64 local compilation; x86_64 prebuilt ZIP."}[i], false),
            () -> view.browse("Licenses", new String[]{"Example notice"}, i -> "UI preview only — sample notice, not a license audit.", true),
            () -> {
                view.showWorkPage(true, "Cancelled. 54 verified objects retained (simulation).", false);
                view.cancel.setOnClickListener(v -> compile());
            });
        view.version("UI preview");
        view.update.setOnClickListener(v -> view.message("Updates", "Simulated network error. Retry later.", null, null));
        view.play.setOnClickListener(v -> view.message("Play", "Preview only: game not launched.", null, null));
        view.select.setOnClickListener(v -> compile());
        if (view.folder != null) view.folder.setOnClickListener(v -> {
            view.showWorkPage(false, "Extracting files (simulation)…", true);
            view.phase("01 IMPORT");
        });
        view.driver.setVisibility(android.view.View.VISIBLE);
        view.driver.setOnClickListener(v -> view.driverPage(true, false,
            choice -> view.message("Restart required", "Preview only: selection not saved.", null, null)));
        getWindow().setDecorFitsSystemWindows(false);
        view.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(safe.left, safe.top, safe.right, safe.bottom);
            return insets;
        });
        setContentView(view);
        view.requestApplyInsets();
        String page = getIntent().getStringExtra("page");
        if ("ready".equals(page)) view.showReady();
        else if ("compile".equals(page)) compile();
        else if ("error".equals(page)) view.showWorkPage(false, "Simulated import error. Saves and settings unchanged.", false);
    }
    private void compile() {
        view.showWorkPage(true, "Compiling 54 / 129 objects (simulation)", true);
        view.phase("02 COMPILE");
        view.workProgress("Compiling 54 / 129 objects (simulation)", 42);
    }
    @Override public void onBackPressed() {
        if (view.back()) return;
        if (view.isWorkPageVisible()) { view.cancel.performClick(); return; }
        super.onBackPressed();
    }
}
