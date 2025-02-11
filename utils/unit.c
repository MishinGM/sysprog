#include "unit.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
//лол

const char *current_test_name = "unknown";

void unit_test_start(void) {

    printf("-------- %s started --------\n", current_test_name);
}

void unit_test_finish(void) {
  
    printf("-------- %s done --------\n", current_test_name);
}

void unit_msg(const char *msg) {
  
    printf("# %s\n", msg);
}

void unit_check(bool cond, const char *format, ...) {
    if (!cond) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
        fprintf(stderr, "\n");
        exit(1);
    }
}

void unit_assert(bool cond) {
    if (!cond) {
        fprintf(stderr, "Assertion failed\n");
        exit(1);
    }
}

int doCmdMaxPoints(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}
