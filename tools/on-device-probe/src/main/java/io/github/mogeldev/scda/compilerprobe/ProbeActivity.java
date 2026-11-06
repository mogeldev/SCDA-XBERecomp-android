package io.github.mogeldev.scda.compilerprobe;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.security.MessageDigest;
import java.util.Iterator;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Callable;
import android.app.ActivityManager;
import android.os.PowerManager;

public final class ProbeActivity extends Activity {
    static { System.loadLibrary("probe"); }
    private static native int checkLibrary(String path);
    private TextView output;
    private final StringBuilder report = new StringBuilder();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        output = new TextView(this);
        output.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(output);
        setContentView(scroll);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                runProbe();
                log("PASS");
            } catch (Exception | LinkageError e) {
                Log.e("CompilerProbe", "Probe failed", e);
                log("FAIL: " + e);
            } finally {
                try {
                    File temporary = new File(getFilesDir(), "result.tmp");
                    Files.write(temporary.toPath(), report.toString().getBytes(StandardCharsets.UTF_8));
                    Files.move(temporary.toPath(), new File(getFilesDir(), "result.txt").toPath(),
                            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                } catch (Exception e) { Log.e("CompilerProbe", "Saving report", e); }
            }
        }, "compiler-probe").start();
    }

    private synchronized void log(String text) {
        report.append(text).append('\n');
        Log.i("CompilerProbe", text);
        String snapshot = report.toString();
        runOnUiThread(() -> output.setText(snapshot));
    }

    private void command(String tool, String... args) throws Exception {
        List<String> argv = new ArrayList<>();
        // Toybox reports per-process peak RSS via wait4, including compiler startup.
        argv.add("/system/bin/time");
        argv.add("-v");
        argv.add(new File(getApplicationInfo().nativeLibraryDir, "libprobe_" + tool + ".so").getPath());
        argv.addAll(Arrays.asList(args));
        ProcessBuilder builder = new ProcessBuilder(argv).directory(getFilesDir()).redirectErrorStream(true);
        builder.environment().put("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir);
        File diagnostic = File.createTempFile("command-", ".log", getFilesDir());
        builder.redirectOutput(diagnostic);
        long start = System.nanoTime();
        Process process = builder.start();
        try {
            if (!process.waitFor(300, TimeUnit.SECONDS)) {
                throw new IllegalStateException(tool + " timed out after 300 seconds");
            }
            String text = new String(Files.readAllBytes(diagnostic.toPath()), StandardCharsets.UTF_8);
            log(tool + " exit=" + process.exitValue() + " time="
                    + ((System.nanoTime() - start) / 1_000_000) + "ms\n" + text);
            if (process.exitValue() != 0) throw new IllegalStateException(tool + " failed");
        } finally {
            if (process.isAlive()) process.destroyForcibly();
            Files.deleteIfExists(diagnostic.toPath());
        }
    }

    private void elf(String name, int type, int machine) throws Exception {
        File file = new File(getFilesDir(), name);
        byte[] bytes = new byte[64];
        try (java.io.DataInputStream in = new java.io.DataInputStream(new FileInputStream(file))) {
            in.readFully(bytes);
        }
        if (file.length() < 64 || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L'
                || bytes[3] != 'F' || bytes[4] != 2 || bytes[5] != 1
                || (bytes[16] & 255) != type || bytes[17] != 0
                || (bytes[18] & 255) != machine || bytes[19] != 0) {
            throw new IllegalStateException("Wrong ELF type/architecture: " + name);
        }
        log(name + ": ELF64 type=" + type + " machine=" + machine + " size=" + file.length());
    }

    private String digest(File file, String algorithm) throws Exception {
        MessageDigest hash = MessageDigest.getInstance(algorithm);
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = in.read(buffer)) != -1) hash.update(buffer, 0, count);
        }
        StringBuilder hex = new StringBuilder();
        for (byte value : hash.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return hex.toString();
    }

    private void translate() throws Exception {
        File xbe = new File(getFilesDir(), "input/default.xbe");
        if (!digest(xbe, "SHA-1").equals("91290baf93c2f6ae735a3106c48734665c826d84")) {
            throw new IllegalStateException("Unsupported default.xbe; translator not started");
        }
        // A fresh directory makes stale output incapable of passing the reference checks.
        File generated = new File(getFilesDir(), "generated");
        if (generated.exists()) {
            File[] files = generated.listFiles();
            if (files == null) throw new IllegalStateException("Cannot list previous translator output");
            for (File file : files) Files.delete(file.toPath());
            Files.delete(generated.toPath());
        }
        command("recomp", "input/config/scda_v2.toml", "input/default.xbe", "generated");
        JSONObject expected = new JSONObject(new String(Files.readAllBytes(
                new File(getFilesDir(), "input/expected.json").toPath()), StandardCharsets.UTF_8));
        if (expected.length() != 134) throw new IllegalStateException("Expected 134 translator reference hashes");
        Iterator<String> names = expected.keys();
        while (names.hasNext()) {
            String name = names.next();
            if (!name.matches("[A-Za-z0-9_.]+")) throw new IllegalStateException("Invalid reference filename");
            if (!digest(new File(generated, name), "SHA-256").equals(expected.getString(name))) {
                throw new IllegalStateException("Translator output differs from Windows reference: " + name);
            }
        }
        log("XBE translation: all 134 outputs match Windows SHA-256 references");
    }

    private void compileGame(String source, String object) throws Exception {
        command("clang", "-cc1", "-triple", "aarch64-linux-android30", "-emit-obj",
                "-std=c++20", "-O2", "-mrelocation-model", "pic", "-pic-level", "2",
                "-funwind-tables=2", "-relaxed-aliasing", "-fcxx-exceptions", "-fexceptions",
                "-stack-protector", "2", "-target-feature", "+neon",
                "-target-feature", "-outline-atomics", "-DX86_GAME_CODE",
                "-nostdsysteminc", "-nobuiltininc", "-w", "-Igenerated", "-Iinput", "-Iinput/cpu/freestanding",
                "-o", object, "generated/" + source);
        elf(object, 1, 183);
    }

    private void fullBuild() throws Exception {
        elf("input/libscda_host.so", 3, 183);
        Files.deleteIfExists(new File(getFilesDir(), "libscda_code.so").toPath());
        File objects = new File(getFilesDir(), "obj");
        if (!objects.isDirectory() && !objects.mkdir()) throw new IllegalStateException("Cannot create obj directory");
        long start = System.nanoTime();
        List<String> sources = new ArrayList<>();
        sources.add("x86_func_mapping.cpp");
        for (int i = 0; i < 128; i++) sources.add("x86_recomp." + i + ".cpp");
        StringBuilder response = new StringBuilder();
        ActivityManager activity = getSystemService(ActivityManager.class);
        PowerManager power = getSystemService(PowerManager.class);
        int cores = Runtime.getRuntime().availableProcessors();
        ExecutorService workers = Executors.newFixedThreadPool(Math.max(1, cores - 1));
        int next = 0, previousLimit = -1;
        try {
            while (next < sources.size()) {
                ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
                activity.getMemoryInfo(memory);
                int thermal = power.getCurrentThermalStatus();
                // Re-evaluate at batch boundaries when no compiler memory is still in flight.
                int limit = BuildJobs.choose(cores, memory.availMem, memory.threshold, thermal);
                if (limit != previousLimit) {
                    log("Build workers=" + limit + "; cores=" + cores + "; availableMiB="
                            + memory.availMem / BuildJobs.MIB + "; thermal=" + thermal);
                    previousLimit = limit;
                }
                List<Callable<String>> batch = new ArrayList<>();
                for (int count = 0; count < limit && next < sources.size(); count++) {
                    final String source = sources.get(next++);
                    log("Compiling " + next + "/129: " + source);
                    batch.add(() -> {
                        compileGame(source, "obj/" + source + ".o");
                        return source;
                    });
                }
                for (java.util.concurrent.Future<String> result : workers.invokeAll(batch)) result.get();
                log("Completed " + next + "/129");
            }
        } finally {
            // Let active bounded commands finish before publishing failure; never link incomplete output.
            workers.shutdown();
            if (!workers.awaitTermination(310, TimeUnit.SECONDS)) workers.shutdownNow();
        }
        for (String source : sources) response.append("obj/").append(source).append(".o\n");
        Files.write(new File(getFilesDir(), "objects.rsp").toPath(), response.toString().getBytes(StandardCharsets.UTF_8));
        command("lld", "-flavor", "gnu", "-shared", "-soname", "libscda_code.so",
                "-z", "max-page-size=16384", "-z", "now", "-z", "relro", "--hash-style=gnu",
                "--eh-frame-hdr", "--no-as-needed", "input/libscda_host.so", "@objects.rsp",
                "-o", "libscda_code.so");
        elf("libscda_code.so", 3, 183);
        log("Full build: 129 objects linked in " + ((System.nanoTime() - start) / 1_000_000) + "ms");
        log("Full library SHA-256: " + digest(new File(getFilesDir(), "libscda_code.so"), "SHA-256"));
        log("Link-only test: full game library has NOT been loaded or installed into the game");
    }

    private void runProbe() throws Exception {
        Files.deleteIfExists(new File(getFilesDir(), "result.txt").toPath());
        String abi = android.os.Build.SUPPORTED_ABIS[0];
        boolean arm64 = abi.equals("arm64-v8a");
        if (!arm64 && !abi.equals("x86_64")) throw new IllegalStateException("Unsupported ABI: " + abi);
        String triple = arm64 ? "aarch64-linux-android30" : "x86_64-linux-android30";
        int machine = arm64 ? 183 : 62;
        log("Android " + android.os.Build.VERSION.SDK_INT + "; ABI: " + abi
                + "; tools: " + getApplicationInfo().nativeLibraryDir);
        command("clang", "--version");
        // Invoke cc1 directly: no desktop sysroot, resource headers, driver subprocesses or CRT needed.
        Files.write(new File(getFilesDir(), "tiny.cpp").toPath(),
                "extern \"C\" int probe_answer(int value) { return value + 7; }\n".getBytes(StandardCharsets.UTF_8));
        command("clang", "-cc1", "-triple", triple, "-emit-obj", "-O2",
                "-mrelocation-model", "pic", "-o", "tiny.o", "tiny.cpp");
        elf("tiny.o", 1, machine);
        command("lld", "-flavor", "gnu", "-shared", "-z", "max-page-size=16384",
                "-z", "noexecstack", "-z", "now", "-z", "relro", "--no-undefined",
                "-soname", "libtiny.so", "tiny.o", "-o", "libtiny.so");
        elf("libtiny.so", 3, machine);
        int answer = checkLibrary(new File(getFilesDir(), "libtiny.so").getPath());
        if (answer != 42) throw new IllegalStateException("Loaded function returned " + answer);
        log("dlopen/dlsym/call: 42 (expected)");
        translate();
        if (getIntent().getBooleanExtra("full_build", false)) {
            if (!arm64) throw new IllegalStateException("Full build requires ARM64 device");
            fullBuild();
            return;
        }
        compileGame("x86_recomp.0.cpp", "game-arm64.o");
        // Partial game library: unresolved host/other-partition references are intentional; never load it.
        command("lld", "-flavor", "gnu", "-shared", "-z", "max-page-size=16384",
                "-z", "now", "-z", "relro", "--eh-frame-hdr", "-soname", "libpartial-game.so",
                "game-arm64.o", "-o", "libpartial-game.so");
        elf("libpartial-game.so", 3, 183);
    }
}
