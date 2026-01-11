#include "linear.h"

#include "curl/curl.h"
#include "../vendor/cjson/cJSON.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#define LINEAR_GRAPHQL_URL "https://api.linear.app/graphql"

#define LINEAR_CURL_CONNECT_TIMEOUT_MS 5000L
#define LINEAR_CURL_TIMEOUT_MS 20000L
#define LINEAR_CURL_LOW_SPEED_LIMIT 100L
#define LINEAR_CURL_LOW_SPEED_TIME 10L

struct linear_memory_buffer {
    char *data;
    size_t size;
};

static bool linear_debug_enabled(void) {
    static int enabled = -1;
    if (enabled != -1) {
        return enabled == 1;
    }

    const char *env = getenv("LINEARFS_DEBUG");
    enabled = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
    return enabled == 1;
}

static void linear_debug(const char *fmt, ...) {
    if (!linear_debug_enabled()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static size_t linear_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    struct linear_memory_buffer *buffer = (struct linear_memory_buffer *)userp;

    char *new_data = realloc(buffer->data, buffer->size + total_size + 1);
    if (new_data == NULL) {
        return 0;
    }

    buffer->data = new_data;
    memcpy(&(buffer->data[buffer->size]), contents, total_size);
    buffer->size += total_size;
    buffer->data[buffer->size] = '\0';

    return total_size;
}

static char *linear_strdup(const char *src) {
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

static const char *linear_skip_spaces(const char *s) {
    if (s == NULL) {
        return NULL;
    }

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }

    return s;
}


static CURLcode linear_perform_request(
    CURL *curl,
    const char *payload,
    const char *auth_header,
    struct linear_memory_buffer *response,
    long *http_code
) {
    if (response != NULL) {
        free(response->data);
        response->data = NULL;
        response->size = 0;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, LINEAR_GRAPHQL_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, linear_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, LINEAR_CURL_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, LINEAR_CURL_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, LINEAR_CURL_LOW_SPEED_LIMIT);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, LINEAR_CURL_LOW_SPEED_TIME);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "learn-c-linearfs/1");

    linear_debug("linear: POST /graphql (%zu bytes)\n", (payload != NULL) ? strlen(payload) : 0UL);
    CURLcode result = curl_easy_perform(curl);

    long local_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &local_code);

    double total_time = 0.0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);

    linear_debug(
        "linear: result=%d http=%ld time=%.3fs bytes=%zu\n",
        (int)result,
        local_code,
        total_time,
        (response != NULL) ? response->size : 0
    );

    if (result == CURLE_OK && http_code != NULL) {
        *http_code = local_code;
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(headers);
    return result;
}

static void linear_report_graphql_errors(cJSON *root) {
    if (root == NULL) {
        return;
    }

    cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    if (errors != NULL && cJSON_IsArray(errors)) {
        int errors_count = cJSON_GetArraySize(errors);
        for (int i = 0; i < errors_count; i++) {
            cJSON *err = cJSON_GetArrayItem(errors, i);
            if (err == NULL || !cJSON_IsObject(err)) {
                continue;
            }

            cJSON *message = cJSON_GetObjectItemCaseSensitive(err, "message");
            if (cJSON_IsString(message)) {
                fprintf(stderr, "Linear GraphQL error: %s\n", message->valuestring);
            }
        }
    }
}

static const char *linear_get_env_token(void) {
    const char *token = getenv("LINEAR_ACCESS_TOKEN");

    if (token == NULL || token[0] == '\0') {
        fprintf(stderr, "LINEAR_ACCESS_TOKEN (personal access token) is not set\n");
        return NULL;
    }

    const char *token_trimmed = linear_skip_spaces(token);
    if (token_trimmed == NULL || token_trimmed[0] == '\0') {
        fprintf(stderr, "Linear access token is empty\n");
        return NULL;
    }

    return token_trimmed;
}

static char *linear_build_auth_header(void) {
    const char *token_value = linear_get_env_token();
    if (token_value == NULL) {
        return NULL;
    }

    size_t auth_header_len = strlen("Authorization: ") + strlen(token_value) + 1;
    char *auth_header = malloc(auth_header_len);
    if (auth_header == NULL) {
        return NULL;
    }

    snprintf(auth_header, auth_header_len, "Authorization: %s", token_value);
    return auth_header;
}

void linear_free_project_list(struct linear_project_list *list) {
    if (list == NULL) {
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        free(list->projects[i].name);
        free(list->projects[i].id);
    }
    free(list->projects);

    list->projects = NULL;
    list->count = 0;
}

