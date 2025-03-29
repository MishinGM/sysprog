#include "userfs.h"
#include <stdlib.h>
#include <string.h>

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100
};

static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    struct block *next;
    struct block *prev;
    int occupied;
    char data[BLOCK_SIZE];
};

struct file {
    char *name;
    int refs;
    int deleted;
    size_t size;
    struct block *first;
    struct block *last;
    struct file *prev;
    struct file *next;
};

struct filedesc {
    struct file *f;
    size_t offset;
    struct block *cur;
    size_t cur_offset;
};

static struct filedesc **fds = NULL;
static size_t fds_size = 0;
static struct file *file_list = NULL;

static struct block *find_block_for_offset(struct file *f, size_t offset, int *off_in_block);

static void set_error(enum ufs_error_code err) {
    ufs_error_code = err;
}

enum ufs_error_code ufs_errno(void) {
    return ufs_error_code;
}

static void remove_file_from_list(struct file *f) {
    if (f->prev)
        f->prev->next = f->next;
    else
        file_list = f->next;
    if (f->next)
        f->next->prev = f->prev;
    f->prev = f->next = 0;
}

static void free_file(struct file *f) {
    struct block *b = f->first;
    while (b) {
        struct block *tmp = b;
        b = b->next;
        free(tmp);
    }
    free(f->name);
    free(f);
}

static struct file *find_file(const char *name) {
    for (struct file *f = file_list; f; f = f->next)
        if (!f->deleted && strcmp(f->name, name) == 0)
            return f;
    return 0;
}

static struct block *alloc_block(struct file *f) {
    struct block *nb = calloc(1, sizeof(*nb));
    if (!nb)
        return 0;
    nb->prev = f->last;
    if (f->last)
        f->last->next = nb;
    else
        f->first = nb;
    f->last = nb;
    return nb;
}

static struct block *get_block(struct file *f, size_t offset, int create, int *off_in_block) {
    (void)create;
    return (offset < f->size) ?
        find_block_for_offset(f, offset, off_in_block) :
        (f->last && ((size_t)f->last->occupied < BLOCK_SIZE) ?
         (*off_in_block = f->last->occupied, f->last) :
         (*off_in_block = 0, alloc_block(f)));
}

static struct block *find_block_for_offset(struct file *f, size_t offset, int *off_in_block) {
    size_t passed = 0;
    struct block *b = f->first;
    while (b) {
        size_t eff = b->occupied;
        if (passed + eff > f->size)
            eff = f->size - passed;
        if (offset < passed + eff) {
            *off_in_block = (int)(offset - passed);
            return b;
        }
        passed += eff;
        b = b->next;
    }
    *off_in_block = 0;
    return 0;
}

static int allocate_fd_slot(struct filedesc *desc) {
    if (!fds) {
        fds_size = 16;
        fds = calloc(fds_size, sizeof(*fds));
        if (!fds) return -1;
    }
    for (size_t i = 0; i < fds_size; i++) {
        if (fds[i] == NULL) {
            fds[i] = desc;
            return (int)i;
        }
    }
    size_t old_size = fds_size;
    fds_size *= 2;
    struct filedesc **new_fds = realloc(fds, fds_size * sizeof(*fds));
    if (!new_fds) return -1;
    fds = new_fds;
    for (size_t i = old_size; i < fds_size; i++)
        fds[i] = NULL;
    fds[old_size] = desc;
    return (int)old_size;
}

int ufs_open(const char *filename, int flags) {
    struct file *ff = find_file(filename);
    if (!ff) {
        if (!(flags & UFS_CREATE)) {
            set_error(UFS_ERR_NO_FILE);
            return -1;
        }
        ff = calloc(1, sizeof(*ff));
        if (!ff) { set_error(UFS_ERR_NO_MEM); return -1; }
        ff->name = strdup(filename);
        if (!ff->name) { free(ff); set_error(UFS_ERR_NO_MEM); return -1; }
        ff->next = file_list;
        if (file_list)
            file_list->prev = ff;
        file_list = ff;
    }
    struct filedesc *desc = calloc(1, sizeof(*desc));
    if (!desc) { set_error(UFS_ERR_NO_MEM); return -1; }
    desc->f = ff;
    desc->offset = 0;
    desc->cur = NULL;
    desc->cur_offset = 0;
    int fd = allocate_fd_slot(desc);
    if (fd < 0) {
        free(desc);
        set_error(UFS_ERR_NO_MEM);
        return -1;
    }
    ff->refs++;
    return fd;
}

