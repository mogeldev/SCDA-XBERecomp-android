package io.github.mogeldev.scda;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

// No upload client. Only explicit share/issue intents after the user reviews the report.
final class DeviceReport {
    private static final String[] RESULTS = {"Not tested", "Starts", "Gameplay works", "Graphics issues", "Crashes"};

    static void show(Activity activity) {
        String version;
        long updated;
        try {
            android.content.pm.PackageInfo p = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0);
            version = p.versionName;
            updated = p.lastUpdateTime;
        } catch (Exception e) { version = "Unknown"; updated = Long.MAX_VALUE; }
        String snapshot = "Not recorded for this app installation/update. Start the game once first.";
        File f = new File(activity.getFilesDir(), "last-vulkan-report.txt");
        try {
            if (f.isFile() && f.length() <= 4096 && f.lastModified() >= updated) {
                snapshot = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
                snapshot += "Recorded: " + new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm z", java.util.Locale.ROOT)
                    .format(new java.util.Date(f.lastModified()));
            }
        } catch (Exception ignored) { }
        String selected = "Manufacturer";
        try {
            File choice = new File(activity.getFilesDir(), "turnip-enabled");
            if (choice.length() <= 16 && new String(Files.readAllBytes(choice.toPath()), StandardCharsets.UTF_8).trim().equals("1"))
                selected = "Turnip";
        } catch (Exception ignored) { }
        String base = "SCDA Android device report\n\nApp: " + version
            + "\nDevice: " + Build.MANUFACTURER + " / " + Build.MODEL
            + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
            + "\nRuntime ABI: " + (android.os.Process.is64Bit() ? Build.SUPPORTED_64_BIT_ABIS[0] : Build.SUPPORTED_32_BIT_ABIS[0])
            + "\nSelected driver (next full restart): " + selected
            + "\nTurnip eligibility: " + (DriverChoice.supported(activity) ? "Supported match" : "Unsupported or detection unavailable")
            + "\n\nLast successful Vulkan device initialization (not proof of gameplay):\n" + snapshot;

        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(16 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad, pad, pad);
        TextView privacy = new TextView(activity);
        privacy.setText("Nothing is sent automatically. Review before sharing. GitHub issues are public and linked to your account. "
            + "No unique device IDs, accounts, paths, raw logs or game files are included automatically. Do not put personal information in notes.");
        content.addView(privacy);
        Spinner result = new Spinner(activity);
        result.setContentDescription("Player test result");
        result.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, RESULTS));
        content.addView(result);
        EditText notes = new EditText(activity);
        notes.setHint("Optional notes: scene, symptoms, settings (no personal information)");
        notes.setMinLines(2);
        notes.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(600)});
        content.addView(notes);
        TextView preview = new TextView(activity);
        preview.setTextIsSelectable(true);
        preview.setText(base);
        content.addView(preview);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        final String data = base;
        new AlertDialog.Builder(activity).setTitle("Share device report — preview").setView(scroll)
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Share", (d, w) -> send(activity, data, RESULTS[result.getSelectedItemPosition()], notes.getText().toString(), false))
            .setPositiveButton("Review on GitHub", (d, w) -> send(activity, data, RESULTS[result.getSelectedItemPosition()], notes.getText().toString(), true))
            .show();
    }

    private static void send(Activity a, String base, String result, String notes, boolean github) {
        String report = base + "\n\nPlayer result: " + result + "\nNotes: " + notes
            + "\n\nPlayer-submitted report; not a compatibility certification.";
        try {
            if (github) {
                Uri url = Uri.parse("https://github.com/mogeldev/SCDA-XBERecomp-android/issues/new").buildUpon()
                    .appendQueryParameter("title", "Device report: " + Build.MANUFACTURER + " " + Build.MODEL)
                    .appendQueryParameter("body", report).build();
                a.startActivity(new Intent(Intent.ACTION_VIEW, url));
            } else {
                Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, report)
                    .putExtra(Intent.EXTRA_SUBJECT, "SCDA Android device report");
                a.startActivity(Intent.createChooser(send, "Share device report"));
            }
        } catch (android.content.ActivityNotFoundException e) {
            Toast.makeText(a, "No compatible browser/share app available. You can copy the preview text.", Toast.LENGTH_LONG).show();
        }
    }
}
