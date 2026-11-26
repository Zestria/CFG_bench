#!/usr/bin/env bash
set -eu

REPO_ROOT=/home/hollow/repo
BUILD_DIR="$REPO_ROOT/CFG_bench/build"
DEPS_DIR="$BUILD_DIR/dependencies"
LAGRAPH_OPTIMIZED_BUILD="$DEPS_DIR/lagraph-optimized"
LAGRAPH_OLD_BUILD="$DEPS_DIR/lagraph-old"
LAGRAPH_INCLUDE="$REPO_ROOT/LAGraph/include"
GRAPHBLAS_BUILD="$DEPS_DIR/graphblas"
GRAPHBLAS_INCLUDE="$REPO_ROOT/GraphBLAS/Include"

PAIRS_CONFIG=configs/for_test.csv
PAIRS_OUTPUT_DIR="$BUILD_DIR/start_nodes"
export GRAPHBLAS_CACHE_PATH="$GRAPHBLAS_BUILD/jit-cache"

if [[ "$LAGRAPH_OPTIMIZED_BUILD/experimental/liblagraphx.so" -ef "$LAGRAPH_OLD_BUILD/experimental/liblagraphx.so" ]]; then
    echo 'Use distinct liblagraphx.so files for optimized and old.' >&2
    exit 1
fi

cd -- "$REPO_ROOT/CFG_bench"
unset LD_LIBRARY_PATH LD_PRELOAD

build_bench() {
    local variant=$1 lagraph_build=$2
    local output="$BUILD_DIR/$variant"
    local lagraphx="$lagraph_build/experimental" lagraph="$lagraph_build/src"

    make -B all BUILD_DIR="$output" \
        INCLUDES="-Isrc -Isrc/adapters -I\"$LAGRAPH_INCLUDE\" -I\"$GRAPHBLAS_INCLUDE\"" \
        LDLIBS="\"$lagraphx/liblagraphx.so\" \"$lagraph/liblagraph.so\" \"$GRAPHBLAS_BUILD/libgraphblas.so\" \
                -Wl,-rpath,\"$lagraphx:$lagraph:$GRAPHBLAS_BUILD\""
    ldd "$output/cfg_bench"
}

build_bench optimized "$LAGRAPH_OPTIMIZED_BUILD"
build_bench old "$LAGRAPH_OLD_BUILD"

mkdir -p -- "$PAIRS_OUTPUT_DIR" "$GRAPHBLAS_CACHE_PATH"
"$BUILD_DIR/optimized/pairs_extractor" -c "$PAIRS_CONFIG" -o "$PAIRS_OUTPUT_DIR" --start-only
