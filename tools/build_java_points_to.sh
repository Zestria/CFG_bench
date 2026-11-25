#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
repos=$(dirname -- "$PWD")
graphblas=$repos/GraphBLAS
lagraph=$repos/LAGraph
out=$PWD/build/java-points-to
gb_build=$out/GraphBLAS
lg_build=$out/LAGraph
compiler=${CC:-gcc}
export CC="$compiler"

variant=$lagraph/experimental/algorithm/cfpq_rsm_lab/variants/active/p1_full_clean__655586ec2c10.c
printf '%s  %s\n' 655586ec2c107d5fc75d0f41bc0c132cdec7039803e3bedf24a674be6c40d682 "$variant" | sha256sum -c -

cmake -S "$graphblas" -B "$gb_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
    -DSUITESPARSE_USE_FORTRAN=OFF -DGRAPHBLAS_COMPACT=OFF
cmake --build "$gb_build" --target GraphBLAS

cmake -S "$lagraph" -B "$lg_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
    -DBUILD_TESTING=OFF -DGraphBLAS_DIR="$gb_build" \
    -DSUITESPARSE_USE_FORTRAN=OFF
cmake --build "$lg_build" --target LAGraphX

lg_core=$lg_build/src
lg_extra=$lg_build/experimental
make BUILD_DIR="$out" CC="$compiler" \
    INCLUDES="-I$graphblas/Include -I$lagraph/include -Isrc -Isrc/adapters" \
    LDLIBS="-L$lg_extra -L$lg_core -L$gb_build -Wl,-rpath,$lg_extra:$lg_core:$gb_build -llagraphx -llagraph -lgraphblas -ldl"

"$compiler" -O2 -fPIC -shared -std=c11 \
    -I"$graphblas/Include" -I"$lagraph/include" -I"$lagraph/src/utility" \
    "$variant" -o "$out/p1_full_clean.so" -L"$lg_core" -L"$gb_build" \
    -Wl,-rpath,"$lg_core:$gb_build" -llagraph -lgraphblas
