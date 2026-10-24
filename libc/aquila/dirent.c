#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <aquila/syscall.h>

#include "syscall.h"

DIR *fdopendir(int fd)
{
    struct stat st;

    if (fstat(fd, &st))
        return NULL;

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }

    DIR *dir = malloc(sizeof(DIR));

    if (!dir)
        return NULL;

    dir->fd = fd;
    return dir;
}

DIR *opendir(const char *path)
{
    int fd = open(path, O_SEARCH | O_DIRECTORY | O_CLOEXEC);

    if (fd == -1)
        return NULL;

    DIR *dir = fdopendir(fd);

    if (!dir)
        close(fd);

    return dir;
}

struct dirent *readdir(DIR *dir)
{
    /* returns the number of entries read, 0 at the end of the directory */
    long ret = __syscall(SYS_readdir, dir->fd, (long) &dir->dirent, 0);

    if (ret <= 0) {
        if (ret < 0)
            errno = -ret;
        return NULL;
    }

    return &dir->dirent;
}

/* into the caller's entry, *result NULL at the end of the directory */
int readdir_r(DIR *dir, struct dirent *entry, struct dirent **result)
{
    long ret = __syscall(SYS_readdir, dir->fd, (long) entry, 0);

    *result = ret > 0 ? entry : NULL;
    return ret < 0 ? -ret : 0;
}

void rewinddir(DIR *dir)
{
    lseek(dir->fd, 0, SEEK_SET);
}

int closedir(DIR *dir)
{
    int ret = close(dir->fd);
    free(dir);
    return ret;
}

int dirfd(DIR *dir)
{
    return dir->fd;
}

/* alphabetical order of two entries, for scandir() (POSIX: strcoll();
 * there are no locales, it's strcmp()) */
int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

/*
 * The entries of a directory that `filter` keeps (all if NULL), copied
 * into a new array in *namelist, sorted with `compar` (unsorted if NULL):
 * how many there are, or -1 and errno. The caller frees each entry, then
 * the array.
 */
int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    DIR *dir = opendir(path);

    if (!dir)
        return -1;

    struct dirent **list = NULL, *d;
    size_t count = 0, room = 0;
    int err = 0;

    errno = 0;

    while ((d = readdir(dir))) {
        if (filter && !filter(d))
            continue;

        if (count == room) {
            size_t n = room ? 2 * room : 16;
            struct dirent **grown = realloc(list, n * sizeof(*list));

            if (!grown) {
                err = ENOMEM;
                break;
            }

            list = grown;
            room = n;
        }

        if (!(list[count] = malloc(sizeof(**list)))) {
            err = ENOMEM;
            break;
        }

        memcpy(list[count++], d, sizeof(**list));
    }

    /* readdir() failing, not the end */
    if (!err && errno)
        err = errno;

    closedir(dir);

    if (err) {
        while (count)
            free(list[--count]);

        free(list);
        errno = err;
        return -1;
    }

    if (compar && count > 1)
        qsort(list, count, sizeof(*list), (int (*)(const void *, const void *)) compar);

    *namelist = list;
    return count;
}
