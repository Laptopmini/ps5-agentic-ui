// Does a payload that does nothing still abort at exit? (tls-check aborted after main returned
// with "resource deadlock would occur", even with only libcurl in use.)
#include <stdio.h>

int main(void) {
  printf("[exit-check] hello, returning from main\n");
  fflush(stdout);
  return 0;
}
