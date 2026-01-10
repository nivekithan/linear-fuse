#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)
#define D_FILE_OFFSET_BITS 64

#include "../vendor/libfuse/include/fuse.h"
#include <string.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <bits/time.h>
#include <stdbool.h>
#include <stdio.h>
#include "helper.c"

static struct timespec mount_time;
static const char* hello_path = "/hello";
static const char *hello_content = "Hello, World!\n";

int my_getattr(const char * path, struct stat * stat, struct fuse_file_info* file_info) {
    uint user_id = geteuid();
    uint group_id = getegid();

    if (are_string_equal(path, "/")) {
        stat->st_mode = S_IFDIR | 0755;
        stat->st_nlink = 2; // One for . and another for ..
        stat->st_uid = user_id;
        stat->st_gid = group_id;
        stat->st_atim = mount_time;
        stat->st_mtim = mount_time;
        return 0;
    } else if (are_string_equal(path, hello_path)) {
        stat->st_mode = S_IFREG | 0755;
        stat->st_nlink = 1; // One from . directory itself
        stat->st_uid = user_id;
        stat->st_gid = group_id;
        stat->st_atim = mount_time;
        stat->st_mtim = mount_time;
        stat->st_size = strlen(hello_content);
        return  0;
    }

    return -ENOENT;
};

int my_read( const char * path, char *output, size_t read_size, off_t offset, struct fuse_file_info * file_info) {
    if (are_string_equal(path, hello_path)) {
        int total_content_size = strlen(hello_content);

        if (offset >= total_content_size) {
            // Return 0 to indicate EOF;
            return 0;
        }

        int max_to_read = min_of_numbers(total_content_size-offset, read_size);
        memcpy(output, hello_content + offset, max_to_read);

        return max_to_read;
    }

    return -ENOENT;
};

int my_open(const char *path, struct fuse_file_info *file_info){
    if (strcmp(path, hello_path) != 0) {
        return -ENOENT;
    }

    if ((file_info->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
    }

    return 0;
};

int my_readir(
    const char* path,
    void* buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info* file_info,
    enum fuse_readdir_flags readdir_flags
) {

    if (are_string_equal("/", path)) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        // removes the first '/' from the hello_path
        filler(buf, hello_path + 1, NULL, 0, 0);
        return 0;
    }

    return -ENONET;
};


static struct fuse_operations my_fuse_op  = {
    .getattr = my_getattr,
    .read = my_read,
    .readdir = my_readir,
    .open = my_open,
};

int main(int argc, char *argv[]) {
    clock_gettime(CLOCK_REALTIME, &mount_time);

    return fuse_main(argc, argv, &my_fuse_op, NULL);
}