ssize_t ufs_write(int fd, const char *buf, size_t size) {
    if (fd < 0 || (size_t)fd >= fds_size || !fds[fd]) { set_error(UFS_ERR_NO_FILE); return -1; }
    if (size == 0)
        return 0;
    struct filedesc *desc = fds[fd];
    struct file *f = desc->f;
    size_t new_size = (desc->offset + size > f->size) ? desc->offset + size : f->size;
    if (new_size > MAX_FILE_SIZE) {
        set_error(UFS_ERR_NO_MEM);
        return -1;
    }
    size_t written = 0;
    while (written < size) {
        int off = 0;
        struct block *b;
        if (desc->offset < f->size) {
            b = find_block_for_offset(f, desc->offset, &off);
            if (!b) { set_error(UFS_ERR_NO_MEM); return -1; }
        } else {
            if (f->last && ((size_t)f->last->occupied < BLOCK_SIZE)) {
                b = f->last;
                off = f->last->occupied;
            } else {
                b = alloc_block(f);
                if (!b) { set_error(UFS_ERR_NO_MEM); return -1; }
                off = 0;
            }
        }
        size_t space = BLOCK_SIZE - off;
        size_t to_write = (size - written < space) ? size - written : space;
        memcpy(b->data + off, buf + written, to_write);
        if ((size_t)off + to_write > (size_t)b->occupied)
            b->occupied = off + (int)to_write;
        written += to_write;
        desc->offset += to_write;
    }
    if (desc->offset > f->size)
        f->size = desc->offset;
    return (ssize_t)written;
}

ssize_t ufs_read(int fd, char *buf, size_t size) {
    if (fd < 0 || (size_t)fd >= fds_size || !fds[fd]) { set_error(UFS_ERR_NO_FILE); return -1; }
    if (size == 0)
        return 0;
    struct filedesc *desc = fds[fd];
    struct file *f = desc->f;
    if (desc->offset >= f->size)
        return 0;
    size_t total = 0;
    while (total < size && desc->offset < f->size) {
        int off = 0;
        struct block *b;
        if (desc->cur && desc->offset >= desc->cur_offset &&
            desc->offset < desc->cur_offset + (size_t)desc->cur->occupied) {
            b = desc->cur;
            off = (int)(desc->offset - desc->cur_offset);
        } else {
            b = find_block_for_offset(f, desc->offset, &off);
            size_t cum = 0;
            for (struct block *tmp = f->first; tmp && tmp != b; tmp = tmp->next)
                cum += (size_t)tmp->occupied;
            desc->cur = b;
            desc->cur_offset = cum;
        }
        if (!b)
            break;
        size_t avail = (size_t)b->occupied - off;
        if (desc->offset + avail > f->size)
            avail = f->size - desc->offset;
        size_t remain = size - total;
        size_t can = (avail < remain) ? avail : remain;
        memcpy(buf + total, b->data + off, can);
        desc->offset += can;
        total += can;
        if (desc->cur && desc->offset >= desc->cur_offset + (size_t)desc->cur->occupied) {
            desc->cur_offset += (size_t)desc->cur->occupied;
            desc->cur = desc->cur->next;
        }
    }
    return (ssize_t)total;
}

int ufs_close(int fd) {
    if (fd < 0 || (size_t)fd >= fds_size || !fds[fd]) { set_error(UFS_ERR_NO_FILE); return -1; }
    struct filedesc *desc = fds[fd];
    struct file *f = desc->f;
    free(desc);
    fds[fd] = NULL;
    f->refs--;
    if (f->deleted && f->refs == 0) {
        if (f->prev || f->next || file_list == f)
            remove_file_from_list(f);
        free_file(f);
    }
    return 0;
}

int ufs_delete(const char *filename) {
    struct file *f = find_file(filename);
    if (!f) { set_error(UFS_ERR_NO_FILE); return -1; }
    f->deleted = 1;
    if (f->refs == 0) {
        remove_file_from_list(f);
        free_file(f);
    }
    return 0;
}

void ufs_destroy(void) {
    for (size_t i = 0; i < fds_size; i++)
        if (fds && fds[i])
            ufs_close(i);
    while (file_list) {
        free_file(file_list);
        file_list = file_list->next;
    }
    ufs_error_code = UFS_ERR_NO_ERR;
}
