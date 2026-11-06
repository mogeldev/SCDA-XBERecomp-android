package io.github.mogeldev.scda.compilerprobe;

final class BuildJobs {
    static final long MIB = 1024L * 1024;

    static int choose(int cores, long available, long lowMemoryThreshold, int thermalStatus) {
        int cpu = Math.max(1, cores - 1);
        // 512 MiB per compiler leaves headroom above measured ~250 MiB partition peaks.
        long reserve = Math.max(768 * MIB, lowMemoryThreshold);
        int memory = (int)Math.max(1, Math.min(cpu, (available - reserve) / (512 * MIB)));
        if (thermalStatus >= 3) return 1; // Android THERMAL_STATUS_SEVERE
        if (thermalStatus >= 2) cpu = Math.max(1, cpu / 2);
        return Math.min(cpu, memory);
    }
}
