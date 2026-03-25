/* Sample C module for Tree-sitter testing. */
#include <stdio.h>
#include <string.h>

/* A simple calculator struct */
typedef struct {
    int history[16];
    int count;
} Calculator;

void calc_init(Calculator* calc) {
    calc->count = 0;
}

int calc_add(Calculator* calc, int a, int b) {
    int result = a + b;
    if (calc->count < 16) {
        calc->history[calc->count++] = result;
    }
    return result;
}

int calc_multiply(Calculator* calc, int a, int b) {
    (void)calc;
    return a * b;
}

void greet(const char* name) {
    printf("Hello, %s!\n", name);
}

void farewell(const char* name) {
    printf("Goodbye, %s!\n", name);
}

int main(void) {
    Calculator calc;
    calc_init(&calc);
    greet("world");
    int result = calc_add(&calc, 2, 3);
    int product = calc_multiply(&calc, result, 2);
    farewell("world");
    return 0;
}
