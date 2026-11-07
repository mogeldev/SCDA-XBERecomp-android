package io.github.mogeldev.scda;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// No Android dependencies: the ZIP trust boundary is also exercised by a plain-Java test.
final class GameImport {
    private static final long MAX_BYTES = 16L * 1024 * 1024 * 1024;
    private static final long SPACE_RESERVE = 32L * 1024 * 1024;
    private static final int MAX_ENTRIES = 100000;

    interface Progress {
        void update(String name, long bytes) throws IOException;
        default void updatePercent(String name, int percent) throws IOException { update(name, -1); }
    }

    static boolean hasAssets(File root) {
        return new File(root, "default.xbe").isFile()
            && new File(root, "Default").isDirectory() && new File(root, "Media").isDirectory();
    }

    static boolean isInstalled(File files) {
        return hasAssets(new File(files, "game/assets")) && new File(files, "game/libscda_code.so").isFile();
    }

    static void extract(InputStream input, File files, int machine, Progress progress) throws IOException {
        extract(input, files, machine, false, progress);
    }

    interface Extractor { void extract(Path stage) throws IOException; }

    static void extract(InputStream input, File files, int machine, boolean allowAssetsOnly, Progress progress) throws IOException {
        extractWith(files, stage -> {
            unpack(input, stage, progress);
            validate(stage, machine, allowAssetsOnly, progress);
        });
    }

    static void extractWith(File files, Extractor extractor) throws IOException {
        Files.createDirectories(files.toPath());
        // Also prevents a new setup Activity from deleting a previous worker's staging directory.
        try (FileChannel channel = FileChannel.open(new File(files, ".game-import.lock").toPath(),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE)) {
            FileLock lock;
            try {
                lock = channel.tryLock();
            } catch (java.nio.channels.OverlappingFileLockException e) {
                throw new IOException("An import is already running. Please wait and try again.", e);
            }
            if (lock == null) throw new IOException("An import is already running. Please wait and try again.");
            try (FileLock held = lock) {
                File target = new File(files, "game");
                if (target.exists()) throw new IOException("The game folder already exists; it has not been overwritten.");
                Path stage = new File(files, ".game-import").toPath();
                deleteTree(stage);
                Files.createDirectory(stage);
                Throwable failure = null;
                try {
                    extractor.extract(stage);
                    interrupted();
                    // One rename publishes both assets and code; an interrupted import is never considered installed.
                    try {
                        Files.move(stage, target.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE);
                    } catch (java.nio.file.AtomicMoveNotSupportedException e) {
                        Files.move(stage, target.toPath());
                    }
                } catch (IOException | RuntimeException | Error e) {
                    failure = e;
                    throw e;
                } finally {
                    try { deleteTree(stage); }
                    catch (IOException cleanup) {
                        if (failure != null) failure.addSuppressed(cleanup);
                        else throw cleanup;
                    }
                }
            }
        }
    }

