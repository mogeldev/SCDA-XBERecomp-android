package io.github.mogeldev.scda;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

// Native, landscape menu. Retains Android focus/accessibility and real importer progress; no WebView/game art.
final class SetupView extends LinearLayout {
    static final int TEXT = 0xffc9cdc8, DIM = 0xffa2aaa0, GREEN = 0xffa9c2a2;
    final Button select, folder, cancel, play, update, driver, report, logs;
    final ProgressBar progress;
    final TextView status, percent;
    private final TextView heading, subheading, stage, version;
    private final LinearLayout panel, home, work, footer;
    private final ScrollView scroll;
    private Button back;
    private final boolean local;
    private boolean installed, working;
    private Runnable returnPage;
    private View previousFocus;
    private Button lastAction;

    SetupView(Context context, boolean local, Runnable requirements, Runnable licenses, Runnable cancelAction) {
        super(context);
        this.local = local;
        setOrientation(VERTICAL);
        setGravity(Gravity.CENTER_HORIZONTAL);
        setBackground(new Backdrop());
        TextView brand = text("SCDA - Android", 23, GREEN);
        brand.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        brand.setGravity(Gravity.CENTER);
        brand.setPadding(0, dp(6), 0, dp(6));
        addView(brand, new LayoutParams(-1, -2));
        scroll = new ScrollView(context);
        scroll.setFillViewport(true);
        addView(scroll, new LayoutParams(-1, 0, 1));
        LinearLayout center = column();
        center.setGravity(Gravity.CENTER);
        scroll.addView(center, new ScrollView.LayoutParams(-1, -1));
        panel = column();
        center.addView(panel, new LayoutParams(-1, -2));
        heading = bar("SETUP");
        subheading = bar("Import game");
        panel.addView(heading);
        panel.addView(subheading);
        home = column();
        panel.addView(home);
        play = button("Play");
        play.setVisibility(GONE);
        home.addView(play);
        select = button(local ? "Import game file · ISO / archive" : "Import game ZIP");
        home.addView(select);
        folder = local ? button("Extracted folder") : null;
        if (folder != null) home.addView(folder);
        driver = button("Vulkan driver");
        driver.setVisibility(GONE); // activity attaches detection; preview harness has no native code
        home.addView(driver);
        update = button("Check for updates");
        home.addView(update);
        Button help = button("Requirements");
        help.setOnClickListener(v -> requirements.run());
        home.addView(help);
        Button notice = button("Licenses");
        notice.setOnClickListener(v -> licenses.run());
        home.addView(notice);
        report = button("Share device report");
        home.addView(report);
        logs = button("Export error logs (ZIP)");
        home.addView(logs);
        work = column();
        work.setPadding(dp(16), dp(12), dp(16), dp(12));
        work.setBackground(gradient(0xff202220, 0xff080908));
        work.setVisibility(GONE);
        panel.addView(work);
        stage = text("01 IMPORT   /   02 COMPILE   /   03 PLAY", 13, GREEN);
        work.addView(stage);
        progress = new ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal);
        progress.setProgressTintList(ColorStateList.valueOf(GREEN));
        progress.setIndeterminateTintList(ColorStateList.valueOf(GREEN));
        progress.setVisibility(GONE);
        work.addView(progress, new LayoutParams(-1, dp(14)));
        percent = text("", 14, GREEN);
        work.addView(percent);
        cancel = button("Back / cancel");
        cancel.setOnClickListener(v -> cancelAction.run());
        work.addView(cancel);
        status = text("Your own original Xbox copy required. No game content included.", 13, DIM);
        status.setPadding(dp(12), dp(8), dp(12), dp(8));
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        panel.addView(status);
        footer = new LinearLayout(context);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        Button accept = button("Ⓐ Select");
        accept.setTextColor(GREEN);
        accept.setOnClickListener(v -> {
            if (lastAction != null && lastAction.isShown() && lastAction.isEnabled()) lastAction.performClick();
        });
        back = button("Ⓑ Back");
        back.setTextColor(0xffdc9e97);
        accept.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        back.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        accept.setOnFocusChangeListener(null);
        back.setOnFocusChangeListener(null);
        back.setOnClickListener(v -> ((android.app.Activity) getContext()).onBackPressed());
        footer.addView(accept);
        footer.addView(back);
        version = text("", 12, DIM);
        version.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        footer.addView(version, new LayoutParams(0, -2, 1));
        addView(footer, new LayoutParams(-1, -2));
    }

    void version(String name) {
        version.setText(name + "\nSCDA Android · XBE Recomp"
            + (android.os.Build.VERSION.SDK_INT == 29 ? "\nANDROID 10 — EXPERIMENTAL" : ""));
    }
    boolean isWorkPageVisible() { return working; }
    void showReady() {
        installed = true;
        returnHome();
        heading.setText("MAIN MENU");
        subheading.setVisibility(GONE);
        play.setVisibility(VISIBLE);
        select.setVisibility(GONE);
        if (folder != null) folder.setVisibility(GONE);
        // Reimporting over an installed game is deliberately not offered: importer refuses to overwrite it.
        status.setText("Game installed. Saves and settings stay on this device.");
        play.setSelected(true);
        play.requestFocus();
    }
    void showImportPage() {
        returnHome();
        if (installed) { showReady(); return; }
        heading.setText("SETUP");
        subheading.setVisibility(VISIBLE);
        subheading.setText("Import game");
        select.requestFocus();
    }
    private void returnHome() {
        returnPage = null;
        working = false;
        while (panel.getChildCount() > 5) panel.removeViewAt(5);
        home.setVisibility(VISIBLE);
        work.setVisibility(GONE);
        status.setVisibility(VISIBLE);
    }
    void showWorkPage(boolean compiling, String message, boolean active) {
        returnHome();
        working = true;
        home.setVisibility(GONE);
        work.setVisibility(VISIBLE);
        heading.setText("SETUP");
        subheading.setVisibility(VISIBLE);
        subheading.setText(active ? (compiling ? "Compiling game code" : "Importing game") : "Setup stopped");
        progress.setIndeterminate(true);
        progress.setVisibility(active ? VISIBLE : GONE);
        percent.setVisibility(GONE);
        cancel.setText(active ? "Back / cancel" : compiling ? "Compile / resume" : "Choose another file");
        status.setText(message);
        cancel.requestFocus();
    }
    void phase(String text) {
        stage.setText(text);
        if (text.startsWith("02")) subheading.setText("Compile / resume");
    }
    void workProgress(String message, int value) {
        status.setText(message);
        boolean known = value >= 0;
        percent.setVisibility(known ? VISIBLE : GONE);
        progress.setIndeterminate(!known);
        if (known) {
            progress.setProgress(value);
            percent.setText(value + "%");
            progress.setContentDescription("Compilation " + value + " percent");
        } else progress.setContentDescription(message);
    }

    // Internal pages use the same bars and Back path; long content scrolls without clipping controls.
    private LinearLayout page(String title, Runnable onBack) {
        previousFocus = getRootView().findFocus();
        returnHome();
        home.setVisibility(GONE);
        status.setVisibility(GONE);
        heading.setText("SCDA - ANDROID");
        subheading.setVisibility(VISIBLE);
        subheading.setText(title);
        returnPage = onBack;
        LinearLayout content = column();
        content.setBackground(gradient(0xff202220, 0xff080908));
        panel.addView(content);
        scroll.scrollTo(0, 0);
        return content;
    }
    boolean back() {
        if (returnPage == null) return false;
        Runnable action = returnPage;
        action.run();
        return true;
    }
    private void home() {
        View focus = previousFocus;
        if (installed) showReady(); else showImportPage();
        if (focus != null && focus.isShown()) focus.requestFocus();
    }
    void driverPage(boolean supported, boolean selected, java.util.function.Consumer<Boolean> choose) {
        LinearLayout content = page("Vulkan driver · restart required", this::home);
        TextView note = text(supported
            ? "Turnip v4.5 is experimental. Selected: " + (selected ? "Turnip" : "Manufacturer")
                + ". Selection applies after fully closing and restarting the app. If the game fails to start, return here and select Manufacturer."
            : "This GPU is not supported by the bundled driver. Select Manufacturer to recover.", 15, TEXT);
        note.setPadding(dp(16), dp(12), dp(16), dp(12));
        content.addView(note);
        Button manufacturer = button("Manufacturer");
        manufacturer.setOnClickListener(v -> choose.accept(false));
        content.addView(manufacturer);
        manufacturer.requestFocus();
        if (supported) {
            Button turnip = button("Turnip v4.5 (experimental)");
            turnip.setOnClickListener(v -> choose.accept(true));
            content.addView(turnip);
            if (selected) turnip.requestFocus();
        }
    }
    void message(String title, String message, String action, Runnable callback) {
        LinearLayout content = page(title, this::home);
        TextView text = text(message, 16, TEXT);
        text.setPadding(dp(16), dp(12), dp(16), dp(12));
        content.addView(text);
        if (action != null) {
            Button button = button(action);
            button.setOnClickListener(v -> callback.run());
            content.addView(button);
            button.requestFocus();
        }
        Button close = button("Back");
        close.setOnClickListener(v -> home());
        content.addView(close);
        if (action == null) close.requestFocus();
    }
    void browse(String title, String[] labels, java.util.function.IntFunction<String> details, boolean license) {
        browse(title, labels, details, license, 0);
    }
    private void browse(String title, String[] labels, java.util.function.IntFunction<String> details, boolean license, int selected) {
        LinearLayout content = page(title, this::home);
        LinearLayout split = new LinearLayout(getContext());
        content.addView(split, new LayoutParams(-1, dp(240)));
        ScrollView listScroll = new ScrollView(getContext());
        LinearLayout list = column();
        listScroll.addView(list);
        split.addView(listScroll, new LayoutParams(0, -1, 1));
        ScrollView detailScroll = new ScrollView(getContext());
        TextView detail = text("", 15, TEXT);
        detail.setPadding(dp(16), dp(12), dp(16), dp(12));
        detailScroll.addView(detail);
        split.addView(detailScroll, new LayoutParams(0, -1, 1.4f));
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            Button row = button(labels[i]);
            Runnable display = () -> {
                for (int n = 0; n < list.getChildCount(); n++) list.getChildAt(n).setSelected(n == index);
                detail.setText(license ? labels[index] + "\n\nPackaged notice — select to read full text."
                    : details.apply(index));
                detailScroll.scrollTo(0, 0);
            };
            row.setOnFocusChangeListener((v, has) -> { if (has) { lastAction = row; display.run(); } });
            row.setOnClickListener(v -> {
                display.run();
                if (!license) return;
                LinearLayout full = page(labels[index], () -> browse(title, labels, details, true, index));
                TextView body = text(details.apply(index), 13, TEXT);
                body.setTypeface(Typeface.MONOSPACE);
                body.setTextIsSelectable(true);
                body.setPadding(dp(16), dp(12), dp(16), dp(12));
                full.addView(body);
                Button close = button("Back to licenses");
                close.setOnClickListener(x -> back());
                full.addView(close, 0);
                close.requestFocus();
            });
            list.addView(row);
        }
        if (labels.length > 0) {
            list.getChildAt(selected).requestFocus();
            detail.setText(license ? labels[selected] + "\n\nPackaged notice — select to read full text." : details.apply(selected));
            list.getChildAt(selected).setSelected(true);
        }
    }

    LinearLayout column() {
        LinearLayout layout = new LinearLayout(getContext());
        layout.setOrientation(VERTICAL);
        return layout;
    }
    TextView text(String value, int size, int color) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setTypeface(Typeface.create("sans-serif-condensed", Typeface.NORMAL));
        return view;
    }
    private TextView bar(String title) {
        TextView view = text(title, 19, TEXT);
        view.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        view.setPadding(dp(12), dp(4), dp(12), dp(4));
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            new int[]{0xff969a96, 0xff484c48, 0xff151715, 0xff020302});
        bg.setStroke(dp(1), 0xff555b55);
        view.setBackground(bg);
        view.setAccessibilityHeading(true);
        return view;
    }
    Button button(String label) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setTextSize(17);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC));
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        button.setPadding(dp(20), dp(6), dp(10), dp(6));
        button.setMinHeight(dp(48));
        button.setMinimumHeight(dp(48));
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setStateListAnimator(null);
        button.setTextColor(new ColorStateList(new int[][]{{-android.R.attr.state_enabled},
            {android.R.attr.state_pressed}, {android.R.attr.state_focused}, {android.R.attr.state_selected}, {}},
            new int[]{0xff626762, GREEN, GREEN, GREEN, DIM}));
        StateListDrawable backgrounds = new StateListDrawable();
        for (int state : new int[]{android.R.attr.state_pressed, android.R.attr.state_focused, android.R.attr.state_selected})
            backgrounds.addState(new int[]{state}, new RowBackground(true));
        backgrounds.addState(new int[]{}, new RowBackground(false));
        button.setBackground(backgrounds);
        button.setOnFocusChangeListener((v, focused) -> {
            if (focused) { play.setSelected(false); lastAction = button; }
        });
        return button;
    }
    private GradientDrawable gradient(int top, int bottom) {
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{top, bottom});
        bg.setStroke(dp(1), 0xff353935);
        return bg;
    }
    int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int available = Math.max(0, View.MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight());
        // Narrow landscape / large font: use more of the safe width; scroll instead of shrinking text/targets.
        float fontScale = getResources().getConfiguration().fontScale;
        scroll.getLayoutParams().width = Math.min((int)(available * (fontScale > 1.2f ? 0.94f : 0.8f)), dp(1000));
        footer.getLayoutParams().width = Math.min((int)(available * 0.94f), dp(1100));
        super.onMeasure(widthSpec, heightSpec);
    }
    private final class RowBackground extends Drawable {
        private final boolean selected;
        private final Paint p = new Paint();
        RowBackground(boolean selected) { this.selected = selected; }
        @Override public void draw(Canvas c) {
            android.graphics.Rect r = getBounds();
            p.setColor(selected ? 0xff383d38 : 0xff272a27); c.drawRect(r, p);
            p.setColor(0xff494e49); c.drawLine(r.left, r.top, r.right, r.top, p);
            p.setColor(0xff080a08); c.drawLine(r.left, r.bottom - 1, r.right, r.bottom - 1, p);
            p.setColor(selected ? GREEN : 0xff080a08);
            c.drawRect(r.left + dp(7), r.centerY() - dp(6), r.left + dp(10), r.centerY() + dp(6), p);
        }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.OPAQUE; }
    }
    private final class Backdrop extends Drawable {
        private final Paint p = new Paint();
        @Override public void draw(Canvas c) {
            android.graphics.Rect r = getBounds();
            p.setShader(new android.graphics.RadialGradient(r.centerX(), r.centerY(), Math.max(r.width(), r.height()) * .55f,
                new int[]{0xff303630, 0xff131713, 0xff000000}, null, android.graphics.Shader.TileMode.CLAMP));
            c.drawRect(r, p); p.setShader(null);
            p.setColor(0x33000000);
            for (int y = r.top; y < r.bottom; y += dp(3)) c.drawLine(r.left, y, r.right, y, p);
        }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.OPAQUE; }
    }
}
