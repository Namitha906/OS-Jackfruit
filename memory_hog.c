#include <stdlib.h>
#include <unistd.h>

int main() {
    while (1) {
        void *p = malloc(512 * 1024); // 512 KB
        if (!p) break;
        usleep(200000); // 0.2 sec
    }
    return 0;
}