    private static void unpack(InputStream input, Path stage, Progress progress) throws IOException {
        Set<String> names = new HashSet<>();
        byte[] buffer = new byte[256 * 1024];
        long total = 0, spaceCheck = 0, entryWritten = 0;
        int entryPercent = -1;
        int entries = 0;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(input, buffer.length))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                interrupted();
                if (++entries > MAX_ENTRIES) throw new IOException("ZIP has too many entries.");
                String name = entry.getName();
                if (name.endsWith("/")) name = name.substring(0, name.length() - 1);
                validateName(name, entry.isDirectory());
                if (!names.add(name.toLowerCase(Locale.ROOT))) throw new IOException("Duplicate ZIP path: " + name);
                Path path = stage.resolve(name).normalize();
                if (!path.startsWith(stage)) throw new IOException("Unsafe ZIP path: " + name);
                if (entry.isDirectory()) {
                    if (zip.read() != -1) throw new IOException("ZIP directory contains file data: " + name);
                    Files.createDirectories(path);
                } else {
                    entryWritten = 0;
                    entryPercent = -1;
                    if (entry.getSize() > MAX_BYTES - total) throw new IOException("Game ZIP exceeds the 16 GiB limit.");
                    Files.createDirectories(path.getParent());
                    try (java.io.OutputStream output = Files.newOutputStream(path, StandardOpenOption.CREATE_NEW)) {
                        int count;
                        while ((count = zip.read(buffer)) != -1) {
                            interrupted();
                            if (count > MAX_BYTES - total) throw new IOException("Game ZIP exceeds the 16 GiB limit.");
                            if (total >= spaceCheck) {
                                if (stage.toFile().getUsableSpace() < SPACE_RESERVE + count)
                                    throw new IOException("Not enough free storage. Free space and import the ZIP again.");
                                spaceCheck = total + 1024 * 1024;
                                progress.update(name, total);
                            }
                            output.write(buffer, 0, count);
                            total += count;
                            entryWritten += count;
                            if (entry.getSize() > 0) {
                                int percent = (int)Math.min(100, entryWritten * 100 / entry.getSize());
                                if (percent != entryPercent) {
                                    progress.updatePercent("Extracting " + name, percent);
                                    entryPercent = percent;
                                }
                            }
                        }
                    }
                }
                zip.closeEntry(); // Validates the entry CRC, including directories.
            }
        }
    }

    static void validate(Path stage, int machine, boolean allowAssetsOnly, Progress progress) throws IOException {
        byte[] buffer = new byte[256 * 1024];
        File assets = stage.resolve("assets").toFile();
        if (!hasAssets(assets) || !hasFiles(stage.resolve("assets/Default")) || !hasFiles(stage.resolve("assets/Media")))
            throw new IOException("ZIP must contain assets/default.xbe, assets/Default/ and assets/Media/ with their files.");
        byte[] xbe = header(stage.resolve("assets/default.xbe"), 4);
        if (xbe[0] != 'X' || xbe[1] != 'B' || xbe[2] != 'E' || xbe[3] != 'H')
            throw new IOException("default.xbe is not an Xbox executable.");
        if (allowAssetsOnly && !Files.exists(stage.resolve("libscda_code.so"))) {
            try {
                java.security.MessageDigest hash = java.security.MessageDigest.getInstance("SHA-1");
                try (InputStream xbeInput = Files.newInputStream(stage.resolve("assets/default.xbe"))) {
                    int count;
                    while ((count = xbeInput.read(buffer)) != -1) {
                        interrupted();
                        hash.update(buffer, 0, count);
                    }
                }
                StringBuilder hex = new StringBuilder();
                for (byte value : hash.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 255));
                if (!hex.toString().equals("91290baf93c2f6ae735a3106c48734665c826d84"))
                    throw new IOException("On-device compilation requires the supported unmodified USA default.xbe.");
            } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
            interrupted();
            progress.update("Assets imported; ready to compile", -1);
            return;
        }
        byte[] elf = header(stage.resolve("libscda_code.so"), 20);
        int type = (elf[16] & 255) | ((elf[17] & 255) << 8);
        int arch = (elf[18] & 255) | ((elf[19] & 255) << 8);
        if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F'
                || elf[4] != 2 || elf[5] != 1 || type != 3 || arch != machine)
            throw new IOException("libscda_code.so is not a matching 64-bit Android library. Use the matching player builder.");
        interrupted();
        progress.update("Import complete", -1);
    }

    private static void validateName(String name, boolean directory) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":") || name.indexOf('\0') >= 0)
            throw new IOException("Unsafe ZIP path: " + name);
        for (String part : name.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("Unsafe ZIP path: " + name);
        }
        boolean allowed = directory
            ? name.equals("assets") || name.equals("assets/Default") || name.startsWith("assets/Default/")
                || name.equals("assets/Media") || name.startsWith("assets/Media/")
            : name.equals("libscda_code.so") || name.equals("assets/default.xbe")
                || name.startsWith("assets/Default/") || name.startsWith("assets/Media/");
        if (!allowed) throw new IOException("Unexpected ZIP path: " + name
            + ". Put libscda_code.so and assets/ at the ZIP root; include campaign files only.");
    }

    private static boolean hasFiles(Path dir) throws IOException {
        try (java.util.stream.Stream<Path> paths = Files.walk(dir)) {
            return paths.anyMatch(Files::isRegularFile);
        }
    }

    private static byte[] header(Path file, int length) throws IOException {
        byte[] bytes = new byte[length];
        try (java.io.DataInputStream input = new java.io.DataInputStream(Files.newInputStream(file))) {
            input.readFully(bytes);
        } catch (IOException e) {
            throw new IOException("Missing or incomplete " + file.getFileName() + ".", e);
        }
        return bytes;
    }

    private static void interrupted() throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("Import cancelled. You can select the ZIP again to retry.");
    }

    private static void deleteTree(Path root) throws IOException {
        if (!Files.exists(root, java.nio.file.LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                Files.delete(file);
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(dir);
                return FileVisitResult.CONTINUE;
            }
        });
    }
}
