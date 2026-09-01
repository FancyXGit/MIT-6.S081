#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

void xargs(char *cmd, int argc, char *argv[])
{
    if (fork() == 0)
    {
        exec(cmd, argv);
        fprintf(2, "xargs: can't execute %s\n", cmd);
        exit(1);
    }
    else
    {
        wait(0);
    }
}

int splitString(char *p, char *end, int cnt, char *argv[])
{
    char *start;
    while (p < end)
    {
        while (p < end && *p == ' ')
            p++;
        if (p >= end)
            break;

        start = p;
        while (p < end && *p != ' ' && *p != '\n')
            p++;

        if (cnt >= MAXARG - 1)
        {
            fprintf(2, "xargs: too many arguments\n");
            exit(1);
        }
        *p = '\0';
        argv[cnt++] = start;
    }
    argv[cnt] = 0;
    return cnt;
}

int main(int argc, char *argv[])
{
    char *cmd = argv[1];
    char *new_argv[MAXARG];
    int new_argc;
    if (argc - 1 > MAXARG)
    {
        fprintf(2, "xargs: too many arguments\n");
        exit(1);
    }
    new_argv[0] = cmd;
    for (int i = 2; i < argc; i++)
    {
        new_argv[i - 1] = argv[i];
    }
    new_argc = argc - 1;

    char buf[512];
    char *p = buf;
    char *c;
    int n = 0;
    while ((n = read(0, p, buf + sizeof(buf) - p)) > 0)
    {
        c = buf;
        p += n;
        if (p >= buf + sizeof(buf))
        {
            fprintf(2, "xargs: line too long\n");
            exit(2);
        }
        while (c < p)
        {
            if (*c == '\n')
            {
                new_argc = splitString(buf, c, new_argc ,new_argv);
                xargs(cmd, new_argc, new_argv);
                memmove(buf, c + 1, p - c - 1); // 把从c + 1到p - 1的内存拷贝到buf上，向前移动c + 1 - buf位
                p = p - (c + 1 - buf);
                new_argc = argc - 1;
                break;
            }
            c++;
        }
    }
    if (p > buf)
    {
        new_argc = splitString(buf, p, new_argc, new_argv);
        xargs(cmd, new_argc, new_argv);
    }
    exit(0);
}
