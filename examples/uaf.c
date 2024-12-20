#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv){

    char *ptr = (char*)malloc(32);

    printf("obj allocated at: %p\n", ptr);

    free(ptr);
    printf("obj %p has been deallocated\n", ptr);

    printf("use after free...\n");
    printf("%x\n", ptr[argc]);

    return 0;
}
