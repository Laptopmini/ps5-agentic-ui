// Stubs for dynamic-loader hooks the static unwind/libc++ runtime references but a native title
// never calls (no exceptions are thrown here). Defined locally so they aren't unresolved imports
// the native converter needs an SDK stub for; the version script keeps them internal.
#include <stddef.h>
void *__dlopen(const char *path, int mode) { (void)path; (void)mode; return NULL; }
int __dlclose(void *handle) { (void)handle; return 0; }
void *__dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return NULL; }
int __dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }
char *__dlerror(void) { return NULL; }
