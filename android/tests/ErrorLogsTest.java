package io.github.mogeldev.scda;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

public class ErrorLogsTest {
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("scda-logs-test");
        try {
            Path logs = Files.createDirectories(root.resolve("logs"));
            Files.writeString(logs.resolve("scda.log"), "error: test");
            Files.writeString(logs.resolve("scda_001.log"), "previous run");
            Files.writeString(logs.resolve("private.txt"), "excluded");
            Files.writeString(root.resolve("last-setup-error.txt"), "setup failed");
            Files.writeString(root.resolve("save.dat"), "excluded");
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            ErrorLogs.write(bytes, root.toFile(), root.toFile());
            Set<String> names = new HashSet<>();
            try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                    names.add(entry.getName());
                    if (entry.getName().equals("logs/scda.log") && !new String(zip.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).equals("error: test"))
                        throw new AssertionError("Log content changed");
                }
            }
            if (!names.equals(Set.of("README.txt", "logs/scda.log", "logs/scda_001.log", "last-setup-error.txt")))
                throw new AssertionError(names);
            ErrorLogs.write(new ByteArrayOutputStream(), null, root.resolve("missing").toFile());
            System.out.println("PASS: log ZIP allowlist, contents and missing storage");
        } finally {
            try (var paths = Files.walk(root)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) Files.delete(path);
            }
        }
    }
}
