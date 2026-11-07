// Restricted libarchive reader: regular files/directories only, no links or metadata restoration.
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <unistd.h>

static void fail(const char *message) { fprintf(stderr, "Archive import: %s\n", message); exit(1); }
static void safe_name(char *name) {
    size_t length = strlen(name);
    if (length && name[length-1] == '/') name[--length] = 0;
    if (!length || length > 2048 || name[0] == '/' || strpbrk(name, "\\:")) fail("Unsafe archive path");
    unsigned depth = 0;
    for (char *part = name; part;) {
        char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && !strncmp(part, "..", 2)) || ++depth > 64)
            fail("Unsafe archive path or nesting exceeds 64");
        for (size_t i = 0; i < size; ++i)
            if ((unsigned char)part[i] < 32 || (unsigned char)part[i] == 127) fail("Unsafe archive path");
        part = end ? end + 1 : NULL;
    }
}
static void parents(char *name) {
    for (char *p = name; *p; ++p) if (*p == '/') {
        *p = 0;
        struct stat st;
        if (mkdir(name, 0700) && errno != EEXIST) fail("Cannot create archive directory");
        if (lstat(name, &st) || !S_ISDIR(st.st_mode)) fail("Archive path is not a directory");
        *p = '/';
    }
}
int main(int argc, char **argv) {
    if (argc != 3) fail("Expected archive and empty extraction directory");
    setlocale(LC_ALL, "C.UTF-8");
    struct rlimit cpu = {900, 900};
    setrlimit(RLIMIT_CPU, &cpu);
    // Android Scudo reserves multi-TiB virtual mappings; RLIMIT_AS would abort even small ZIPs.
    struct archive *a = archive_read_new();
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_lzma(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, argv[1], 256 * 1024) != ARCHIVE_OK) fail(archive_error_string(a));
    if (chdir(argv[2])) fail("Cannot open extraction directory");
    uint64_t total = 0;
    unsigned entries = 0;
    struct archive_entry *entry;
    int result;
    while ((result = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (++entries > 100000) fail("Too many archive entries");
        if (archive_entry_is_encrypted(entry)) fail("Encrypted archives are not supported");
        const char *path = archive_entry_pathname(entry);
        if (!path) fail("Missing archive path");
        while (!strncmp(path, "./", 2)) path += 2;
        if ((!*path || !strcmp(path, ".")) && archive_entry_filetype(entry) == AE_IFDIR) continue;
        char *name = strdup(path);
        if (!name) fail("Out of memory");
        safe_name(name);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry)) fail("Archive links are not allowed");
        parents(name);
        int type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            struct stat st;
            if (mkdir(name, 0700) && errno != EEXIST) fail("Cannot create archive directory");
            if (lstat(name, &st) || !S_ISDIR(st.st_mode)) fail("Invalid archive directory");
        } else if (type == AE_IFREG) {
            if (archive_entry_size(entry) < 0 || (uint64_t)archive_entry_size(entry) > (16ULL << 30) - total)
                fail("Archive exceeds 16 GiB");
            int fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (fd < 0) fail("Duplicate or invalid archive file");
            char buffer[256 * 1024];
            la_ssize_t count;
            while ((count = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
                struct statvfs st;
                if ((uint64_t)count > (16ULL << 30) - total) fail("Archive exceeds 16 GiB");
                if (statvfs(".", &st) || (uint64_t)st.f_bavail * st.f_frsize < (32ULL << 20) + count)
                    fail("Not enough free storage");
                if (write(fd, buffer, count) != count) fail("Writing extracted file failed");
                total += count;
            }
            if (count < 0) fail(archive_error_string(a));
            if (close(fd)) fail("Closing extracted file failed");
        } else fail("Only regular files and directories are allowed");
        free(name);
    }
    if (result != ARCHIVE_EOF) fail(archive_error_string(a));
    if (archive_read_has_encrypted_entries(a) > 0) fail("Encrypted archives are not supported");
    if (archive_read_free(a) != ARCHIVE_OK) fail("Archive validation failed");
    puts("ARCHIVE_EXTRACT_PASS");
    return 0;
}
