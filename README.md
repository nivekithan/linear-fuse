# linear-fuse

A small C playground that currently implements **LinearFS**: a read-only FUSE filesystem that exposes your Linear projects and issues as directories/files.

- `/` lists Linear projects
- `/<project>/` lists issues (by identifier)
- `/<project>/<issue>` is a generated Markdown view of the issue

The filesystem is read-only (`open` rejects non-`O_RDONLY`). Issues are cached in-memory per project (TTL: ~30s) and capped to 50 issues per project.

## Requirements

- Linux with FUSE 3 support
- `clang`
- `libcurl`
- `pthread` (usually part of libc)
- `just` (optional, but recommended; see `justfile`)

Vendored dependencies:

- `vendor/libfuse` (includes a prebuilt `builddir` used by the build command)
- `vendor/cjson`

## Environment

- `LINEAR_ACCESS_TOKEN` (required) – your Linear personal access token
- `LINEARFS_DEBUG=1` (optional) – enables debug logging to stderr

## Build

Using `just`:

```sh
just build
```

This compiles `src/hello.c` and links against vendored FUSE and cJSON:

- include paths: `vendor/libfuse/include`, `vendor/libfuse/builddir`, `vendor/cjson`
- libs: `-lfuse3 -lcurl -lm -pthread`

## Run / Mount

```sh
export LINEAR_ACCESS_TOKEN="..."
just run
```

This mounts the filesystem at `./mnt` and runs FUSE in the foreground:

- mount options: `-o auto_unmount`
- runtime lib path: `LD_LIBRARY_PATH=./vendor/libfuse/builddir/lib`

In another terminal:

```sh
ls mnt
ls "mnt/<project>"
cat "mnt/<project>/<ISSUE-ID>"
```

The issue file renders as Markdown with a small YAML-like front matter (status/assignee), then the title and description.

## Notes

- Project and issue names are sanitized for filesystem safety.
- The schema used for Linear GraphQL lives at `external-api/linear/schema.graphql`.