void linear_free_issue_list(struct linear_issue_list *list) {
    if (list == NULL) {
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        free(list->issues[i].id);
        free(list->issues[i].identifier);
        free(list->issues[i].title);
        free(list->issues[i].description);
        free(list->issues[i].state);
        free(list->issues[i].assignee);
    }
    free(list->issues);

    list->issues = NULL;
    list->count = 0;
}

static struct linear_project_list linear_projects_from_response(const char *json, size_t max_projects) {
    struct linear_project_list list = {0};

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return list;
    }

    linear_report_graphql_errors(root);

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *projects = (data != NULL) ? cJSON_GetObjectItemCaseSensitive(data, "projects") : NULL;
    cJSON *nodes = (projects != NULL) ? cJSON_GetObjectItemCaseSensitive(projects, "nodes") : NULL;

    if (nodes == NULL || !cJSON_IsArray(nodes)) {
        cJSON_Delete(root);
        return list;
    }

    int nodes_count = cJSON_GetArraySize(nodes);
    for (int i = 0; i < nodes_count && list.count < max_projects; i++) {
        cJSON *node = cJSON_GetArrayItem(nodes, i);
        if (node == NULL || !cJSON_IsObject(node)) {
            continue;
        }

        cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(node, "name");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) {
            continue;
        }

        struct linear_project *new_projects =
            realloc(list.projects, (list.count + 1) * sizeof(*list.projects));
        if (new_projects == NULL) {
            linear_free_project_list(&list);
            cJSON_Delete(root);
            return list;
        }
        list.projects = new_projects;

        list.projects[list.count].id = linear_strdup(id->valuestring);
        list.projects[list.count].name = linear_strdup(name->valuestring);
        if (list.projects[list.count].id == NULL || list.projects[list.count].name == NULL) {
            free(list.projects[list.count].id);
            free(list.projects[list.count].name);
            linear_free_project_list(&list);
            cJSON_Delete(root);
            return list;
        }

        list.count++;
    }

    cJSON_Delete(root);
    return list;
}

static struct linear_issue_list linear_issues_from_response(const char *json, size_t max_issues) {
    struct linear_issue_list list = {0};

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return list;
    }

    linear_report_graphql_errors(root);

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    cJSON *issues = (data != NULL) ? cJSON_GetObjectItemCaseSensitive(data, "issues") : NULL;
    if (issues == NULL && data != NULL) {
        cJSON *project = cJSON_GetObjectItemCaseSensitive(data, "project");
        issues = (project != NULL) ? cJSON_GetObjectItemCaseSensitive(project, "issues") : NULL;
    }

    cJSON *nodes = (issues != NULL) ? cJSON_GetObjectItemCaseSensitive(issues, "nodes") : NULL;

    if (nodes == NULL || !cJSON_IsArray(nodes)) {
        cJSON_Delete(root);
        return list;
    }

    int nodes_count = cJSON_GetArraySize(nodes);
    for (int i = 0; i < nodes_count && list.count < max_issues; i++) {
        cJSON *node = cJSON_GetArrayItem(nodes, i);
        if (node == NULL || !cJSON_IsObject(node)) {
            continue;
        }

        cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
        cJSON *identifier = cJSON_GetObjectItemCaseSensitive(node, "identifier");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(node, "title");
        cJSON *description = cJSON_GetObjectItemCaseSensitive(node, "description");

        if (!cJSON_IsString(id) || !cJSON_IsString(identifier) || !cJSON_IsString(title)) {
            continue;
        }

        const char *description_value = (cJSON_IsString(description) ? description->valuestring : "");

        const char *state_name_value = "";
        cJSON *state = cJSON_GetObjectItemCaseSensitive(node, "state");
        if (state != NULL && cJSON_IsObject(state)) {
            cJSON *state_name = cJSON_GetObjectItemCaseSensitive(state, "name");
            if (cJSON_IsString(state_name)) {
                state_name_value = state_name->valuestring;
            }
        }

        const char *assignee_name_value = "";
        cJSON *assignee = cJSON_GetObjectItemCaseSensitive(node, "assignee");
        if (assignee != NULL && cJSON_IsObject(assignee)) {
            cJSON *assignee_name = cJSON_GetObjectItemCaseSensitive(assignee, "name");
            if (cJSON_IsString(assignee_name)) {
                assignee_name_value = assignee_name->valuestring;
            }
        }

        struct linear_issue *new_issues =
            realloc(list.issues, (list.count + 1) * sizeof(*list.issues));
        if (new_issues == NULL) {
            linear_free_issue_list(&list);
            cJSON_Delete(root);
            return list;
        }
        list.issues = new_issues;

        list.issues[list.count].id = linear_strdup(id->valuestring);
        list.issues[list.count].identifier = linear_strdup(identifier->valuestring);
        list.issues[list.count].title = linear_strdup(title->valuestring);
        list.issues[list.count].description = linear_strdup(description_value);
        list.issues[list.count].state = linear_strdup(state_name_value);
        list.issues[list.count].assignee = linear_strdup(assignee_name_value);

        if (
            list.issues[list.count].id == NULL ||
            list.issues[list.count].identifier == NULL ||
            list.issues[list.count].title == NULL ||
            list.issues[list.count].description == NULL ||
            list.issues[list.count].state == NULL ||
            list.issues[list.count].assignee == NULL
        ) {
            free(list.issues[list.count].id);
            free(list.issues[list.count].identifier);
            free(list.issues[list.count].title);
            free(list.issues[list.count].description);
            free(list.issues[list.count].state);
            free(list.issues[list.count].assignee);
            linear_free_issue_list(&list);
            cJSON_Delete(root);
            return list;
        }

        list.count++;
    }

    cJSON_Delete(root);
    return list;
}

