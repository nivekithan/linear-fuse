[private]
build:
    clang -I ./vendor/libfuse/include -I ./vendor/libfuse/builddir \
        -L ./vendor/libfuse/builddir/lib \
        ./src/hello.c -l fuse3

[private]
build-example:
    clang -I ./vendor/libfuse/include -I ./vendor/libfuse/builddir \
        -L ./vendor/libfuse/builddir/lib ./src/example.c -lfuse3 -pthread -ldl -lrt

run: build
    mkdir -p ./mnt
    LD_LIBRARY_PATH=./vendor/libfuse/builddir/lib ./a.out -f -o auto_unmount ./mnt

mount-example: build-example
    mkdir -p ./mnt
    LD_LIBRARY_PATH=./vendor/libfuse/builddir/lib ./a.out -f -o auto_unmount ./mnt
