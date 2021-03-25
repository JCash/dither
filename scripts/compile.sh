#!/usr/bin/env bash

BUILD_DIR=./build
mkdir -p $BUILD_DIR

clang++ -O0 -o $BUILD_DIR/dither ./src/dither.cpp