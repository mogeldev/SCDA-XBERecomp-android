package io.github.mogeldev.scda;

import java.nio.file.Files;
import java.nio.file.Path;

public final class CompileCacheTest {
    private static int checks;
    private static void check(boolean value) {
        if (!value) throw new AssertionError("Check " + checks);
        checks++;
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("scda compile-cache test");
        try {
            CompileCache.reset(root, "version1");
            Path object = root.resolve("partition.o");
            Files.write(object, new byte[]{1, 2, 3});
            check(!CompileCache.valid(object));
            CompileCache.seal(object);
            check(CompileCache.valid(object));
            CompileCache.reset(root, "version1");
            check(CompileCache.valid(object));
            Files.write(object, new byte[]{3, 2, 1});
            check(!CompileCache.valid(object));
            CompileCache.seal(object);
            check(CompileCache.valid(object));
            CompileCache.reset(root, "version2");
            check(!Files.exists(object));
            check(CompileCache.text(root.resolve("version")).equals("version2"));
            Files.write(root.resolve("partial.tmp"), new byte[]{1});
            check(!CompileCache.valid(root.resolve("partial")));
            Files.write(root.resolve("partial.sha256"), new byte[]{1});
            check(!CompileCache.valid(root.resolve("partial")));
            Thread.currentThread().interrupt();
            try { CompileCache.interrupted(); throw new AssertionError("Ignored cancellation"); }
            catch (java.io.IOException expected) { check(Thread.interrupted()); }
            System.out.println("PASS: " + checks + " cache/recovery checks");
        } finally { CompileCache.delete(root); }
    }
}
