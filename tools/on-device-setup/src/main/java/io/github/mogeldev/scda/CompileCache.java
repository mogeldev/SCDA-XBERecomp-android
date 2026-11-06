package io.github.mogeldev.scda;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Comparator;

final class CompileCache {
    static String hash(Path file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream input = Files.newInputStream(file)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    interrupted();
                    digest.update(buffer, 0, count);
                }
            }
            StringBuilder hex = new StringBuilder();
            for (byte value : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            return hex.toString();
        } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    static String text(Path path) throws IOException {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }

    static void interrupted() throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("Cancelled; completed objects retained. Tap Compile / resume.");
    }

    static void atomicText(Path target, String value) throws IOException {
        Path temporary = target.resolveSibling(target.getFileName() + ".tmp");
        Files.write(temporary, value.getBytes(StandardCharsets.UTF_8));
        Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    static boolean valid(Path output) throws IOException {
        Path receipt = output.resolveSibling(output.getFileName() + ".sha256");
        return Files.isRegularFile(output) && Files.isRegularFile(receipt)
            && text(receipt).equals(hash(output));
    }

    static void seal(Path output) throws IOException {
        atomicText(output.resolveSibling(output.getFileName() + ".sha256"), hash(output));
    }

    static void reset(Path root, String fingerprint) throws IOException {
        Path version = root.resolve("version");
        if (Files.isRegularFile(version) && text(version).equals(fingerprint)) return;
        delete(root);
        Files.createDirectories(root);
        atomicText(version, fingerprint);
    }

    static void delete(Path root) throws IOException {
        if (!Files.exists(root)) return;
        try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
            for (Path path : paths.sorted(Comparator.reverseOrder()).toArray(Path[]::new)) Files.delete(path);
        }
    }
}
