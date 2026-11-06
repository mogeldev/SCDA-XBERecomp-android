package io.github.mogeldev.scda;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class GameSourceTest {
    private static int checks;
    private static void check(boolean value) { if (!value) throw new AssertionError(); checks++; }
    private static void game(Path root) throws IOException {
        Files.createDirectories(root.resolve("Default"));
        Files.createDirectories(root.resolve("Media"));
        Files.write(root.resolve("default.xbe"), new byte[]{'X','B','E','H'});
        Files.write(root.resolve("Default/data"), new byte[]{1});
        Files.write(root.resolve("Media/intro"), new byte[]{2});
        byte[] elf = new byte[64];
        elf[0]=127; elf[1]='E'; elf[2]='L'; elf[3]='F'; elf[4]=2; elf[5]=1; elf[16]=3; elf[18]=(byte)183;
        Files.write(root.resolve("libscda_code.so"), elf);
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("game source tests");
        try {
            for (String name : new String[]{"../bad", "/bad", "a//b", "a/./b", "a\\b", "a:b", "a\0b"}) {
                try { GameSource.safePath(name); throw new AssertionError(name); }
                catch (IOException expected) { checks++; }
            }
            GameSource.safePath("space folder/Default/data.bin"); checks++;
            Path file = root.resolve("input");
            try (java.io.RandomAccessFile out = new java.io.RandomAccessFile(file.toFile(), "rw")) {
                out.seek(0x18300000L + 0x10000); out.write("MICROSOFT*XBOX*MEDIA".getBytes("US-ASCII"));
            }
            check(GameSource.isIso(file));
            Files.write(file, new byte[]{'P','K',3,4}); check(!GameSource.isIso(file));
            Path files = root.resolve("valid");
            GameSource.importFile(new ByteArrayInputStream(new byte[]{1}), files.toFile(), 183,
                (tool, input, output, progress) -> {
                    check(tool.equals("archive"));
                    game(output.resolve("wrapper/assets"));
                    Files.write(output.resolve("readme.txt"), new byte[]{1});
                }, (name, bytes) -> {});
            check(GameImport.isInstalled(files.toFile()));
            check(!Files.exists(files.resolve("game/unpacked")));
            try (java.util.stream.Stream<Path> paths = Files.list(files.resolve("game"))) { check(paths.count() == 2); }
            for (String kind : new String[]{"ambiguous", "unsupported", "cancel", "failure"}) {
                Path bad = root.resolve(kind);
                try {
                    GameSource.importFile(new ByteArrayInputStream(new byte[]{1}), bad.toFile(), 183,
                        (tool, input, output, progress) -> {
                            game(output.resolve("one"));
                            if (kind.equals("ambiguous")) game(output.resolve("two"));
                            if (kind.equals("unsupported")) Files.delete(output.resolve("one/libscda_code.so"));
                            if (kind.equals("cancel")) Thread.currentThread().interrupt();
                            if (kind.equals("failure")) throw new IOException("tool failed");
                        }, (name, bytes) -> {});
                    throw new AssertionError(kind);
                } catch (IOException expected) {
                    Thread.interrupted();
                    check(!Files.exists(bad.resolve("game")));
                    check(!Files.exists(bad.resolve(".game-import")));
                }
            }
            System.out.println("PASS: " + checks + " source import checks");
        } finally { CompileCache.delete(root); }
    }
}
