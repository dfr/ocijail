#include <sys/procctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (procctl(P_PID, 0, PROC_REAP_ACQUIRE, NULL) < 0) {
        perror("procctl");
        return 1;
    }
    execvp(argv[1], argv + 1);
    abort();
}
