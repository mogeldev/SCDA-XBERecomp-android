package io.github.mogeldev.scda;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// Asks GitHub for the newest release: quietly at each app start and when the player taps "Check for updates".
// Nothing is downloaded or installed: a newer version opens its release page. Tools/android-builder/Builder.cs compares the same way.
final class UpdateCheck {
    static final String REPOSITORY = "mogeldev/SCDA-XBERecomp-android";
    private static final String API = "https://api.github.com/repos/" + REPOSITORY + "/releases?per_page=30";
    private static final Pattern TAG = Pattern.compile("\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
    // RELEASING.md versions: 0.1.0-testing.N, then 0.1.0-rc.N, then 0.1.0
    private static final Pattern VERSION = Pattern.compile("v?(\\d+)\\.(\\d+)\\.(\\d+)(?:-(testing|rc)\\.(\\d+))?");

    private UpdateCheck() {}

    static String page(String tag) { return "https://github.com/" + REPOSITORY + "/releases/tag/" + tag; }

    // major, minor, patch, stage (testing < rc < stable), number; null for any other name
    static long[] parse(String version) {
        Matcher m = VERSION.matcher(version == null ? "" : version.trim());
        if (!m.matches()) return null;
        int stage = m.group(4) == null ? 2 : m.group(4).equals("rc") ? 1 : 0;
        return new long[]{Long.parseLong(m.group(1)), Long.parseLong(m.group(2)), Long.parseLong(m.group(3)), stage,
            m.group(5) == null ? 0 : Long.parseLong(m.group(5))};
    }

    static int compare(long[] a, long[] b) {
        for (int i = 0; i < a.length; i++) if (a[i] != b[i]) return Long.compare(a[i], b[i]);
        return 0;
    }

    // The newest release tag in the GitHub answer, pre-releases included; null if none has a release version.
    static String newest(String json) {
        String best = null;
        long[] bestVersion = null;
        for (Matcher m = TAG.matcher(json); m.find();) {
            long[] version = parse(m.group(1));
            if (version != null && (bestVersion == null || compare(version, bestVersion) > 0)) {
                best = m.group(1);
                bestVersion = version;
            }
        }
        return best;
    }

    // true if tag is newer than this app's version; an unknown app version counts every release as newer
    static boolean isNewer(String tag, String current) {
        long[] mine = parse(current);
        return mine == null || compare(parse(tag), mine) > 0;
    }

    static String fetch(String current) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(API).openConnection();
        connection.setConnectTimeout(10000);
        connection.setReadTimeout(10000);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "SCDA-Android/" + current);
        try {
            int code = connection.getResponseCode();
            if (code != 200) throw new IOException("GitHub answered " + code);
            try (InputStream input = connection.getInputStream()) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[16384];
                for (int n; (n = input.read(buffer)) > 0;) {
                    if (bytes.size() + n > 4 << 20) throw new IOException("Release list too large");
                    bytes.write(buffer, 0, n);
                }
                return new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            connection.disconnect();
        }
    }
}
