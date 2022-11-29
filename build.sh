#! /bin/bash

cp dirtypipe-android ../app/src/main/jniLibs/arm64-v8a/libdirtypipe.so
cp -rf magisk ../app/src/main/assets/
cp -rf startup-root ../app/src/main/assets/
cp -rf env-patcher ../app/src/main/assets/
