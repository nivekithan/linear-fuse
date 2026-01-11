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
#include <pthread.h>
#include <stdarg.h>

#include "curl/curl.h"
#include "helper.c"
#include "linear.c"

static struct timespec mount_time;

static struct linear_project_list linear_projects = {0};

#define LINEARFS_MAX_ISSUES 50
#define LINEARFS_ISSUES_CACHE_TTL_SEC 30

struct linear_issue_cache_entry {
    char *project_id;
    struct linear_issue_list issues;
    time_t fetched_at;
};

static pthread_mutex_t issues_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct linear_issue_cache_entry *issues_cache = NULL;
static size_t issues_cache_count = 0;

static char *linearfs_strdup(const char *src) {
    if (src == NULL) {
        return NULL;
    }

    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }

    memcpy(out, src, len + 1);
    return out;
}

static bool linearfs_debug_enabled(void) {
    static int enabled = -1;
    if (enabled != -1) {
        return enabled == 1;
    }

    const char *env = getenv("LINEARFS_DEBUG");
    enabled = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
    return enabled == 1;
}

static void linearfs_log(const char *fmt, ...) {
    if (!linearfs_debug_enabled()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static struct linear_issue_cache_entry *linearfs_find_cache_entry(const char *project_id) {
    if (project_id == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < issues_cache_count; i++) {
        if (issues_cache[i].project_id != NULL && strcmp(issues_cache[i].project_id, project_id) == 0) {
            return &issues_cache[i];
        }
    }

    return NULL;
}

static struct linear_issue_cache_entry *linearfs_get_or_create_cache_entry_locked(const char *project_id) {
    struct linear_issue_cache_entry *entry = linearfs_find_cache_entry(project_id);
    if (entry != NULL) {
        return entry;
    }

    struct linear_issue_cache_entry *new_cache =
        realloc(issues_cache, (issues_cache_count + 1) * sizeof(*issues_cache));
    if (new_cache == NULL) {
        return NULL;
    }
    issues_cache = new_cache;

    entry = &issues_cache[issues_cache_count];
    memset(entry, 0, sizeof(*entry));
    entry->project_id = linearfs_strdup(project_id);
    if (entry->project_id == NULL) {
        return NULL;
    }

    issues_cache_count++;
    return entry;
}

static const struct linear_issue_list *linearfs_project_issues_locked(const char *project_id) {
    if (project_id == NULL || project_id[0] == '\0') {
        return NULL;
    }

    struct linear_issue_cache_entry *entry = linearfs_get_or_create_cache_entry_locked(project_id);
    if (entry == NULL) {
        return NULL;
    }

    time_t now = time(NULL);
    bool refresh = entry->fetched_at == 0 || (now - entry->fetched_at) >= LINEARFS_ISSUES_CACHE_TTL_SEC;
    if (refresh) {
        linearfs_log("linearfs: refreshing issues for project %s\n", project_id);
        linear_free_issue_list(&entry->issues);
        entry->issues = linear_list_project_issues(project_id, LINEARFS_MAX_ISSUES);
        entry->fetched_at = now;
    }

    return &entry->issues;
}

static void linearfs_free_issue_fields(struct linear_issue *issue) {
    if (issue == NULL) {
        return;
    }

    free(issue->id);
    free(issue->identifier);
    free(issue->title);
    free(issue->description);
    free(issue->state);
    free(issue->assignee);
    memset(issue, 0, sizeof(*issue));
}

static bool linearfs_copy_issue_fields(const struct linear_issue *src, struct linear_issue *dst) {
    if (src == NULL || dst == NULL) {
        return false;
    }

    memset(dst, 0, sizeof(*dst));
    dst->id = linearfs_strdup((src->id != NULL) ? src->id : "");
    dst->identifier = linearfs_strdup((src->identifier != NULL) ? src->identifier : "");
    dst->title = linearfs_strdup((src->title != NULL) ? src->title : "");
    dst->description = linearfs_strdup((src->description != NULL) ? src->description : "");
    dst->state = linearfs_strdup((src->state != NULL) ? src->state : "");
    dst->assignee = linearfs_strdup((src->assignee != NULL) ? src->assignee : "");

    if (
        dst->id == NULL ||
        dst->identifier == NULL ||
        dst->title == NULL ||
        dst->description == NULL ||
        dst->state == NULL ||
        dst->assignee == NULL
    ) {
        linearfs_free_issue_fields(dst);
        return false;
    }

    return true;
}

static void linearfs_free_issues_cache(void) {
    pthread_mutex_lock(&issues_cache_mutex);

    for (size_t i = 0; i < issues_cache_count; i++) {
        free(issues_cache[i].project_id);
        linear_free_issue_list(&issues_cache[i].issues);
    }
    free(issues_cache);

    issues_cache = NULL;
    issues_cache_count = 0;

    pthread_mutex_unlock(&issues_cache_mutex);
}

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

static bool issue_markdown_len(const struct linear_issue *issue, size_t *out_len) {
    if (issue == NULL || out_len == NULL) {
        return false;
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
        return false;
    }

    *out_len = (size_t)needed;
    return true;
}

static char *render_issue_markdown(const struct linear_issue *issue, size_t *out_len) {
    if (issue == NULL) {
        return NULL;
    }

    size_t len = 0;
    if (!issue_markdown_len(issue, &len)) {
        return NULL;
    }

    const char *status = (issue->state != NULL && issue->state[0] != '\0') ? issue->state : "Unknown";
    const char *assigned_to = (issue->assignee != NULL && issue->assignee[0] != '\0') ? issue->assignee : "Unassigned";
    const char *title = (issue->title != NULL) ? issue->title : "";
    const char *description = (issue->description != NULL) ? issue->description : "";

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

    if (stat != NULL) {
        memset(stat, 0, sizeof(*stat));
    }

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

    pthread_mutex_lock(&issues_cache_mutex);
    const struct linear_issue_list *issues = linearfs_project_issues_locked(project->id);
    const struct linear_issue *issue = find_issue_by_filename_in_list(issues, file_leaf);

    size_t content_len = 0;
    bool ok = (issue != NULL) && issue_markdown_len(issue, &content_len);
    pthread_mutex_unlock(&issues_cache_mutex);

    if (!ok) {
        return -ENOENT;
    }

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

    struct linear_issue issue_copy;
    bool ok = false;

    pthread_mutex_lock(&issues_cache_mutex);
    const struct linear_issue_list *issues = linearfs_project_issues_locked(project->id);
    const struct linear_issue *issue = find_issue_by_filename_in_list(issues, file_leaf);
    if (issue != NULL) {
        ok = linearfs_copy_issue_fields(issue, &issue_copy);
    }
    pthread_mutex_unlock(&issues_cache_mutex);

    if (!ok) {
        return -ENOENT;
    }

    size_t content_len = 0;
    char *content = render_issue_markdown(&issue_copy, &content_len);
    linearfs_free_issue_fields(&issue_copy);

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

    pthread_mutex_lock(&issues_cache_mutex);
    const struct linear_issue_list *issues = linearfs_project_issues_locked(project->id);
    const struct linear_issue *issue = find_issue_by_filename_in_list(issues, file_leaf);
    pthread_mutex_unlock(&issues_cache_mutex);

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
    (void)file_info;
    (void)readdir_flags;

    size_t dir_offset = (offset < 0) ? 0 : (size_t)offset;

    uint user_id = geteuid();
    uint group_id = getegid();

    if (are_string_equal(path, "/")) {
        struct stat dir_stat = {0};
        dir_stat.st_mode = S_IFDIR | 0755;
        dir_stat.st_nlink = 2;
        dir_stat.st_uid = user_id;
        dir_stat.st_gid = group_id;
        dir_stat.st_atim = mount_time;
        dir_stat.st_mtim = mount_time;

        if (dir_offset == 0) {
            if (filler(buf, ".", &dir_stat, 1, FUSE_FILL_DIR_PLUS) != 0) {
                return 0;
            }
        }

        if (dir_offset <= 1) {
            if (filler(buf, "..", &dir_stat, 2, FUSE_FILL_DIR_PLUS) != 0) {
                return 0;
            }
        }

        size_t project_start = 0;
        if (dir_offset > 2) {
            project_start = dir_offset - 2;
        }

        char safe_name[256];
        for (size_t i = project_start; i < linear_projects.count; i++) {
            linear_project_fs_name(linear_projects.projects[i].name, safe_name, sizeof(safe_name));
            if (safe_name[0] == '\0') {
                continue;
            }

            off_t next_offset = (off_t)(i + 3);
            if (filler(buf, safe_name, &dir_stat, next_offset, FUSE_FILL_DIR_PLUS) != 0) {
                break;
            }
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

    struct stat dir_stat = {0};
    dir_stat.st_mode = S_IFDIR | 0755;
    dir_stat.st_nlink = 2;
    dir_stat.st_uid = user_id;
    dir_stat.st_gid = group_id;
    dir_stat.st_atim = mount_time;
    dir_stat.st_mtim = mount_time;

    struct stat file_stat = {0};
    file_stat.st_mode = S_IFREG | 0644;
    file_stat.st_nlink = 1;
    file_stat.st_uid = user_id;
    file_stat.st_gid = group_id;
    file_stat.st_atim = mount_time;
    file_stat.st_mtim = mount_time;

    if (dir_offset == 0) {
        if (filler(buf, ".", &dir_stat, 1, FUSE_FILL_DIR_PLUS) != 0) {
            return 0;
        }
    }

    if (dir_offset <= 1) {
        if (filler(buf, "..", &dir_stat, 2, FUSE_FILL_DIR_PLUS) != 0) {
            return 0;
        }
    }

    size_t issue_start = 0;
    if (dir_offset > 2) {
        issue_start = dir_offset - 2;
    }

    pthread_mutex_lock(&issues_cache_mutex);
    const struct linear_issue_list *issues = linearfs_project_issues_locked(project->id);
    if (issues == NULL) {
        pthread_mutex_unlock(&issues_cache_mutex);
        return 0;
    }

    char issue_name[256];
    for (size_t i = issue_start; i < issues->count; i++) {
        linear_issue_fs_name(issues->issues[i].identifier, issue_name, sizeof(issue_name));
        if (issue_name[0] == '\0') {
            continue;
        }

        size_t content_len = 0;
        if (issue_markdown_len(&issues->issues[i], &content_len)) {
            file_stat.st_size = (off_t)content_len;
        } else {
            file_stat.st_size = 0;
        }

        off_t next_offset = (off_t)(i + 3);
        if (filler(buf, issue_name, &file_stat, next_offset, FUSE_FILL_DIR_PLUS) != 0) {
            break;
        }
    }

    pthread_mutex_unlock(&issues_cache_mutex);
    return 0;
}

static void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;

    if (cfg != NULL) {
        cfg->entry_timeout = 5.0;
        cfg->attr_timeout = 5.0;
        cfg->negative_timeout = 1.0;
    }

    return NULL;
}

static void my_destroy(void *private_data) {
    (void)private_data;
    linearfs_free_issues_cache();
}

static struct fuse_operations my_fuse_op = {
    .getattr = my_getattr,
    .read = my_read,
    .readdir = my_readir,
    .open = my_open,
    .init = my_init,
    .destroy = my_destroy,
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
