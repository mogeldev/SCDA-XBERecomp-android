package io.github.mogeldev.scda;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.zip.*;

final class ErrorLogs {
    static void write(OutputStream output, File external, File internal) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            zip.putNextEntry(new ZipEntry("README.txt"));
            zip.write("SCDA saved error logs. May contain file paths; review before sharing. No game files or saves included. Missing logs were not available.\n".getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            if (external != null) {
                File[] logs = new File(external, "logs").listFiles();
                if (logs != null) for (File log : logs) {
                    if (log.getName().matches("scda(?:_[0-9]+)?\\.log")) add(zip, log, "logs/" + log.getName());
                }
            }
            add(zip, new File(internal, "last-setup-error.txt"), "last-setup-error.txt");
            add(zip, new File(internal, "last-vulkan-report.txt"), "last-vulkan-report.txt");
        }
    }

    private static void add(ZipOutputStream zip, File file, String name) throws IOException {
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        zip.putNextEntry(new ZipEntry(name));
        Files.copy(file.toPath(), zip);
        zip.closeEntry();
    }
}
