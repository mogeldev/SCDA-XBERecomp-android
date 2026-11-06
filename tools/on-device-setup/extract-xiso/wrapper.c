// Android extraction-only entry. Upstream code is copyright in@fishtank.com; see extract-xiso.txt.
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <unistd.h>

static unsigned scda_depth, scda_entries;
static uint64_t scda_total;
static void scda_fail(const char *message) {
    fprintf(stderr, "ISO import: %s\n", message);
    exit(1);
}
static void scda_name(const char *name, size_t length) {
    if (!length || strlen(name) != length || !strcmp(name, ".") || !strcmp(name, "..")
            || strpbrk(name, "/\\:")) scda_fail("Unsafe ISO filename");
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] == 127) scda_fail("Unsafe ISO filename");
}
static void scda_entry(int fd) {
    struct stat st;
    off_t position = lseek(fd, 0, SEEK_CUR);
    if (++scda_entries > 100000 || fstat(fd, &st) || position < 0 || position > st.st_size - 14)
        scda_fail("Invalid ISO directory or too many entries");
}
static void scda_extent(int fd, off_t position, uint64_t size) {
    struct stat st;
    if (fstat(fd, &st) || position < 0 || (uint64_t)position > (uint64_t)st.st_size
            || size > (uint64_t)st.st_size - position || size > (16ULL << 30) - scda_total)
        scda_fail("Invalid ISO file extent or extraction exceeds 16 GiB");
    scda_total += size;
}
static void scda_space(size_t size) {
    struct statvfs st;
    if (statvfs(".", &st) || (uint64_t)st.f_bavail * st.f_frsize < (32ULL << 20) + size)
        scda_fail("Not enough free storage");
}
#include "extract-xiso.c"

int main(int argc, char **argv) {
    if (argc != 3) scda_fail("Expected input ISO and empty extraction directory");
    struct rlimit cpu = {900, 900};
    setrlimit(RLIMIT_CPU, &cpu);
    s_quiet = true;
    s_media_enable = false;
    s_copy_buffer = malloc(READWRITE_BUFFER_SIZE);
    if (!s_copy_buffer) scda_fail("Out of memory");
    // No create/rewrite CLI can ever modify the source image.
    int result = decode_xiso(argv[1], argv[2], k_extract, NULL, false);
    free(s_copy_buffer);
    if (!result) puts("XISO_EXTRACT_PASS");
    return result ? 1 : 0;
}
