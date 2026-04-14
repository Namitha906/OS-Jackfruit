#include <stdlib.h>
#include <unistd.h>

int main() {
    while (1) {
        void *p = malloc(256 * 1024); // slower growth
        if (!p) break;
        usleep(500000); // 0.5 sec delay
    }
    return 0;
}
