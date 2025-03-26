#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    char *memory;
    int occupied;
    struct block *next;
    struct block *prev;
};

struct file {
    struct block *block_list;
    struct block *last_block;
    int refs;
    char *name;
    size_t size;
    int deleted;
    struct file *next;
    struct file *prev;
};

struct filedesc {
    struct file *file;
    struct block *pos_block;
    int block_offset;
    size_t global_offset;
};

static struct file *file_list = NULL;
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

static int
set_error(enum ufs_error_code err)
{
    ufs_error_code = err;
    return -1;
}

static ssize_t
set_error_ssize(enum ufs_error_code err)
{
    ufs_error_code = err;
    return -1;
}

static struct block *
alloc_block(void)
{
    struct block *b = malloc(sizeof(*b));
    if (!b)
        return NULL;
    b->memory = malloc(BLOCK_SIZE);
    if (!b->memory) {
        free(b);
        return NULL;
    }
    b->occupied = 0;
    b->next = NULL;
    b->prev = NULL;
    return b;
}

static void
free_block(struct block *b)
{
    if (!b) return;
    free(b->memory);
    free(b);
}

static void
file_list_insert(struct file *f)
{
    f->prev = NULL;
    f->next = file_list;
    if (file_list)
        file_list->prev = f;
    file_list = f;
}

static void
file_list_remove(struct file *f)
{
    if (f->prev)
        f->prev->next = f->next;
    else
        file_list = f->next;
    if (f->next)
        f->next->prev = f->prev;
    f->prev = NULL;
    f->next = NULL;
}

static void
free_file(struct file *f)
{
    if (!f) return;
    file_list_remove(f);
    struct block *b = f->block_list;
    while (b) {
        struct block *next = b->next;
        free_block(b);
        b = next;
    }
    free(f->name);
    free(f);
}

static struct file *
find_file_by_name(const char *filename)
{
    struct file *f = file_list;
    while (f) {
        if (!f->deleted && strcmp(f->name, filename) == 0)
            return f;
        f = f->next;
    }
    return NULL;
}

static int
expand_descriptors_if_needed(void)
{
    if (file_descriptor_count < file_descriptor_capacity)
        return 0;
    int new_cap = (file_descriptor_capacity == 0) ? 16 : file_descriptor_capacity * 2;
    struct filedesc **new_array = realloc(file_descriptors, new_cap * sizeof(struct filedesc *));
    if (!new_array)
        return -1;
    for (int i = file_descriptor_capacity; i < new_cap; i++)
        new_array[i] = NULL;
    file_descriptor_capacity = new_cap;
    file_descriptors = new_array;
    return 0;
}

static int
allocate_fd_slot(void)
{
    if (expand_descriptors_if_needed() < 0)
        return -1;
    for (int i = 0; i < file_descriptor_capacity; i++) {
        if (!file_descriptors[i])
            return i;
    }
    return -1;
}

static struct filedesc *
fd_get(int fd)
{
    if (fd < 0 || fd >= file_descriptor_capacity)
        return NULL;
    return file_descriptors[fd];
}

enum ufs_error_code
ufs_errno()
{
    return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
    struct file *f = find_file_by_name(filename);
    if (!f) {
        if (!(flags & UFS_CREATE)) {
            return set_error(UFS_ERR_NO_FILE);
        }
        f = malloc(sizeof(*f));
        if (!f)
            return set_error(UFS_ERR_NO_MEM);
        memset(f, 0, sizeof(*f));
        f->name = strdup(filename);
        if (!f->name) {
            free(f);
            return set_error(UFS_ERR_NO_MEM);
        }
        file_list_insert(f);
    }
    struct filedesc *fdp = malloc(sizeof(*fdp));
    if (!fdp)
        return set_error(UFS_ERR_NO_MEM);
    fdp->file = f;
    fdp->pos_block = f->block_list;
    fdp->block_offset = 0;
    fdp->global_offset = 0;
    int fd_idx = allocate_fd_slot();
    if (fd_idx < 0) {
        free(fdp);
        return set_error(UFS_ERR_NO_MEM);
    }
    file_descriptors[fd_idx] = fdp;
    file_descriptor_count++;
    f->refs++;
    return fd_idx;
}

