#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)
#define D_FILE_OFFSET_BITS 64

#include "../vendor/libfuse/include/fuse.h"

#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <bits/time.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "curl/curl.h"
#include "helper.c"
#include "linear.c"

static struct timespec mount_time;

static struct linear_project_list linear_projects = {0};

static void linear_project_fs_name(const char *project_name, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    if (project_name == NULL) {
        out[0] = '\0';
        return;
    }

    size_t write_idx = 0;
    for (size_t read_idx = 0; project_name[read_idx] != '\0' && write_idx + 1 < out_size; read_idx++) {
        char c = project_name[read_idx];
        if (c == '/') {
            c = '_';
        }
        out[write_idx++] = c;
    }
    out[write_idx] = '\0';
}

static void linear_issue_fs_name(const char *identifier, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }

    if (identifier == NULL) {
        out[0] = '\0';
        return;
    }

    const char *dash = strchr(identifier, '-');
    if (dash == NULL || dash == identifier || dash[1] == '\0') {
        snprintf(out, out_size, "%s.md", identifier);
        return;
    }

    long number = strtol(dash + 1, NULL, 10);
    if (number <= 0) {
        snprintf(out, out_size, "%s.md", identifier);
        return;
    }

    snprintf(out, out_size, "%03ld.md", number);
}

static bool fs_split_path(const char *path, char *first, size_t first_size, char *second, size_t second_size) {
    if (first_size == 0 || second_size == 0) {
        return false;
    }

    first[0] = '\0';
    second[0] = '\0';

    if (path == NULL || path[0] != '/') {
        return false;
    }

    const char *start = path + 1;
    if (*start == '\0') {
        return false;
    }

    const char *slash = strchr(start, '/');
    if (slash == NULL) {
        snprintf(first, first_size, "%s", start);
        return true;
    }

    size_t first_len = (size_t)(slash - start);
    if (first_len == 0) {
        return false;
    }

    if (first_len + 1 > first_size) {
        first_len = first_size - 1;
    }
    memcpy(first, start, first_len);
    first[first_len] = '\0';

    const char *second_start = slash + 1;
    if (*second_start == '\0') {
        second[0] = '\0';
        return true;
    }

    if (strchr(second_start, '/') != NULL) {
        return false;
    }

    snprintf(second, second_size, "%s", second_start);
    return true;
}

static const struct linear_project *find_project_by_fs_name(const char *leaf) {
    char safe_name[256];

    for (size_t i = 0; i < linear_projects.count; i++) {
        linear_project_fs_name(linear_projects.projects[i].name, safe_name, sizeof(safe_name));
        if (safe_name[0] == '\0') {
            continue;
        }

        if (are_string_equal(leaf, safe_name)) {
            return &linear_projects.projects[i];
        }
    }

    return NULL;
}

static const struct linear_issue *find_issue_by_filename_in_list(const struct linear_issue_list *list, const char *filename) {
    if (list == NULL || filename == NULL) {
        return NULL;
    }

    char issue_name[256];
    for (size_t i = 0; i < list->count; i++) {
        linear_issue_fs_name(list->issues[i].identifier, issue_name, sizeof(issue_name));
        if (are_string_equal(filename, issue_name)) {
            return &list->issues[i];
        }
    }

    return NULL;
}

static char *render_issue_markdown(const struct linear_issue *issue, size_t *out_len) {
    if (issue == NULL) {
        return NULL;
    }

    const char *status = (issue->state != NULL && issue->state[0] != '\0') ? issue->state : "Unknown";
    const char *assigned_to = (issue->assignee != NULL && issue->assignee[0] != '\0') ? issue->assignee : "Unassigned";
    const char *title = (issue->title != NULL) ? issue->title : "";
    const char *description = (issue->description != NULL) ? issue->description : "";

    int needed = snprintf(
        NULL,
        0,
        "---\nstatus: \"%s\"\nassigned_to: \"%s\"\n---\n\n# %s\n\n%s\n",
        status,
        assigned_to,
        title,
        description
    );

    if (needed < 0) {
        return NULL;
    }

    size_t len = (size_t)needed;
    char *content = malloc(len + 1);
    if (content == NULL) {
        return NULL;
    }

    snprintf(
        content,
        len + 1,
        "---\nstatus: \"%s\"\nassigned_to: \"%s\"\n---\n\n# %s\n\n%s\n",
        status,
        assigned_to,
        title,
        description
    );

    if (out_len != NULL) {
        *out_len = len;
    }

    return content;
}

static size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

