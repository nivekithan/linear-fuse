set dotenv-load

[private]
build:
    clang -I ./vendor/libfuse/include -I ./vendor/libfuse/builddir -I ./vendor/cjson \
        -L ./vendor/libfuse/builddir/lib \
        ./src/hello.c ./vendor/cjson/cJSON.c -l fuse3 -l curl -lm

run: build
    mkdir -p ./mnt
    LD_LIBRARY_PATH=./vendor/libfuse/builddir/lib ./a.out -f -o auto_unmount ./mnt
