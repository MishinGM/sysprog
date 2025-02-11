#ifndef UNIT_H
#define UNIT_H

#include <stdbool.h>
#include <stdio.h>

extern const char *current_test_name;

void unit_test_start(void);
void unit_test_finish(void);
void unit_msg(const char *msg);
void unit_check(bool cond, const char *format, ...);
void unit_assert(bool cond);

int doCmdMaxPoints(int argc, char **argv);

#endif /* UNIT_H */
