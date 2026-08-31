#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"

void prime(void)
{
    int p[2];
    pipe(p);

    int prime_num;
    if (read(0, &prime_num, sizeof(int)) == 0)
    {
        exit(0);
    }

    printf("prime %d\n", prime_num);

    if (fork() == 0)
    {
        close(0);
        dup(p[0]);
        close(p[0]);
        close(p[1]);
        prime();
        exit(0);
    }
    else
    {
        close(p[0]);
        int num;
        while (read(0, &num, sizeof(int)))
        {
            if (num % prime_num != 0)
            {
                write(p[1], &num, sizeof(int));
            }
        }
        close(p[1]);
        wait(0);
        exit(0);
    }
}

#pragma GCC diagnostic pop

int main(int argc, char *argv[])
{
    int p[2];
    pipe(p);

    if (fork() == 0)
    {
        close(0);
        dup(p[0]);
        close(p[0]);
        close(p[1]);
        prime();
        exit(0);
    }
    else
    {
        close(p[0]);

        for (int i = 2; i < 36; i++)
        {
            write(p[1], &i, sizeof(int));
        }

        close(p[1]);
        wait(0);
        exit(0);
    }
}