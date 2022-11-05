LLVM & Clang tools wrapped up in one portable executable

First time setup:

    ./init.sh

Build & test:

    ./build.sh -debug
    build/debug/c0 build examples/hello.c
    build/hello

Build & run in continuous mode:

    ./build.sh -debug -w -run='build/debug/c0 build examples/hello.c && build/hello'


## LLVM

By default llvm & clang is built in release mode with assertions.
There's a small but noticeable perf hit introduced by assertions.
You can build llvm without them like this:

    etc/llvm/build-llvm.sh -force -no-assertions

You can also customize llvm build mode.
Available modes: Debug, Release, RelWithDebInfo and MinSizeRel (default)

    etc/llvm/build-llvm.sh -force -mode RelWithDebInfo

Note: These flags can also be passed to `./init.sh`.
