package io.github.mogeldev.scda;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

// Format-independent import transaction, also tested without Android.
final class GameSource {
    static final long MAX_BYTES = 16L << 30;
    interface Tool { void extract(String tool, Path input, Path output, GameImport.Progress progress) throws IOException; }

    static void copy(InputStream input, Path output, GameImport.Progress progress) throws IOException {
        try (OutputStream out = Files.newOutputStream(output, StandardOpenOption.CREATE_NEW)) {
            byte[] buffer = new byte[256 * 1024];
            long total = 0, next = 0;
            int count;
            while ((count = input.read(buffer)) != -1) {
                interrupted();
                if (count > MAX_BYTES - total) throw new IOException("Input exceeds 16 GiB");
                if (output.toFile().getUsableSpace() < (32L << 20) + count) throw new IOException("Not enough free storage");
                out.write(buffer, 0, count);
                total += count;
                if (total >= next) { progress.update("Copying selected input", total); next = total + (4L << 20); }
            }
        }
    }

    static boolean isIso(Path path) throws IOException {
        long[] offsets = {0, 0xFD90000L, 0x2080000L, 0x18300000L};
        byte[] expected = "MICROSOFT*XBOX*MEDIA".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        try (java.io.RandomAccessFile input = new java.io.RandomAccessFile(path.toFile(), "r")) {
            byte[] bytes = new byte[expected.length];
            for (long offset : offsets) {
                if (input.length() < offset + 0x10000 + bytes.length) continue;
                input.seek(offset + 0x10000);
                input.readFully(bytes);
                if (java.util.Arrays.equals(bytes, expected)) return true;
            }
        }
        return false;
    }

    static void importFile(InputStream input, File files, int machine, Tool tool, GameImport.Progress progress) throws IOException {
        GameImport.extractWith(files, stage -> {
            Path source = stage.resolve("source.input");
            copy(input, source, progress);
            Path unpacked = stage.resolve("unpacked");
            Files.createDirectory(unpacked);
            boolean iso = isIso(source);
            tool.extract(iso ? "xiso" : "archive", source, unpacked, progress);
            Files.delete(source);
            normalize(stage, unpacked, machine, tool, progress, !iso);
        });
    }

    static void normalize(Path stage, Path unpacked, int machine, Tool tool, GameImport.Progress progress, boolean allowImage) throws IOException {
        interrupted();
        List<Path> roots = new ArrayList<>(), images = new ArrayList<>();
        Set<String> names = new HashSet<>();
        long total = 0;
        int entries = 0;
        try (java.util.stream.Stream<Path> walk = Files.walk(unpacked)) {
            for (java.util.Iterator<Path> paths = walk.iterator(); paths.hasNext();) {
                interrupted();
                Path path = paths.next();
                if (path.equals(unpacked)) continue;
                String relative = unpacked.relativize(path).toString().replace(File.separatorChar, '/');
                if (++entries > 100000 || unpacked.relativize(path).getNameCount() > 64)
                    throw new IOException("Too many entries or nested directories");
                safePath(relative);
                if (!names.add(relative.toLowerCase(Locale.ROOT))) throw new IOException("Duplicate case-insensitive path: " + relative);
                if (Files.isSymbolicLink(path)) throw new IOException("Links are not allowed");
                if (Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) continue;
                if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Unexpected input file type");
                long size = Files.size(path);
                if (size > MAX_BYTES - total) throw new IOException("Extracted input exceeds 16 GiB");
                total += size;
                if (path.getFileName().toString().equals("default.xbe") && GameImport.hasAssets(path.getParent().toFile()))
                    roots.add(path.getParent());
                String name = path.getFileName().toString().toLowerCase(Locale.ROOT);
                if (name.endsWith(".iso") || name.endsWith(".xiso")) images.add(path);
            }
        }
        if (roots.isEmpty() && allowImage && images.size() == 1) {
            Path image = images.get(0);
            if (!isIso(image)) throw new IOException("Archive does not contain an Xbox ISO");
            Path disc = stage.resolve("disc");
            Files.createDirectory(disc);
            tool.extract("xiso", image, disc, progress);
            CompileCache.delete(unpacked);
            normalize(stage, disc, machine, tool, progress, false);
            return;
        }
        if (roots.size() != 1 || (allowImage && !images.isEmpty()))
            throw new IOException("Select one Xbox ISO or one game folder/archive containing default.xbe, Default/ and Media/");
        Path root = roots.get(0);
        Path assets = stage.resolve("assets");
        Files.createDirectory(assets);
        for (String name : new String[]{"default.xbe", "Default", "Media"}) Files.move(root.resolve(name), assets.resolve(name));
        Path library = root.resolve("libscda_code.so");
        if (!Files.isRegularFile(library) && root.getFileName().toString().equals("assets"))
            library = root.getParent().resolve("libscda_code.so");
        if (Files.isRegularFile(library)) {
            Files.move(library, stage.resolve("libscda_code.so"));
            Path version = library.getParent().resolve(GameImport.VERSION_FILE);
            if (Files.isRegularFile(version, LinkOption.NOFOLLOW_LINKS)) Files.move(version, stage.resolve(GameImport.VERSION_FILE));
        }
        CompileCache.delete(unpacked);
        GameImport.validate(stage, machine, true, progress);
    }

    static void safePath(String name) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":")) throw new IOException("Unsafe input path");
        for (String part : name.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("Unsafe input path");
            for (int i = 0; i < part.length(); i++) if (part.charAt(i) < 32 || part.charAt(i) == 127) throw new IOException("Unsafe input path");
        }
    }

    static void interrupted() throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("Import cancelled; select the input again to retry");
    }
}
