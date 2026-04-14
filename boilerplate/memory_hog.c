#include <stdlib.h>
#include <unistd.h>

int main() {
    while (1) {
        void *p = malloc(1024 * 1024); // allocate 1MB
        if (!p) break;
        sleep(1);
    }
    return 0;
}
