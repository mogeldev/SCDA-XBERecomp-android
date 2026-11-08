package io.github.mogeldev.scda;

import android.app.AlertDialog;
import android.content.Context;
import android.util.AtomicFile;
import android.view.View;
import android.widget.Button;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

// Separate from game settings: recovery works even when Vulkan/game startup crashes.
final class DriverChoice {
    private static Boolean supported;

    private static native boolean nativeSupported();

    // The GPU is in the bundled Turnip's device list (same check as the game: turnip_detect.h).
    static boolean supported(Context c) {
        if (supported == null) {
            boolean ok = false;
            if (new File(c.getApplicationInfo().nativeLibraryDir, "libvulkan_freedreno.so").isFile()) {
                try {
                    System.loadLibrary("scda_gpuprobe");
                    ok = nativeSupported();
                } catch (Throwable e) {
                    ok = false;
                }
            }
            supported = ok;
        }
        return supported;
    }

    private static AtomicFile file(Context c) { return new AtomicFile(new File(c.getFilesDir(), "turnip-enabled")); }
    private static boolean selected(Context c) {
        try { return new String(file(c).readFully(), StandardCharsets.UTF_8).equals("1"); }
        catch (Exception e) { return false; }
    }

    // Hidden on unsupported GPUs, unless Turnip is still selected: switching back must stay possible.
    static void attach(Button b) {
        Context c = b.getContext();
        boolean ok = supported(c), turnip = selected(c);
        b.setVisibility(ok || turnip ? View.VISIBLE : View.GONE);
        b.setText("Vulkan driver: " + (turnip ? "Turnip v4.5 (experimental)" : "Manufacturer"));
        b.setOnClickListener(v -> {
            SetupView screen = null;
            for (android.view.ViewParent p = b.getParent(); p != null; p = p.getParent())
                if (p instanceof SetupView) { screen = (SetupView)p; break; }
            if (screen != null) {
                SetupView target = screen;
                target.driverPage(ok, selected(c), choice -> {
                    if (save(b, choice)) target.message("Restart required",
                        (choice ? "Turnip v4.5" : "Manufacturer") + " selected. Close the app completely and reopen it to apply. Saves and caches are preserved.", null, null);
                });
                return;
            }
            AlertDialog.Builder d = new AlertDialog.Builder(c).setTitle("Vulkan driver — next app start")
                .setMessage(ok
                    ? "Turnip universal v4.5 is experimental. After changing, close the app completely and reopen it. If the game fails to start, return here and select Manufacturer. Saves and caches are preserved."
                    : "This GPU is not supported by the bundled Turnip driver. Select Manufacturer.")
                .setNegativeButton("Manufacturer", (x, w) -> save(b, false))
                .setNeutralButton("Cancel", null);
            if (ok) d.setPositiveButton("Turnip v4.5", (x, w) -> save(b, true));
            d.show();
        });
    }

    private static boolean save(Button b, boolean turnip) {
        AtomicFile f = file(b.getContext());
        FileOutputStream stream = null;
        try {
            stream = f.startWrite();
            stream.write(turnip ? '1' : '0');
            f.finishWrite(stream);
            attach(b);
            return true;
        } catch (Exception e) {
            if (stream != null) f.failWrite(stream);
            Toast.makeText(b.getContext(), "Could not save driver selection", Toast.LENGTH_LONG).show();
            return false;
        }
    }
}
