#!/usr/bin/env bash
set -eu

REPO_ROOT=/home/hollow/repo
DEPS_DIR="$REPO_ROOT/CFG_bench/build/dependencies"
GRAPHBLAS_SOURCE="$REPO_ROOT/GraphBLAS"
LAGRAPH_SOURCE="$REPO_ROOT/LAGraph"
BUILD_TYPE=Release
JOBS=12

GRAPHBLAS_BUILD="$DEPS_DIR/graphblas"
RSM_DIR="$LAGRAPH_SOURCE/experimental/algorithm"
CMAKE_FLAGS=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF)

if [[ ! -f "$RSM_DIR/LAGraph_CFPQ_RSM_old.c" ]]; then
    printf 'Missing old RSM source: %s\n' "$RSM_DIR/LAGraph_CFPQ_RSM_old.c" >&2
    exit 1
fi

mkdir -p "$DEPS_DIR"

# LAGraph collects algorithm/*.c. Keep one RSM implementation per build.
cat > "$DEPS_DIR/select_rsm.cmake" <<'CMAKE'
function(select_rsm)
    get_target_property(sources LAGraphX SOURCES)
    list(REMOVE_ITEM sources "${RSM_SOURCE_TO_EXCLUDE}")
    set_property(TARGET LAGraphX PROPERTY SOURCES "${sources}")
endfunction()
cmake_language(DEFER CALL select_rsm)
CMAKE

cmake -S "$GRAPHBLAS_SOURCE" -B "$GRAPHBLAS_BUILD" "${CMAKE_FLAGS[@]}" -DSUITESPARSE_USE_CUDA=OFF
cmake --build "$GRAPHBLAS_BUILD" --target GraphBLAS --parallel "$JOBS"

build_lagraph() {
    local build="$DEPS_DIR/$1"
    cmake -S "$LAGRAPH_SOURCE" -B "$build" "${CMAKE_FLAGS[@]}" \
        -DBUILD_TESTING=OFF -DGraphBLAS_DIR="$GRAPHBLAS_BUILD" \
        -DCMAKE_PROJECT_LAGraph_INCLUDE="$DEPS_DIR/select_rsm.cmake" \
        -DRSM_SOURCE_TO_EXCLUDE="$RSM_DIR/$2"
    cmake --build "$build" --target LAGraphX --parallel "$JOBS"
}

build_lagraph lagraph-optimized LAGraph_CFPQ_RSM_old.c
build_lagraph lagraph-old LAGraph_CFPQ_RSM.c
