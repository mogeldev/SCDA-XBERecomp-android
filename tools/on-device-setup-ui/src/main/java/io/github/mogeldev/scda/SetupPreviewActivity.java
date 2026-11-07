package io.github.mogeldev.scda;

// UI-only emulator harness. No native libraries, game data or file-import actions.
public final class SetupPreviewActivity extends android.app.Activity {
    @Override public void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        SetupView view = new SetupView(this, !getIntent().getBooleanExtra("prebuilt", false),
            () -> new android.app.AlertDialog.Builder(this).setTitle("Requirements")
                .setMessage("UI preview only. No game files are read.").setPositiveButton("Close", null).show(),
            () -> new android.app.AlertDialog.Builder(this).setTitle("Tools & licenses")
                .setMessage("UI preview only. The app displays its packaged notices here.").setPositiveButton("Close", null).show());
        getWindow().setDecorFitsSystemWindows(false);
        view.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets safe = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(safe.left, safe.top, safe.right, safe.bottom);
            return insets;
        });
        view.select.setOnClickListener(v -> {
            view.phase("02  COMPILE / RESUME");
            view.status.setText("Preview: compilation interrupted. 54 verified objects retained.");
            view.select.setText("Compile / resume");
            if (view.folder != null) view.folder.setVisibility(android.view.View.GONE);
        });
        if (view.folder != null) view.folder.setOnClickListener(v -> {
            view.phase("01  IMPORT");
            view.progress.setVisibility(android.view.View.VISIBLE);
            view.status.setText("Preview: extracting game files…");
        });
        setContentView(view);
        view.requestApplyInsets();
    }
}
