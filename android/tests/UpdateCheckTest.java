package io.github.mogeldev.scda;

// Plain-Java checks for the release comparison; no network.
public final class UpdateCheckTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        check(UpdateCheck.isNewer("v0.1.0-testing.3", "0.1.0-testing.2"), "testing.3 > testing.2");
        check(UpdateCheck.isNewer("v0.1.0-testing.10", "0.1.0-testing.9"), "numeric, not text order");
        check(UpdateCheck.isNewer("v0.1.0-rc.1", "0.1.0-testing.9"), "rc > testing");
        check(UpdateCheck.isNewer("v0.1.0", "0.1.0-rc.4"), "stable > rc");
        check(UpdateCheck.isNewer("v0.2.0-testing.1", "0.1.0"), "next minor");
        check(!UpdateCheck.isNewer("v0.1.0-testing.2", "0.1.0-testing.2"), "same version");
        check(!UpdateCheck.isNewer("v0.1.0-testing.1", "0.1.0-testing.2"), "older version");
        String json = "[{\"tag_name\": \"v0.1.0-testing.1\", \"prerelease\": true},"
            + "{\"tag_name\":\"update-check-test\"},{\"tag_name\":\"v0.1.0-testing.3\"},{\"tag_name\":\"V3\"}]";
        check("v0.1.0-testing.3".equals(UpdateCheck.newest(json)), "newest of a list");
        check(UpdateCheck.newest("[]") == null, "empty list");
        check(UpdateCheck.page("v0.1.0-testing.3").equals(
            "https://github.com/mogeldev/SCDA-XBERecomp-android/releases/tag/v0.1.0-testing.3"), "release page");
        System.out.println("PASS: update version comparison");
    }
}
