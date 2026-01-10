#include <string.h>
#include <stdbool.h>

bool are_string_equal(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

int min_of_numbers(int a, int b ) {
    if (a < b) {
        return a;
    }

    return b;
}
