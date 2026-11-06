package io.github.mogeldev.scda.compilerprobe;

public final class BuildJobsTest {
    private static void check(int expected, int cores, long availableMiB, long thresholdMiB, int thermal) {
        int actual = BuildJobs.choose(cores, availableMiB * BuildJobs.MIB, thresholdMiB * BuildJobs.MIB, thermal);
        if (actual != expected) throw new AssertionError("Expected " + expected + ", got " + actual);
    }

    public static void main(String[] args) {
        check(4, 8, 2933, 300, 0);
        check(7, 8, 8192, 300, 0);
        check(1, 8, 500, 300, 0);
        check(1, 1, 8192, 300, 0);
        check(3, 8, 8192, 300, 2);
        check(1, 8, 8192, 300, 3);
        check(1, 8, 8192, 300, 6);
        check(2, 8, 3072, 2048, 0);
        System.out.println("BuildJobs: 8 checks passed");
    }
}