int my_getattr(const char *path, struct stat *stat, struct fuse_file_info *file_info) {
    (void)file_info;

    uint user_id = geteuid();
    uint group_id = getegid();

    if (are_string_equal(path, "/")) {
        stat->st_mode = S_IFDIR | 0755;
        stat->st_nlink = 2;
        stat->st_uid = user_id;
        stat->st_gid = group_id;
        stat->st_atim = mount_time;
        stat->st_mtim = mount_time;
        return 0;
    }

    char project_leaf[256];
    char file_leaf[256];
    if (!fs_split_path(path, project_leaf, sizeof(project_leaf), file_leaf, sizeof(file_leaf))) {
        return -ENOENT;
    }

    const struct linear_project *project = find_project_by_fs_name(project_leaf);
    if (project == NULL) {
        return -ENOENT;
    }

    if (file_leaf[0] == '\0') {
        stat->st_mode = S_IFDIR | 0755;
        stat->st_nlink = 2;
        stat->st_uid = user_id;
        stat->st_gid = group_id;
        stat->st_atim = mount_time;
        stat->st_mtim = mount_time;
        return 0;
    }

    struct linear_issue_list issues = linear_list_project_issues(project->id, 50);
    const struct linear_issue *issue = find_issue_by_filename_in_list(&issues, file_leaf);
    if (issue == NULL) {
        linear_free_issue_list(&issues);
        return -ENOENT;
    }

    size_t content_len = 0;
    char *content = render_issue_markdown(issue, &content_len);
    linear_free_issue_list(&issues);

    if (content == NULL) {
        return -ENOENT;
    }
    free(content);

    stat->st_mode = S_IFREG | 0644;
    stat->st_nlink = 1;
    stat->st_uid = user_id;
    stat->st_gid = group_id;
    stat->st_atim = mount_time;
    stat->st_mtim = mount_time;
    stat->st_size = (off_t)content_len;

    return 0;
}

int my_read(const char *path, char *output, size_t read_size, off_t offset, struct fuse_file_info *file_info) {
    (void)file_info;

    char project_leaf[256];
    char file_leaf[256];
    if (!fs_split_path(path, project_leaf, sizeof(project_leaf), file_leaf, sizeof(file_leaf))) {
        return -ENOENT;
    }

    const struct linear_project *project = find_project_by_fs_name(project_leaf);
    if (project == NULL || file_leaf[0] == '\0') {
        return -ENOENT;
    }

    struct linear_issue_list issues = linear_list_project_issues(project->id, 50);
    const struct linear_issue *issue = find_issue_by_filename_in_list(&issues, file_leaf);
    if (issue == NULL) {
        linear_free_issue_list(&issues);
        return -ENOENT;
    }

    size_t content_len = 0;
    char *content = render_issue_markdown(issue, &content_len);
    linear_free_issue_list(&issues);

    if (content == NULL) {
        return -ENOENT;
    }

    size_t offset_u = (offset < 0) ? 0 : (size_t)offset;
    if (offset_u >= content_len) {
        free(content);
        return 0;
    }

    size_t max_to_read = min_size(content_len - offset_u, read_size);
    memcpy(output, content + offset_u, max_to_read);
    free(content);

    return (int)max_to_read;
}

int my_open(const char *path, struct fuse_file_info *file_info) {
    char project_leaf[256];
    char file_leaf[256];
    if (!fs_split_path(path, project_leaf, sizeof(project_leaf), file_leaf, sizeof(file_leaf))) {
        return -ENOENT;
    }

    const struct linear_project *project = find_project_by_fs_name(project_leaf);
    if (project == NULL || file_leaf[0] == '\0') {
        return -ENOENT;
    }

    struct linear_issue_list issues = linear_list_project_issues(project->id, 50);
    const struct linear_issue *issue = find_issue_by_filename_in_list(&issues, file_leaf);
    linear_free_issue_list(&issues);

    if (issue == NULL) {
        return -ENOENT;
    }

    if ((file_info->flags & O_ACCMODE) != O_RDONLY) {
        return -EACCES;
    }

    return 0;
}

int my_readir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *file_info,
    enum fuse_readdir_flags readdir_flags
) {
    (void)offset;
    (void)file_info;
    (void)readdir_flags;

    if (are_string_equal(path, "/")) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);

        char safe_name[256];
        for (size_t i = 0; i < linear_projects.count; i++) {
            linear_project_fs_name(linear_projects.projects[i].name, safe_name, sizeof(safe_name));
            if (safe_name[0] == '\0') {
                continue;
            }
            filler(buf, safe_name, NULL, 0, 0);
        }

        return 0;
    }

    char project_leaf[256];
    char file_leaf[256];
    if (!fs_split_path(path, project_leaf, sizeof(project_leaf), file_leaf, sizeof(file_leaf))) {
        return -ENOENT;
    }

    if (file_leaf[0] != '\0') {
        return -ENOENT;
    }

    const struct linear_project *project = find_project_by_fs_name(project_leaf);
    if (project == NULL) {
        return -ENOENT;
    }

    struct linear_issue_list issues = linear_list_project_issues(project->id, 50);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    char issue_name[256];
    for (size_t i = 0; i < issues.count; i++) {
        linear_issue_fs_name(issues.issues[i].identifier, issue_name, sizeof(issue_name));
        if (issue_name[0] == '\0') {
            continue;
        }
        filler(buf, issue_name, NULL, 0, 0);
    }

    linear_free_issue_list(&issues);
    return 0;
}

static struct fuse_operations my_fuse_op = {
    .getattr = my_getattr,
    .read = my_read,
    .readdir = my_readir,
    .open = my_open,
};

int main(int argc, char *argv[]) {
    CURLcode global_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (global_init_result != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed: %s\n", curl_easy_strerror(global_init_result));
        return 1;
    }

    clock_gettime(CLOCK_REALTIME, &mount_time);

    linear_projects = linear_list_all_projects();

    int result = fuse_main(argc, argv, &my_fuse_op, NULL);

    linear_free_project_list(&linear_projects);
    curl_global_cleanup();
    return result;
}
