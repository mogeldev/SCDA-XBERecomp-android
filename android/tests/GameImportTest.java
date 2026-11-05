package io.github.mogeldev.scda;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// Run using the commands in android/tests/README.md. No game assets or test dependencies needed.
public final class GameImportTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static Map<String, byte[]> game(int machine) {
        Map<String, byte[]> entries = new LinkedHashMap<>();
        entries.put("assets/", new byte[0]);
        entries.put("assets/Default/", new byte[0]);
        entries.put("assets/default.xbe", new byte[]{'X', 'B', 'E', 'H'});
        entries.put("assets/Default/data.bin", new byte[]{1, 2, 3});
        entries.put("assets/Media/intro.bin", new byte[]{4, 5, 6});
        byte[] elf = new byte[64];
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[16] = 3; elf[18] = (byte) machine;
        entries.put("libscda_code.so", elf);
        return entries;
    }

    private static byte[] zip(Map<String, byte[]> entries) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (Map.Entry<String, byte[]> item : entries.entrySet()) {
                ZipEntry entry = new ZipEntry(item.getKey());
                // Stored entries make deliberate CRC corruption deterministic.
                entry.setMethod(ZipEntry.STORED);
                entry.setSize(item.getValue().length);
                CRC32 crc = new CRC32();
                crc.update(item.getValue());
                entry.setCrc(crc.getValue());
                zip.putNextEntry(entry);
                zip.write(item.getValue());
                zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    private static void extract(byte[] zip, Path files, GameImport.Progress progress) throws IOException {
        GameImport.extract(new ByteArrayInputStream(zip), files.toFile(), 183, progress);
    }

    private static void rejected(byte[] zip, Path parent, String label) throws IOException {
        Path files = Files.createDirectory(parent.resolve("bad-" + checks));
        Files.writeString(files.resolve("scda.toml"), "settings");
        Files.createDirectories(files.resolve("hdd"));
        Files.writeString(files.resolve("hdd/save"), "save");
        try {
            extract(zip, files, (name, bytes) -> {});
            throw new AssertionError("Accepted " + label);
        } catch (IOException expected) {
            check(!GameImport.isInstalled(files.toFile()), "Published " + label);
            check(!Files.exists(files.resolve("game")), "Left target after " + label);
            check(!Files.exists(files.resolve(".game-import")), "Left staging after " + label);
            check(Files.readString(files.resolve("scda.toml")).equals("settings"), "Changed settings");
            check(Files.readString(files.resolve("hdd/save")).equals("save"), "Changed saves");
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("scda ZIP tests ");
        try {
            byte[] valid = zip(game(183));
            Path files = Files.createDirectory(root.resolve("valid"));
            Files.createDirectories(files.resolve(".game-import"));
            Files.writeString(files.resolve(".game-import/interrupted"), "old");
            extract(valid, files, (name, bytes) -> {});
            check(GameImport.isInstalled(files.toFile()), "Valid ZIP not installed");
            check(Arrays.equals(Files.readAllBytes(files.resolve("game/assets/Default/data.bin")), new byte[]{1, 2, 3}), "Wrong data");
            check(!Files.exists(files.resolve(".game-import")), "Staging remains");
            try {
                extract(valid, files, (name, bytes) -> {});
                throw new AssertionError("Overwrote existing game");
            } catch (IOException expected) {
                check(GameImport.isInstalled(files.toFile()), "Existing game damaged");
            }
            for (String path : new String[]{"../outside", "/outside", "assets/../scda.toml", "assets/Default/../../hdd/save",
                    "assets/Default/./file", "assets/Default//file", "assets\\Default\\file", "C:/outside", "hdd/save",
                    "wrapper/assets/default.xbe", "assets/adv/file", "assets/README.txt", "assets/Default/DATA.BIN"}) {
                Map<String, byte[]> entries = game(183);
                entries.put(path, new byte[]{1});
                rejected(zip(entries), root, path);
            }
            for (String missing : new String[]{"libscda_code.so", "assets/default.xbe", "assets/Default/data.bin", "assets/Media/intro.bin"}) {
                Map<String, byte[]> entries = game(183);
                entries.remove(missing);
                rejected(zip(entries), root, "missing " + missing);
            }
            Map<String, byte[]> badXbe = game(183);
            badXbe.put("assets/default.xbe", new byte[]{0, 0, 0, 0});
            rejected(zip(badXbe), root, "non-XBE");
            Map<String, byte[]> badElf = game(183);
            badElf.put("libscda_code.so", new byte[64]);
            rejected(zip(badElf), root, "non-ELF");
            rejected(zip(game(62)), root, "wrong ABI");
            rejected(new byte[]{1, 2, 3}, root, "non-ZIP");
            rejected(Arrays.copyOf(valid, 40), root, "truncated ZIP entry");
            byte[] corrupt = valid.clone();
            for (int i = 0; i < corrupt.length - 3; i++) {
                if (corrupt[i] == 'X' && corrupt[i + 1] == 'B' && corrupt[i + 2] == 'E' && corrupt[i + 3] == 'H') {
                    corrupt[i] = 'Y';
                    break;
                }
            }
            rejected(corrupt, root, "CRC error");
            Map<String, byte[]> directoryData = game(183);
            directoryData.put("assets/Default/", new byte[]{1});
            rejected(zip(directoryData), root, "directory payload");
            Path cancelled = Files.createDirectory(root.resolve("cancelled"));
            try {
                extract(valid, cancelled, (name, bytes) -> Thread.currentThread().interrupt());
                throw new AssertionError("Ignored cancellation");
            } catch (IOException expected) {
                Thread.interrupted();
                check(!Files.exists(cancelled.resolve("game")), "Published cancelled import");
                check(!Files.exists(cancelled.resolve(".game-import")), "Cancelled staging remains");
            }
            // Reject a concurrent import without clearing its staging data.
            Path locked = Files.createDirectory(root.resolve("locked"));
            try (java.nio.channels.FileChannel channel = java.nio.channels.FileChannel.open(locked.resolve(".game-import.lock"),
                    java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.WRITE);
                    java.nio.channels.FileLock lock = channel.lock()) {
                try {
                    extract(valid, locked, (name, bytes) -> {});
                    throw new AssertionError("Ignored import lock");
                } catch (IOException expected) {
                    check(expected.getMessage().contains("already running"), "Wrong lock error");
                }
            }
            Path x64 = Files.createDirectory(root.resolve("x64"));
            GameImport.extract(new ByteArrayInputStream(zip(game(62))), x64.toFile(), 62, (name, bytes) -> {});
            check(GameImport.isInstalled(x64.toFile()), "x86_64 emulator import failed");
            if (args.length == 1) {
                Path actual = Files.createDirectory(root.resolve("actual game"));
                try (java.io.InputStream input = Files.newInputStream(Path.of(args[0]))) {
                    GameImport.extract(input, actual.toFile(), 183, (name, bytes) -> {});
                }
                check(GameImport.isInstalled(actual.toFile()), "Actual game ZIP not installed");
                System.out.println("PASS: full game ZIP extracted and validated");
            }
            System.out.println("PASS: " + checks + " ZIP import checks");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : paths.sorted(java.util.Comparator.reverseOrder()).toArray(Path[]::new)) Files.delete(path);
            }
        }
    }
}
