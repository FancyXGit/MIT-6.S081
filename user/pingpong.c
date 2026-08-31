#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p_p2c[2];
    int p_c2p[2];

    pipe(p_p2c);
    pipe(p_c2p);

    if (fork() == 0)
    {
        char buf[1];
        int c_pid = getpid();
        close(p_p2c[1]);
        read(p_p2c[0], buf, sizeof(buf));
        close(p_p2c[0]);

        fprintf(1, "%d: received ping\n", c_pid);

        close(p_c2p[0]);
        write(p_c2p[1], buf, sizeof(char));
        close(p_c2p[1]);

        exit(0);
    }
    else
    {
        int p_pid = getpid();
        char content[1];
        content[0] = 'p';
        close(p_p2c[0]);
        write(p_p2c[1], &content, sizeof(char));
        close(p_p2c[1]);

        char buf[1];
        close(p_c2p[1]);
        read(p_c2p[0], buf, sizeof(char));
        close(p_c2p[0]);

        fprintf(1, "%d: received pong\n", p_pid);

        exit(0);
    }
}