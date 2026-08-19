#include "GraphBLAS.h"

#define TRY(GrB_method)                                                                                                \
    {                                                                                                                  \
        GrB_Info LG_GrB_Info = GrB_method;                                                                             \
        if (LG_GrB_Info < GrB_SUCCESS) {                                                                               \
            fprintf(stderr, "LAGraph failure (file %s, line %d): (%d) \n", __FILE__, __LINE__, LG_GrB_Info);           \
            return (LG_GrB_Info);                                                                                      \
        }                                                                                                              \
    }

/* Changes for step 1 --> */
GrB_Info extract_reachable_sources(GrB_Matrix *outputs, size_t start_symbol_idx, size_t V, GrB_Index **srcs_out,
                                   size_t *srcs_count_out) {
    GrB_Matrix S = outputs[start_symbol_idx];

    GrB_Vector row_degrees;
    TRY(GrB_Vector_new(&row_degrees, GrB_INT64, V));
    TRY(GrB_Matrix_reduce_Monoid(row_degrees, NULL, NULL, GrB_PLUS_MONOID_INT64, S, NULL));

    GrB_Index nvals;
    TRY(GrB_Vector_nvals(&nvals, row_degrees));
    GrB_Index *indices = malloc(nvals * sizeof(GrB_Index));
    int64_t *values = malloc(nvals * sizeof(int64_t));

    if (!indices || !values) {
        free(indices);
        free(values);
        GrB_Vector_free(&row_degrees);
        fprintf(stderr, "out of memory\n");
        return GrB_OUT_OF_MEMORY;
    }

    TRY(GrB_Vector_extractTuples_INT64(indices, values, &nvals, row_degrees));

    *srcs_out = indices;
    *srcs_count_out = nvals;

    free(values);
    GrB_free(&row_degrees);
    return GrB_SUCCESS;
}
/* <-- Changes for step 1 */

GrB_Info adapter_CFL_init_src_nodes_common(GrB_Index **srcs, size_t *source_count,
                                           GrB_Index *reachable_pool, size_t start, size_t final) {
    *source_count = final - start + 1;
    *srcs = malloc(*source_count * sizeof(GrB_Index));
    if (!srcs) {
        fprintf(stderr, "out of memory");
    }

    for (size_t i = 0; i < *source_count; ++i) {
        (*srcs)[i] = reachable_pool[start + i];
    }

    return GrB_SUCCESS;
}
