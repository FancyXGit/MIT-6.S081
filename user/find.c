#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void find(char *wd, char *file_name)
{
    int fd;
    struct stat st;
    char buf[512];
    struct dirent de;

    if ((fd = open(wd, 0)) < 0)
    {
        fprintf(2, "find: cannot open %s\n", wd);
        return;
    }

    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "find: cannot stat %s\n", wd);
        close(fd);
        return;
    }

    switch (st.type)
    {
    case T_FILE:
        fprintf(2, "find: not a directory %s\n", wd);
        break;

    case T_DIR:
        if (strlen(wd) + 1 + DIRSIZ + 1 > sizeof(buf))
        {
            printf("ls: path too long\n");
            break;
        }
        strcpy(buf, wd);
        char *p = buf + strlen(buf);
        *p++ = '/';
        char name[DIRSIZ + 1];
        while (read(fd, &de, sizeof(de)) == sizeof(de))
        {
            memcpy(name, de.name, DIRSIZ);
            name[DIRSIZ] = 0;
            if (de.inum == 0)
            {
                continue;
            }
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            {
                continue;
            }

            memmove(p, name, DIRSIZ + 1);

            if (stat(buf, &st) < 0)
            {
                printf("find: cannot stat %s\n", buf);
                continue;
            }

            if (st.type == T_FILE)
            {
                if (strcmp(name, file_name) == 0)
                {
                    printf("%s\n", buf);
                    continue;
                }
            }
            else if (st.type == T_DIR)
            {
                find(buf, file_name);
            }
        }
    }

    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(2, "Usage: find working_dir file_name\n");
        exit(1);
    }

    find(argv[1], argv[2]);
    exit(0);
}