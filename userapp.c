#include "userapp.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#define FILENAME "/proc/mp2/status"

int main(int argc, char *argv[])
{
    int pid = getpid();
    char buf[32] = { 0 };
    int fd = open(FILENAME, O_RDWR|O_CLOEXEC);

    if (fd == 0) {
        perror("open");
        return 1;
    }

    sprintf(buf, "R,%d,%d,%d", pid, 2000, 200);
    int bytes_read = write(fd, buf, sizeof(buf));

    if (bytes_read <= 0) {
        perror("write Registration");
        return 1;
    }
    
    __builtin_memset(buf, 0, sizeof(buf));
    sprintf(buf, "Y,%d", pid);
    bytes_read = write(fd, buf, sizeof(buf));

    if (bytes_read <= 0) {
        perror("write Yield");
        return 1;
    }

    // real time loop
    for (int j = 0; j < 5; j++) { // how do i do "while jobs exist"?

        int i;
        unsigned long val = 0;
        for (i = 0; i < 100; i++) {
            val = val * i;
        }

        __builtin_memset(buf, 0, sizeof(buf));
        sprintf(buf, "Y,%d", pid);
        bytes_read = write(fd, buf, sizeof(buf));

        if (bytes_read <= 0) {
            perror("write Yield");
            return 1;
        }
    }

    __builtin_memset(buf, 0, sizeof(buf));
    sprintf(buf, "D,%d", pid);
    bytes_read = write(fd, buf, sizeof(buf));

     if (bytes_read == 0) {
        perror("write Deregister");
        return 1;
    }

    return 0;
}
