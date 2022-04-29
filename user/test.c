#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

void testCAS() {
    int forks = 5;
    for (int i = 0; i < forks; i++) {
        fork();
    }
    printf("PID: %d\n", getpid());
}


int main(int argc, char** argv){
    testCAS();
    exit(0);
}
