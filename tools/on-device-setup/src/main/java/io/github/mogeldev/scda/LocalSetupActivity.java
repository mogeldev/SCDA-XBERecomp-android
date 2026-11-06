package io.github.mogeldev.scda;

import android.app.ActivityManager;
import android.os.Build;
import android.os.PowerManager;
import android.util.Log;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;

public final class LocalSetupActivity extends SetupActivity {
    @Override protected boolean supportsLocalCompilation() { return true; }

    @Override protected void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        // Debug-only adb entry uses the exact picker/import pipeline, never a prebuilt code transfer.
        File external = getExternalFilesDir(null);
        if (state == null && external != null && !new File(external, "game").exists()
                && getIntent().getBooleanExtra("test_import", false)
                && new File(getFilesDir(), "test-assets.zip").isFile()) {
            importZip(android.net.Uri.fromFile(new File(getFilesDir(), "test-assets.zip")));
        }
    }

    private void command(Path work, String tool, String... args) throws IOException {
        CompileCache.interrupted();
        List<String> argv = new ArrayList<>();
        argv.add(new File(getApplicationInfo().nativeLibraryDir, "libprobe_" + tool + ".so").getPath());
        argv.addAll(Arrays.asList(args));
        Path diagnostic = Files.createTempFile(work, "command-", ".log");
        ProcessBuilder builder = new ProcessBuilder(argv).directory(work.toFile()).redirectErrorStream(true)
            .redirectOutput(diagnostic.toFile());
        builder.environment().put("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir);
        Process process = builder.start();
        try {
            // Packaged tools have $ORIGIN RUNPATH and a renamed C++ runtime, separate from the game's.
            if (!process.waitFor(300, TimeUnit.SECONDS)) throw new IOException(tool + " timed out");
            String text = CompileCache.text(diagnostic);
            Log.i("SCDABuild", tool + " exit=" + process.exitValue() + " " + text);
            if (process.exitValue() != 0) throw new IOException(tool + " failed: " + text.substring(0, Math.min(2000, text.length())));
            if (tool.equals("load") && !text.contains("\nLOAD_CHECK_PASS\n")) throw new IOException("Runtime check incomplete");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Cancelled; completed objects retained", e);
        } finally {
            // No /system/bin/time wrapper: this is the compiler itself, so cancellation kills the real child.
            process.destroyForcibly();
            boolean interrupted = Thread.interrupted();
            try { process.waitFor(); }
            catch (InterruptedException e) { interrupted = true; }
            if (interrupted) Thread.currentThread().interrupt();
            Files.deleteIfExists(diagnostic);
        }
    }

    private void copyAssets(String asset, Path target) throws IOException {
        String[] children = getAssets().list(asset);
        if (children == null) throw new IOException("Missing compiler inputs: " + asset);
        if (children.length != 0) {
            Files.createDirectories(target);
            for (String child : children) copyAssets(asset + "/" + child, target.resolve(child));
        } else {
            Files.createDirectories(target.getParent());
            try (InputStream input = getAssets().open(asset)) {
                Files.copy(input, target, StandardCopyOption.REPLACE_EXISTING);
            }
        }
    }

    private void elf(Path path, int type) throws IOException {
        byte[] bytes = new byte[64];
        try (java.io.DataInputStream input = new java.io.DataInputStream(Files.newInputStream(path))) { input.readFully(bytes); }
        if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F'
                || bytes[4] != 2 || bytes[5] != 1 || (bytes[16] & 255) != type || bytes[17] != 0
                || (bytes[18] & 255) != 183 || bytes[19] != 0) throw new IOException("Invalid ARM64 ELF: " + path);
    }

    private void compile(Path work, String source) throws IOException {
        Path output = work.resolve("obj/" + source + ".o");
        if (CompileCache.valid(output)) { elf(output, 1); return; }
        Files.deleteIfExists(output.resolveSibling(output.getFileName() + ".sha256"));
        Path temporary = output.resolveSibling(output.getFileName() + ".tmp");
        command(work, "clang", "-cc1", "-triple", "aarch64-linux-android30", "-emit-obj", "-std=c++20", "-O2",
            "-mrelocation-model", "pic", "-pic-level", "2", "-funwind-tables=2", "-relaxed-aliasing",
            "-fcxx-exceptions", "-fexceptions", "-stack-protector", "2", "-target-feature", "+neon",
            "-target-feature", "-outline-atomics", "-DX86_GAME_CODE", "-nostdsysteminc", "-nobuiltininc", "-w",
            "-Igenerated", "-Iinput", "-Iinput/cpu/freestanding", "-o", temporary.toString(), "generated/" + source);
        elf(temporary, 1);
        Files.move(temporary, output, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        CompileCache.seal(output);
    }

    @Override protected void compileGame(File external, GameImport.Progress progress) throws IOException {
        if (!Build.SUPPORTED_ABIS[0].equals("arm64-v8a")) throw new IOException("Local compilation currently requires ARM64");
        CompileCache.interrupted();
        Path game = external.toPath().resolve("game");
        Path xbe = game.resolve("assets/default.xbe");
        // Verify even on resume: no translator/compiler execution for a different XBE.
        try {
            java.security.MessageDigest sha1 = java.security.MessageDigest.getInstance("SHA-1");
            try (InputStream in = Files.newInputStream(xbe)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) != -1) sha1.update(buffer, 0, count);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha1.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            if (!hex.toString().equals("91290baf93c2f6ae735a3106c48734665c826d84")) throw new IOException("Unsupported default.xbe");
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
        Path work = getFilesDir().toPath().resolve("compile");
        Files.createDirectories(work);
        try (java.nio.channels.FileChannel channel = java.nio.channels.FileChannel.open(getFilesDir().toPath().resolve("compile.lock"),
                java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.WRITE)) {
            java.nio.channels.FileLock lock;
            try { lock = channel.tryLock(); }
            catch (java.nio.channels.OverlappingFileLockException e) { throw new IOException("Compilation already running", e); }
            if (lock == null) throw new IOException("Compilation already running");
            try (java.nio.channels.FileLock held = lock) {
                String buildId;
                try (java.io.DataInputStream in = new java.io.DataInputStream(getAssets().open("compiler/version"))) {
                    byte[] bytes = new byte[64];
                    in.readFully(bytes);
                    buildId = new String(bytes, StandardCharsets.UTF_8);
                    if (!buildId.matches("[a-f0-9]{64}")) throw new IOException("Invalid compiler version");
                }
                CompileCache.reset(work, buildId + CompileCache.hash(xbe));
                // Automated import ZIP is no longer needed after validated assets were published.
                Files.deleteIfExists(getFilesDir().toPath().resolve("test-assets.zip"));
                if (work.toFile().getUsableSpace() < 512L * 1024 * 1024 || external.getUsableSpace() < 64L * 1024 * 1024)
                    throw new IOException("Free at least 512 MiB for compilation and 64 MiB for the library");
                progress.update("Preparing compiler inputs…", -1);
                copyAssets("compiler/input", work.resolve("input"));
                Files.copy(xbe, work.resolve("input/default.xbe"), StandardCopyOption.REPLACE_EXISTING);
                Path nativeDir = new File(getApplicationInfo().nativeLibraryDir).toPath();
                Path generated = work.resolve("generated");
                Path translated = work.resolve("translated");
                if (!Files.isRegularFile(translated)) {
                    CompileCache.delete(work.resolve("obj"));
                    CompileCache.delete(generated);
                    progress.update("Translating Xbox executable…", -1);
                    command(work, "recomp", "input/config/scda_v2.toml", "input/default.xbe", "generated");
                    // Record every output hash so interrupted/corrupt generated input cannot reuse objects.
                    StringBuilder hashes = new StringBuilder();
                    try (java.util.stream.Stream<Path> paths = Files.list(generated)) {
                        for (Path path : paths.sorted().toArray(Path[]::new))
                            hashes.append(path.getFileName()).append(' ').append(CompileCache.hash(path)).append('\n');
                    }
                    CompileCache.atomicText(translated, hashes.toString());
                }
                List<String> translatedFiles = Files.readAllLines(translated);
                if (translatedFiles.size() != 134) {
                    Files.delete(translated);
                    CompileCache.delete(work.resolve("obj"));
                    throw new IOException("Incomplete translator output; tap resume to regenerate safely");
                }
                for (String line : translatedFiles) {
                    String[] entry = line.split(" ", 2);
                    if (entry.length != 2 || !entry[0].matches("[A-Za-z0-9_.]+")
                            || !Files.isRegularFile(generated.resolve(entry[0]))
                            || !CompileCache.hash(generated.resolve(entry[0])).equals(entry[1])) {
                        Files.delete(translated);
                        CompileCache.delete(work.resolve("obj"));
                        throw new IOException("Generated input changed; tap resume to regenerate safely");
                    }
                }
                Files.createDirectories(work.resolve("obj"));
                List<String> sources = new ArrayList<>();
                sources.add("x86_func_mapping.cpp");
                for (int i = 0; i < 128; i++) sources.add("x86_recomp." + i + ".cpp");
                int done = 0, reused = 0;
                List<String> pending = new ArrayList<>();
                for (String source : sources) {
                    if (CompileCache.valid(work.resolve("obj/" + source + ".o"))) { done++; reused++; }
                    else pending.add(source);
                }
                Log.i("SCDABuild", "Resume: " + reused + "/129 objects reused");
                ExecutorService workers = Executors.newFixedThreadPool(Math.max(1, Runtime.getRuntime().availableProcessors() - 1));
                try {
                    for (int next = 0; next < pending.size();) {
                        CompileCache.interrupted();
                        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
                        getSystemService(ActivityManager.class).getMemoryInfo(memory);
                        int count = BuildJobs.choose(Runtime.getRuntime().availableProcessors(), memory.availMem,
                            memory.threshold, getSystemService(PowerManager.class).getCurrentThermalStatus());
                        progress.update("Compiling " + done + "/129 (" + reused + " reused), " + count + " workers", -1);
                        List<Callable<Void>> batch = new ArrayList<>();
                        for (int n = 0; n < count && next < pending.size(); n++) {
                            String source = pending.get(next++);
                            batch.add(() -> { compile(work, source); return null; });
                        }
                        for (java.util.concurrent.Future<Void> result : workers.invokeAll(batch)) result.get();
                        done += batch.size();
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IOException("Cancelled; completed objects retained", e);
                } catch (java.util.concurrent.ExecutionException e) { throw new IOException("Compilation failed", e.getCause()); }
                finally {
                    workers.shutdownNow();
                    boolean interrupted = Thread.interrupted();
                    try { if (!workers.awaitTermination(310, TimeUnit.SECONDS)) throw new IOException("Compiler workers did not stop"); }
                    catch (InterruptedException e) { interrupted = true; }
                    if (interrupted) Thread.currentThread().interrupt();
                }
                CompileCache.interrupted();
                StringBuilder response = new StringBuilder();
                for (String source : sources) {
                    elf(work.resolve("obj/" + source + ".o"), 1);
                    response.append("obj/").append(source).append(".o\n");
                }
                CompileCache.atomicText(work.resolve("objects.rsp"), response.toString());
                progress.update("Linking 129 objects…", -1);
                Path library = work.resolve("libscda_code.so");
                command(work, "lld", "-flavor", "gnu", "-shared", "-soname", "libscda_code.so", "-z", "max-page-size=16384",
                    "-z", "now", "-z", "relro", "--hash-style=gnu", "--eh-frame-hdr", "--no-as-needed",
                    nativeDir.resolve("libscda_host.so").toString(), "@objects.rsp", "-o", library.toString());
                elf(library, 3);
                progress.update("Checking runtime symbols and dispatch tables…", -1);
                command(work, "load", library.toString());
                CompileCache.interrupted();
                Path staged = game.resolve("libscda_code.so.tmp");
                Files.copy(library, staged, StandardCopyOption.REPLACE_EXISTING);
                if (!CompileCache.hash(library).equals(CompileCache.hash(staged))) throw new IOException("Library copy hash mismatch");
                CompileCache.interrupted();
                Files.move(staged, game.resolve("libscda_code.so"), StandardCopyOption.ATOMIC_MOVE);
                Log.i("SCDABuild", "BUILD_PASS: published verified game library");
            }
        }
    }
}
