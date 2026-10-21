extern int main(int, char **);
extern int __libc_start_main(int (*)(int, char **), int, char **, void *, void *, void *);

__attribute__((noreturn)) void harmonycast_start(long *stack) {
    __libc_start_main(main, (int)stack[0], (char **)(stack + 1), 0, 0, 0);
    for (;;) {}
}
