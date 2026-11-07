package io.github.mogeldev.scda;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;

final class ExtractTools {
    static void extract(Context context, String tool, Path input, Path output, GameImport.Progress progress) throws IOException {
        progress.update(tool.equals("xiso") ? "Extracting Xbox ISO…" : "Extracting archive…", -1);
        Path diagnostic = Files.createTempFile(output.getParent(), "extract-", ".log");
        Process process = new ProcessBuilder(new File(context.getApplicationInfo().nativeLibraryDir, "libscda_" + tool + ".so").toString(),
            input.toString(), output.toString()).redirectErrorStream(true).redirectOutput(diagnostic.toFile()).start();
        long deadline = android.os.SystemClock.elapsedRealtime() + 30L * 60 * 1000;
        try {
            while (!process.waitFor(500, TimeUnit.MILLISECONDS)) {
                GameSource.interrupted();
                if (android.os.SystemClock.elapsedRealtime() > deadline) throw new IOException("Extraction timed out");
                if (Files.size(diagnostic) > (1 << 20)) throw new IOException("Extraction diagnostics exceed limit");
                if (output.toFile().getUsableSpace() < (32L << 20)) throw new IOException("Not enough free storage");
            }
            if (Files.size(diagnostic) > (1 << 20)) throw new IOException("Extraction diagnostics exceed limit");
            String text = CompileCache.text(diagnostic);
            android.util.Log.i("SCDAImport", tool + " exit=" + process.exitValue() + " " + text);
            String marker = tool.equals("xiso") ? "XISO_EXTRACT_PASS" : "ARCHIVE_EXTRACT_PASS";
            if (process.exitValue() != 0 || !text.contains(marker))
                throw new IOException("Extraction failed: " + text.substring(0, Math.min(2000, text.length())));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Import cancelled; select the input again to retry", e);
        } finally {
            process.destroyForcibly();
            boolean interrupted = Thread.interrupted();
            try { process.waitFor(); } catch (InterruptedException e) { interrupted = true; }
            if (interrupted) Thread.currentThread().interrupt();
            Files.deleteIfExists(diagnostic);
        }
    }
}
