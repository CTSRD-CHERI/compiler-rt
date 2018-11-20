// RUN: %clangxx -O0 -g %s -o %t && %run %t 2>&1 | FileCheck %s

// UNSUPPORTED: solaris

#include <stdio.h>

void print_something() {
  for (size_t i = 0; i < 10 * BUFSIZ; i++)
    printf("Hello world %zu\n", i);
}

// setbuffer/setlinebuf/setbuf uses setvbuf
// internally on NetBSD
#if defined(__NetBSD__)
void test_setbuf() {
  char buf[BUFSIZ];

  setbuf(stdout, NULL);

  print_something();

  setbuf(stdout, buf);

  print_something();
}

void test_setbuffer() {
  char buf[BUFSIZ];

  setbuffer(stdout, NULL, 0);

  print_something();

  setbuffer(stdout, buf, BUFSIZ);

  print_something();
}

void test_setlinebuf() {
  setlinebuf(stdout);

  print_something();
}
#endif

void test_setvbuf() {
  char buf[BUFSIZ];

  setvbuf(stdout, NULL, _IONBF, 0);

  print_something();

  setvbuf(stdout, buf, _IOLBF, BUFSIZ);

  print_something();

  setvbuf(stdout, buf, _IOFBF, BUFSIZ);

  print_something();
}

int main(void) {
  printf("setvbuf\n");

#if defined(__NetBSD__)
  test_setbuf();
  test_setbuffer();
  test_setlinebuf();
#endif
  test_setvbuf();

  // CHECK: setvbuf

  return 0;
}
