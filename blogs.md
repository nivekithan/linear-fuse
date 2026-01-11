This is about my experience building `linear-fuse` a FUSE filesystem that lets you browse your Linear projects and issues as if they were regular files and folders on your computer.

Here's what it does:

- **Mount point** (`/`): Shows all your Linear projects as directories
- **Project folders** (`/<project-name>/`): Each contains up to 50 issues as individual files  
- **Issue files** (`/<project-name>/<issue-id>.md`): When opened, display the issue details (status, assignee, title, description) formatted as markdown with YAML front-matter 

Check out the code: https://github.com/nivekithan/linear-fuse

I'm a backend engineer who's spent my career working with TypeScript, with some side projects in Go, Kotlin, and Rust. C was completely new territory for me.

I started this project to learn C properly:

1. Get comfortable writing and reading C code
2. Learn how to build C projects from source  
3. Understand how FUSE filesystems work

By the end, I had achieved all these goals and discovered something surprising about how AI can help with learning new programming languages.

Now these are my learnings 

### C is refreshingly minimal

Coming from high-level languages, I was surprised by how much C makes you do yourself. Making API requests and parsing JSON requires explicit steps that other languages handle automatically.

I don't mind installing `libcurl` and `cjson`. I'm used to dependencies. What surprised me was how much these libraries still make you do manually. Here's what a simple POST request looks like:

```c
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LINEAR_GRAPHQL_URL "https://api.linear.app/graphql"

struct mem {
  char *data;
  size_t size;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t total = size * nmemb;
  struct mem *m = (struct mem *)userp;
  char *p = realloc(m->data, m->size + total + 1);
  if (!p) return 0;
  m->data = p;
  memcpy(m->data + m->size, contents, total);
  m->size += total;
  m->data[m->size] = '\0';
  return total;
}

int main(void) {
  const char *token = getenv("LINEAR_ACCESS_TOKEN");
  
  if (!token || token[0] == '\0') {
    fprintf(stderr, "LINEAR_ACCESS_TOKEN is not set\n");
    return 1;
  }
  
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    fprintf(stderr, "curl_global_init failed\n");
    return 1;
  }
  
  const char *payload =
    "{"
      "\"query\":\"query { projects(first: 1) { nodes { id name } } }\""
    "}";
    
  CURL *curl = curl_easy_init();
  if (!curl) {
    curl_global_cleanup();
    return 1;
  }
  
  size_t auth_len = strlen("Authorization: ") + strlen(token) + 1;
  char *auth = malloc(auth_len);
  snprintf(auth, auth_len, "Authorization: %s", token);
  
  struct mem resp = {0};
  struct curl_slist *headers = NULL;
  
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, auth);
  
  curl_easy_setopt(curl, CURLOPT_URL, LINEAR_GRAPHQL_URL);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  
  CURLcode rc = curl_easy_perform(curl);
  
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  
  if (rc != CURLE_OK) {
    fprintf(stderr, "curl error: %s\n", curl_easy_strerror(rc));
  } else {
    fprintf(stderr, "http=%ld\n", http);
    if (resp.data) puts(resp.data);
  }
  
  free(resp.data);
  curl_slist_free_all(headers);
  free(auth);
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  
  return (rc == CURLE_OK && http >= 200 && http < 300) ? 0 : 1;
}
```

What surprised me was how explicit everything is. You manually set every option with `curl_easy_setopt`, manage memory for string concatenation, and handle responses through callbacks. Every operation requires deliberate steps.

This explicitness gives you complete control. When you write `malloc(strlen(a) + strlen(b) + 1)`, you know exactly what's happening. There's no hidden behavior or automatic memory management.

This makes C well-suited for systems programming.

### The surprising variety of build systems

C has no standard package manager or build system. Coming from npm, cargo, and go mod, this was unexpected.

To build libfuse, I needed Python to install meson. Other libraries use cmake, make, ninja, or custom build scripts. Some projects skip build systems entirely and provide `code.c` and `code.h` files to copy directly.

This approach has advantages. No dependency resolution, version conflicts, or download waits. You get source code you can read, understand, and compile yourself.

It's like `shadcn` but for every dependency. Personally, I don't like this approach but there are tons of C projects in lots of companies which work this way, so I am not going to question it.

### Headers: the surprising simplicity of C's module system

Initially I thought that to divide your code into multiple files, C requires separate `.c` and `.h` files for implementation and declarations.

But there is a nice trick you can use which removes the need to write multiple files for simple projects.

That is `#include` simply pastes the included file's contents at the directive location. This means `#include <file.json>` is valid C syntax (though it won't compile).

Since `#include` copies text, you can include `.c` files directly instead of creating separate headers, avoiding extra files as long as you manage naming conflicts. 


### How AI helped me finish what I started

I learned basics of C syntax, build systems, and FUSE fundamentals before touching the Linear API integration. The core learning goals were complete.

Implementing HTTP requests and JSON parsing in C requires careful attention to detail. Every string concatenation needs memory allocation. Every API call needs error handling. This implementation work is straightforward but time-consuming.

I used AI to help with this implementation phase. The learning had already happened, I understood how C memory management works and why each step was necessary. AI helped translate that understanding into working code.

This approach let me focus on learning the concepts while still completing a functional project. The result is a working filesystem that demonstrates both C fundamentals and practical implementation. Even though it's not much useful in its current state, it's still a completed project in my book.
