#ifndef LINEAR_H
#define LINEAR_H

#include <stddef.h>

struct linear_project {
    char *name;
    char *id;
};

struct linear_project_list {
    struct linear_project *projects;
    size_t count;
};

struct linear_issue {
    char *id;
    char *identifier;
    char *title;
    char *description;
    char *state;
    char *assignee;
};

struct linear_issue_list {
    struct linear_issue *issues;
    size_t count;
};

struct linear_project_list linear_list_all_projects(void);
void linear_free_project_list(struct linear_project_list *list);

struct linear_issue_list linear_list_project_issues(const char *project_id, size_t max_issues);
void linear_free_issue_list(struct linear_issue_list *list);

#endif
