#pragma once

#include <sys/types.h>

#define NEED_OPEN_FLAGS 0
#define NEED_RESIZE 0

enum open_flags {
    UFS_CREATE = 1
};

enum ufs_error_code {
    UFS_ERR_NO_ERR = 0,
    UFS_ERR_NO_FILE,
    UFS_ERR_NO_MEM,
    UFS_ERR_NOT_IMPLEMENTED
};

enum ufs_error_code ufs_errno(void);
int ufs_open(const char *filename, int flags);
ssize_t ufs_write(int fd, const char *buf, size_t size);
ssize_t ufs_read(int fd, char *buf, size_t size);
int ufs_close(int fd);
int ufs_delete(const char *filename);
void ufs_destroy(void);
