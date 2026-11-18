#!/bin/sh
# Builds both demo versions:
#   watcher     (V1) terminal version from demo.c
#   watcher_v2  (V2) 1024x576 windowed version from demo_v2.c
set -e
cd "$(dirname "$0")"
gcc -Os -s -fno-asynchronous-unwind-tables -fno-ident -fno-stack-protector \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Wl,-z,max-page-size=4096 -Wl,-z,noseparate-code -Wl,--build-id=none \
    -o watcher demo.c -lm
gcc -Os -s -mcmodel=tiny -no-pie -fomit-frame-pointer \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -fno-stack-protector \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--no-eh-frame-hdr \
    -Wl,-z,norelro -Wl,--hash-style=gnu -Wl,-z,max-page-size=4096 -Wl,-z,noseparate-code -Wl,--build-id=none \
    -o watcher_v2 demo_v2.c -lm
ls -l watcher watcher_v2
