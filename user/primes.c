#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    // 2、3、5、7、11、13、17、19、23、29、31

    int number[48];
    for (int i = 0; i < 11; i++)
    {
        // pipes为读端指向第i个进程的管道
        int pipes[2];
        pipe(pipes);

        if (fork() == 0)
        {
            close(pipes[1]);
            read(pipes[0], number, sizeof(number));
            close(pipes[0]);

            int size_read = number[0];
            int ptr_write = 1;
            number[0] = 0;
            int prime = number[1];
            printf("prime %d\n", prime);
            for (int ptr_read = 1; ptr_read <= size_read; ptr_read++)
            {
                if (number[ptr_read] % prime != 0)
                {
                    number[0]++;
                    number[ptr_write] = number[ptr_read];
                    ptr_write++;
                }
            }

            continue;
        }
        else
        {
            if (i == 0)
            {
                number[0] = 34;
                for (int j = 1; j < 35; j++)
                {
                    number[j] = j + 1;
                }
            }
            close(pipes[0]);
            write(pipes[1], number, (number[0] + 1) * sizeof(int));
            close(pipes[1]);
            wait(0);
            break;
        }
    }
    exit(0);
}