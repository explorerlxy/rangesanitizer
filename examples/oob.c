#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv){

    if(argc < 2) {
        printf("provide an index argument\n");
        return -1;
    }

    int index = atoi(argv[1]);
    char *ptr = (char*)malloc(32);

    printf("obj allocated at: %p\n", ptr);
    printf("access at index %d...\n", index);

    printf("%x\n", ptr[index]);

    free(ptr);
    return 0;
}
