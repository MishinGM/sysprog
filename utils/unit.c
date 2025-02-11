
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