/**
 * Returns up to 50 Linear projects for the current user.
 * Caller owns the returned strings and must free them.
 */
struct linear_project_list linear_list_all_projects(void) {
    struct linear_project_list projects = {0};

    const char *token_value = linear_get_env_token();
    if (token_value == NULL) {
        return projects;
    }

    const char *payload =
        "{" 
        "\"query\":\"query { projects(first: 50) { nodes { id name } } }\"" 
        "}";

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "curl_easy_init failed\n");
        return projects;
    }

    struct linear_memory_buffer response = {0};

    size_t auth_header_len = strlen("Authorization: ") + strlen(token_value) + 1;
    char auth_header[auth_header_len];
    snprintf(auth_header, auth_header_len, "Authorization: %s", token_value);

    long http_code = 0;
    CURLcode result = linear_perform_request(curl, payload, auth_header, &response, &http_code);
    if (result != CURLE_OK) {
        fprintf(stderr, "Linear request failed: %s\n", curl_easy_strerror(result));
        free(response.data);
        curl_easy_cleanup(curl);
        return projects;
    }

    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "Linear HTTP error: %ld\n", http_code);
        if (response.data != NULL) {
            fprintf(stderr, "%s\n", response.data);
        }
        free(response.data);
        curl_easy_cleanup(curl);
        return projects;
    }

    if (response.data != NULL) {
        projects = linear_projects_from_response(response.data, 50);
    }

    free(response.data);
    curl_easy_cleanup(curl);

    return projects;
}

struct linear_issue_list linear_list_project_issues(const char *project_id, size_t max_issues) {
    struct linear_issue_list issues = {0};

    if (project_id == NULL || project_id[0] == '\0') {
        return issues;
    }

    if (max_issues == 0) {
        max_issues = 50;
    }

    char *auth_header = linear_build_auth_header();
    if (auth_header == NULL) {
        return issues;
    }

    int payload_size = snprintf(
        NULL,
        0,
        "{\"query\":\"query { issues(first: %zu, filter: { project: { id: { eq: \\\"%s\\\" } } }) { nodes { id identifier title description state { name } assignee { name } } } }\"}",
        max_issues,
        project_id
    );

    if (payload_size < 0) {
        free(auth_header);
        return issues;
    }

    size_t payload_len = (size_t)payload_size + 1;
    char *payload = malloc(payload_len);
    if (payload == NULL) {
        free(auth_header);
        return issues;
    }

    snprintf(
        payload,
        payload_len,
        "{\"query\":\"query { issues(first: %zu, filter: { project: { id: { eq: \\\"%s\\\" } } }) { nodes { id identifier title description state { name } assignee { name } } } }\"}",
        max_issues,
        project_id
    );

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "curl_easy_init failed\n");
        free(payload);
        free(auth_header);
        return issues;
    }

    struct linear_memory_buffer response = {0};

    long http_code = 0;
    CURLcode result = linear_perform_request(curl, payload, auth_header, &response, &http_code);
    if (result != CURLE_OK) {
        fprintf(stderr, "Linear request failed: %s\n", curl_easy_strerror(result));
        free(response.data);
        curl_easy_cleanup(curl);
        free(payload);
        free(auth_header);
        return issues;
    }

    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "Linear HTTP error: %ld\n", http_code);
        if (response.data != NULL) {
            fprintf(stderr, "%s\n", response.data);
        }
        free(response.data);
        curl_easy_cleanup(curl);
        free(payload);
        free(auth_header);
        return issues;
    }

    if (response.data != NULL) {
        issues = linear_issues_from_response(response.data, max_issues);
    }

    free(response.data);
    curl_easy_cleanup(curl);
    free(payload);
    free(auth_header);

    return issues;
}
