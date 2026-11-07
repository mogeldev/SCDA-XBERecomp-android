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
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

// Native views retain Android accessibility, controller focus and text scaling. No WebView/assets.
final class SetupView extends ScrollView {
    private static final int BG = 0xff080e0b, PANEL = 0xff111b15, GREEN = 0xffb2ef83;
    private static final int TEXT = 0xffedf4ee, MUTED = 0xffa9b8ae, BORDER = 0xff314537;
    final Button select, folder, cancel, play, update;
    final ProgressBar progress;
    final TextView status, percent;
    private final TextView brand, title, subtitle, stage, workTitle;
    private final LinearLayout choices, importPage, workPage, content;
    private final FrameLayout center;
    private Boolean stacked;

    SetupView(Context context, boolean local, Runnable requirements, Runnable licenses, Runnable cancelAction) {
        super(context);
        setBackgroundColor(BG);
        setFillViewport(true);
        setClipToPadding(false);
        center = new FrameLayout(context);
        content = column();
        center.addView(content, new FrameLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        addView(center, new ScrollView.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        brand = text("● ● ●   THIRD ECHELON / ANDROID", 11, GREEN, true);
        brand.setPadding(0, dp(4), 0, dp(12));
        content.addView(brand);

        importPage = column();
        content.addView(importPage);
        LinearLayout card = column();
        card.setBackground(new Grid());
        card.setPadding(dp(20), dp(16), dp(20), dp(12));
        importPage.addView(card);
        card.addView(text("SPLINTER CELL: DOUBLE AGENT", 11, GREEN, true));
        title = text("Import game", 28, TEXT, false);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        title.setPadding(0, dp(4), 0, dp(4));
        title.setAccessibilityHeading(true);
        card.addView(title);
        subtitle = text(local ? "Choose your game source. Compilation runs on this device."
            : "Select a ZIP from the matching Windows builder.", 14, MUTED, false);
        card.addView(subtitle);
        stage = text("01  IMPORT      /      02  COMPILE      /      03  PLAY", 11, GREEN, true);
        stage.setPadding(0, dp(12), 0, dp(12));
        card.addView(stage);

        choices = new LinearLayout(context);
        choices.setOrientation(LinearLayout.HORIZONTAL);
        card.addView(choices);
        select = button(local ? "Import game file\nISO / XISO · ZIP · 7z · RAR · TAR" : "Import game ZIP", true);
        choices.addView(select, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        // the launcher's main action once a game is installed (showReady)
        play = button("Play", true);
        play.setTextSize(18);
        play.setVisibility(GONE);
        choices.addView(play, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        folder = local ? button("Select extracted folder\ndefault.xbe + Default/ + Media/", false) : null;
        if (folder != null) {
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
            params.setMarginStart(dp(12));
            choices.addView(folder, params);
        }
        TextView facts = text("Controller recommended, touch controls available  ·  Saves and settings unchanged", 11, MUTED, false);
        facts.setPadding(0, dp(10), 0, dp(4));
        card.addView(facts);

        LinearLayout footer = new LinearLayout(context);
        footer.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        Button help = button("Requirements", false);
        help.setTextSize(12);
        help.setOnClickListener(v -> requirements.run());
        footer.addView(help, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        if (local) {
            Button notice = button("Tools & licenses", false);
            notice.setTextSize(12);
            notice.setOnClickListener(v -> licenses.run());
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
            params.setMarginStart(dp(12));
            footer.addView(notice, params);
        }
        update = button("Check for updates", false);
        update.setTextSize(12);
        LinearLayout.LayoutParams updateParams = new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1);
        updateParams.setMarginStart(dp(12));
        footer.addView(update, updateParams);
        card.addView(footer);
        TextView fine = text("Supported original Xbox game required. No game content included."
            + (local ? "\nISO: extract-xiso. This product includes software developed by in <in@fishtank.com>." : ""), 10, MUTED, false);
        fine.setPadding(0, dp(8), 0, dp(2));
        importPage.addView(fine);

        workPage = column();
        workPage.setVisibility(GONE);
        content.addView(workPage);
        LinearLayout workCard = column();
        workCard.setBackground(new Grid());
        workCard.setPadding(dp(20), dp(18), dp(20), dp(16));
        workPage.addView(workCard);
        workCard.addView(text("SPLINTER CELL: DOUBLE AGENT", 11, GREEN, true));
        workTitle = text("Importing game", 26, TEXT, false);
        workTitle.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        workTitle.setPadding(0, dp(5), 0, dp(10));
        workCard.addView(workTitle);
        progress = new ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        progress.setIndeterminateTintList(ColorStateList.valueOf(GREEN));
        progress.setProgressTintList(ColorStateList.valueOf(GREEN));
        progress.setVisibility(GONE);
        workCard.addView(progress, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(8)));
        LinearLayout progressLine = new LinearLayout(context);
        progressLine.setGravity(Gravity.CENTER_VERTICAL);
        status = text("", 13, MUTED, false);
        progressLine.addView(status, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        percent = text("", 12, GREEN, true);
        percent.setVisibility(GONE);
        progressLine.addView(percent);
        status.setPadding(0, dp(12), 0, dp(8));
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        workCard.addView(progressLine);
        LinearLayout actions = new LinearLayout(context);
        actions.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        cancel = button("Back", false);
        cancel.setOnClickListener(v -> cancelAction.run());
        actions.addView(cancel, new LinearLayout.LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
        workCard.addView(actions);
        TextView note = text("Keep setup open. Completed compiler work is saved and can be resumed.", 11, MUTED, false);
        note.setPadding(0, dp(10), 0, 0);
        workPage.addView(note);
    }

    boolean isWorkPageVisible() { return workPage.getVisibility() == VISIBLE; }

    void version(String name) { brand.setText("● ● ●   THIRD ECHELON / ANDROID   ·   " + name); }

    // Launcher page for an installed game: Play instead of the import choices.
    void showReady() {
        title.setText("Ready");
        subtitle.setText("Your game is installed. Saves and settings stay on this device.");
        stage.setText("01  IMPORT  ✓      /      02  COMPILE  ✓      /      03  PLAY");
        select.setVisibility(GONE);
        if (folder != null) folder.setVisibility(GONE);
        play.setVisibility(VISIBLE);
        play.requestFocus();
    }

    void showImportPage() {
        workPage.setVisibility(GONE);
        importPage.setVisibility(VISIBLE);
        stage.setText("01  IMPORT      /      02  COMPILE      /      03  PLAY");
        select.requestFocus();
    }

    void showWorkPage(boolean compiling, String message, boolean active) {
        importPage.setVisibility(GONE);
        workPage.setVisibility(VISIBLE);
        workTitle.setText(compiling ? "Compiling game" : "Importing game");
        progress.setIndeterminate(true);
        progress.setVisibility(active ? VISIBLE : GONE);
        percent.setVisibility(GONE);
        cancel.setText(active ? "Back / cancel" : compiling ? "Compile / resume" : "Choose another file");
        stage.setText(compiling ? "01  IMPORT  ✓      /      02  COMPILE      /      03  PLAY"
            : "01  IMPORT      /      02  COMPILE      /      03  PLAY");
        status.setText(message);
        (active ? cancel : select).requestFocus();
    }

    void phase(String text) {
        stage.setText(text);
        if (text.startsWith("02")) workTitle.setText("Compiling game");
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

    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(getContext());
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private TextView text(String value, int size, int color, boolean mono) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (mono) view.setTypeface(Typeface.MONOSPACE);
        return view;
    }

    private Button button(String label, boolean primary) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setTextSize(13);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        button.setPadding(dp(14), dp(10), dp(14), dp(10));
        button.setMinHeight(dp(48));
        button.setMinimumHeight(dp(48));
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setStateListAnimator(null);
        int foreground = primary ? BG : TEXT;
        button.setTextColor(new ColorStateList(new int[][]{{-android.R.attr.state_enabled}, {}},
            new int[]{0xff84978a, foreground}));
        StateListDrawable backgrounds = new StateListDrawable();
        backgrounds.addState(new int[]{-android.R.attr.state_enabled}, box(PANEL, BORDER, 1));
        backgrounds.addState(new int[]{android.R.attr.state_pressed}, box(primary ? 0xffd1ffad : 0xff29432e, TEXT, 2));
        backgrounds.addState(new int[]{android.R.attr.state_focused}, box(primary ? GREEN : 0xff203526, TEXT, 2));
        backgrounds.addState(new int[]{}, box(primary ? GREEN : PANEL, primary ? GREEN : BORDER, 1));
        button.setBackground(backgrounds);
        return button;
    }

    private Drawable box(int fill, int stroke, int width) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(3));
        drawable.setStroke(dp(width), stroke);
        return drawable;
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int available = Math.max(0, View.MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight());
        int width = Math.min((int) (available * 0.8f), dp(1100));
        content.getLayoutParams().width = width;
        boolean stack = width / getResources().getDisplayMetrics().density
            / getResources().getConfiguration().fontScale < 540;
        if (stacked == null || stack != stacked) {
            stacked = stack;
            choices.setOrientation(stack ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
            boolean before = false;  // a shown button before this one; Play replaces the import buttons
            for (int i = 0; i < choices.getChildCount(); i++) {
                LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                    stack ? LayoutParams.MATCH_PARENT : 0, LayoutParams.WRAP_CONTENT, stack ? 0 : 1);
                if (before) { if (stack) params.topMargin = dp(10); else params.setMarginStart(dp(12)); }
                before |= choices.getChildAt(i).getVisibility() != GONE;
                choices.getChildAt(i).setLayoutParams(params);
            }
        }
        center.setPadding(0, dp(12), 0, dp(12));
        super.onMeasure(widthSpec, heightSpec);
    }

    private final class Grid extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        @Override public void draw(Canvas canvas) {
            android.graphics.Rect r = getBounds();
            paint.setColor(PANEL);
            canvas.drawRect(r, paint);
            paint.setColor(0x0cb2ef83);
            paint.setStrokeWidth(dp(1));
            for (int x = r.left; x < r.right; x += dp(32)) canvas.drawLine(x, r.top, x, r.bottom, paint);
            for (int y = r.top; y < r.bottom; y += dp(32)) canvas.drawLine(r.left, y, r.right, y, paint);
            paint.setColor(BORDER);
            paint.setStyle(Paint.Style.STROKE);
            canvas.drawRect(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, paint);
            paint.setStyle(Paint.Style.FILL);
        }
        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(android.graphics.ColorFilter filter) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.OPAQUE; }
    }
}
