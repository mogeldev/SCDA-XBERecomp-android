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

public class OnDeviceSetupActivity extends SetupActivity {
    @Override protected boolean supportsLocalCompilation() {
        return Build.SUPPORTED_ABIS[0].equals("arm64-v8a")
            && new File(getApplicationInfo().nativeLibraryDir, "libprobe_clang.so").isFile();
    }

    @Override protected void importSource(android.net.Uri uri, File external, int machine, GameImport.Progress progress) throws IOException {
        if (!supportsLocalCompilation()) { super.importSource(uri, external, machine, progress); return; }
        if (android.provider.DocumentsContract.isTreeUri(uri)) {
            GameImport.extractWith(external, stage -> {
                Path unpacked = stage.resolve("unpacked");
                Files.createDirectory(unpacked);
                String id = android.provider.DocumentsContract.getTreeDocumentId(uri);
                copyTree(uri, id, unpacked, progress, new long[2], 0);
                GameSource.normalize(stage, unpacked, machine, this::extractTool, progress, false);
            });
        } else {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("Cannot open selected input");
                GameSource.importFile(input, external, machine, this::extractTool, progress);
            }
        }
    }

    private void copyTree(android.net.Uri tree, String id, Path output, GameImport.Progress progress, long[] totals, int depth) throws IOException {
        if (depth > 64) throw new IOException("Folder nesting exceeds 64");
        android.net.Uri children = android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] columns = {android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME, android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (android.database.Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot read selected folder");
            while (cursor.moveToNext()) {
                GameSource.interrupted();
                if (++totals[0] > 100000) throw new IOException("Too many folder entries");
                String childId = cursor.getString(0), name = cursor.getString(1), mime = cursor.getString(2);
                if (name == null || name.contains("/")) throw new IOException("Invalid folder filename");
                GameSource.safePath(name);
                Path child = output.resolve(name);
                if (android.provider.DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    Files.createDirectory(child);
                    copyTree(tree, childId, child, progress, totals, depth + 1);
                } else {
                    try (InputStream input = getContentResolver().openInputStream(
                            android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, childId))) {
                        if (input == null) throw new IOException("Cannot open folder file: " + name);
                        GameSource.copy(input, child, (message, bytes) -> {
                            if (bytes > GameSource.MAX_BYTES - totals[1]) throw new IOException("Folder exceeds 16 GiB");
                            progress.update(name, totals[1] + bytes);
                        });
                        long size = Files.size(child);
                        if (size > GameSource.MAX_BYTES - totals[1]) throw new IOException("Folder exceeds 16 GiB");
                        totals[1] += size;
                    }
                }
            }
        }
    }

    private void extractTool(String tool, Path input, Path output, GameImport.Progress progress) throws IOException {
        ExtractTools.extract(this, tool, input, output, progress);
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
        command(work, "clang", "-cc1", "-triple", "aarch64-linux-android29", "-emit-obj", "-std=c++20", "-O2",
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
                while ((count = in.read(buffer)) != -1) { CompileCache.interrupted(); sha1.update(buffer, 0, count); }
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
                // Gradle builds the runtime independently of tool staging. Include actual packaged bytes.
                Path nativeDir = new File(getApplicationInfo().nativeLibraryDir).toPath();
                StringBuilder runtimeId = new StringBuilder(buildId);
                for (String name : new String[]{"libscda_host.so", "libSDL3.so", "libc++_shared.so", "libmain.so"})
                    runtimeId.append(CompileCache.hash(nativeDir.resolve(name)));
                CompileCache.reset(work, runtimeId.toString() + CompileCache.hash(xbe));
                if (work.toFile().getUsableSpace() < 512L * 1024 * 1024 || external.getUsableSpace() < 64L * 1024 * 1024)
                    throw new IOException("Free at least 512 MiB for compilation and 64 MiB for the library");
                progress.update("Preparing compiler inputs…", -1);
                copyAssets("compiler/input", work.resolve("input"));
                copyAssets("compiler/libunwind.a", work.resolve("libunwind.a"));
                Files.copy(xbe, work.resolve("input/default.xbe"), StandardCopyOption.REPLACE_EXISTING);
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
                        String compileStatus = "Compiling " + done + "/129 (" + reused + " reused), " + count + " workers";
                        progress.updatePercent(compileStatus, done * 100 / 129);
                        List<Callable<Void>> batch = new ArrayList<>();
                        for (int n = 0; n < count && next < pending.size(); n++) {
                            String source = pending.get(next++);
                            batch.add(() -> { compile(work, source); return null; });
                        }
                        for (java.util.concurrent.Future<Void> result : workers.invokeAll(batch)) result.get();
                        done += batch.size();
                        progress.updatePercent("Compiled " + done + "/129 objects", done * 100 / 129);
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
                progress.updatePercent("All 129 objects compiled", 100);
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
                    nativeDir.resolve("libscda_host.so").toString(), "@objects.rsp", "libunwind.a", "-o", library.toString());
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
