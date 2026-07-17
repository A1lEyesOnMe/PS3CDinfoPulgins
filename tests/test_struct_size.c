#include <stdio.h>
#include "../include/musicbrainz.h"

int main() {
    printf("CDInfo size: %zu bytes\n", sizeof(CDInfo));
    printf("Cache total: %zu bytes\n", sizeof(CDInfo) * CACHE_SIZE);
    printf("Should be < 1MB\n");
    return 0;
}