static void
fd_sync_position(struct filedesc *fdp)
{
    struct file *f = fdp->file;
    size_t off = fdp->global_offset;
    if (off == 0) {
        fdp->pos_block = f->block_list;
        fdp->block_offset = 0;
        return;
    }
    if (off == f->size) {
        fdp->pos_block = f->last_block;
        fdp->block_offset = f->last_block ? f->last_block->occupied : 0;
        return;
    }
    struct block *b = f->block_list;
    size_t remain = off;
    while (b) {
        if (remain <= (size_t)b->occupied) {
            fdp->pos_block = b;
            fdp->block_offset = (int)remain;
            return;
        }
        remain -= b->occupied;
        b = b->next;
    }
    fdp->pos_block = f->last_block;
    fdp->block_offset = f->last_block ? f->last_block->occupied : 0;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    struct filedesc *fdp = fd_get(fd);
    if (!fdp)
        return set_error_ssize(UFS_ERR_NO_FILE);
    if (!size)
        return 0;
    struct file *f = fdp->file;
    if (f->size + size > MAX_FILE_SIZE)
        return set_error_ssize(UFS_ERR_NO_MEM);
    fd_sync_position(fdp);
    size_t written = 0;
    const char *src = buf;
    size_t left = size;
    while (left > 0) {
        if (!fdp->pos_block || fdp->block_offset == BLOCK_SIZE) {
            if (fdp->pos_block && fdp->pos_block->next) {
                fdp->pos_block = fdp->pos_block->next;
                fdp->block_offset = 0;
            } else {
                struct block *nb = alloc_block();
                if (!nb)
                    return set_error_ssize(UFS_ERR_NO_MEM);
                if (!f->block_list) {
                    f->block_list = nb;
                    f->last_block = nb;
                } else if (!fdp->pos_block) {
                    struct block *lb = f->last_block;
                    lb->next = nb;
                    nb->prev = lb;
                    f->last_block = nb;
                } else {
                    struct block *lb = f->last_block;
                    lb->next = nb;
                    nb->prev = lb;
                    f->last_block = nb;
                }
                fdp->pos_block = nb;
                fdp->block_offset = 0;
            }
        }
        int space = BLOCK_SIZE - fdp->block_offset;
        size_t chunk = (left < (size_t)space) ? left : (size_t)space;
        memcpy(fdp->pos_block->memory + fdp->block_offset, src, chunk);
        if (fdp->pos_block->occupied < fdp->block_offset + (int)chunk)
            fdp->pos_block->occupied = fdp->block_offset + (int)chunk;
        fdp->block_offset += (int)chunk;
        fdp->global_offset += chunk;
        src += chunk;
        left -= chunk;
        written += chunk;
        if (fdp->global_offset > f->size) {
            f->size = fdp->global_offset;
            f->last_block = fdp->pos_block;
        }
    }
    return (ssize_t)written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
    struct filedesc *fdp = fd_get(fd);
    if (!fdp)
        return set_error_ssize(UFS_ERR_NO_FILE);
    if (!size)
        return 0;
    struct file *f = fdp->file;
    if (fdp->global_offset >= f->size)
        return 0;
    fd_sync_position(fdp);
    size_t total = 0;
    char *dst = buf;
    size_t left = size;
    while (left > 0) {
        if (!fdp->pos_block)
            break;
        if (fdp->global_offset >= f->size)
            break;
        int can_read = fdp->pos_block->occupied - fdp->block_offset;
        if (can_read <= 0) {
            if (fdp->pos_block->next) {
                fdp->pos_block = fdp->pos_block->next;
                fdp->block_offset = 0;
                continue;
            } else {
                break;
            }
        }
        size_t chunk = (left < (size_t)can_read) ? left : (size_t)can_read;
        memcpy(dst, fdp->pos_block->memory + fdp->block_offset, chunk);
        fdp->block_offset += (int)chunk;
        fdp->global_offset += chunk;
        dst += chunk;
        left -= chunk;
        total += chunk;
    }
    return (ssize_t)total;
}

int
ufs_close(int fd)
{
    struct filedesc *fdp = fd_get(fd);
    if (!fdp)
        return set_error(UFS_ERR_NO_FILE);
    struct file *f = fdp->file;
    file_descriptors[fd] = NULL;
    file_descriptor_count--;
    f->refs--;
    if (f->refs == 0 && f->deleted) {
        free_file(f);
    }
    free(fdp);
    return 0;
}

int
ufs_delete(const char *filename)
{
    struct file *f = find_file_by_name(filename);
    if (!f)
        return set_error(UFS_ERR_NO_FILE);
    f->deleted = 1;
    file_list_remove(f);
    if (f->refs == 0) {
        free_file(f);
    }
    return 0;
}

#if NEED_RESIZE
int
ufs_resize(int fd, size_t new_size)
{
    (void)fd;
    (void)new_size;
    ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
    return -1;
}
#endif

void
ufs_destroy(void)
{
    for (int i = 0; i < file_descriptor_capacity; i++) {
        if (file_descriptors[i]) {
            struct filedesc *fdp = file_descriptors[i];
            file_descriptors[i] = NULL;
            fdp->file->refs--;
            free(fdp);
        }
    }
    file_descriptor_count = 0;
    free(file_descriptors);
    file_descriptors = NULL;
    file_descriptor_capacity = 0;
    struct file *f = file_list;
    while (f) {
        struct file *n = f->next;
        free_file(f);
        f = n;
    }
    file_list = NULL;
    ufs_error_code = UFS_ERR_NO_ERR;
}
