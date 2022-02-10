// Test __sanitizer_set_report_path and __sanitizer_get_report_path:
// RUN: rm -rf %t.report_path
// RUN: %clangxx -O2 %s -o %t
// RUN: %run %t | FileCheck %s
// Try again with a directory without write access.
// RUN: rm -rf %t.report_path && mkdir -p %t.report_path
// RUN: chmod u-w %t.report_path || true
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=FAIL

#include <assert.h>
#include <sanitizer/common_interface_defs.h>
#include <stdio.h>
#include <string.h>

volatile int *null = 0;

int main(int argc, char **argv) {
  char buff[1000];
  sprintf(buff, "%s.report_path/report", argv[0]);
  __sanitizer_set_report_path(buff);
  assert(strncmp(buff, __sanitizer_get_report_path(), strlen(buff)) == 0);
  printf("Path %s\n", __sanitizer_get_report_path());
}

// CHECK: Path {{.*}}Posix/Output/sanitizer_set_report_path_test.cpp.tmp.report_path/report.
// FAIL: ERROR: Can't open file: {{.*}}Posix/Output/sanitizer_set_report_path_test.cpp.tmp.report_path/report.
