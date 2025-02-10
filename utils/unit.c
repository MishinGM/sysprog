#include "unit.h"
#include <stdarg.h>
#include <stdlib.h>

void
unit_test_start(void) {
}

void
unit_test_finish(void) {
}

void
unit_msg(const char *msg) {
    printf("[msg] %s\n", msg);
}

void
unit_check(bool cond, const char *format, ...) {
    if (!cond) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
        fprintf(stderr, "\n");
        exit(1);
    }
}

void
unit_assert(bool cond) {
    if (!cond) {
        fprintf(stderr, "Assertion failed\n");
        exit(1);
    }
}

int
doCmdMaxPoints(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}
