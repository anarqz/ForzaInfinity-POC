/* Runs azimuth_ini.c's azimuth_prepare_ini on the folder given on the command line, as the hook
   does at startup. Built for the host by tests/ini_merge_test.py, which sets the INI up first and
   checks it after. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "adapters/azimuth_ini.c"

void log_append(const char *message) {
    fputs(message, stdout);
}
size_t text_length(const char *text) {
    return strlen(text);
}
void memory_copy(void *destination, const void *source, size_t size) {
    memmove(destination, source, size);
}
int memory_equal(const void *left, const void *right, size_t size) {
    return !memcmp(left, right, size);
}
int path_join(wchar_t output[PATH_CAPACITY], const wchar_t *directory, const wchar_t *name) {
    return swprintf(output, PATH_CAPACITY, L"%ls\\%ls", directory, name) > 0;
}

int wmain(int count, wchar_t **arguments) {
    if (count != 2) {
        return 2;
    }
    azimuth_prepare_ini(arguments[1]);
    azimuth_log_ini();
    return 0;
}
