#pragma once

#include <GraphBLAS.h>
#include <LAGraph.h>
#include <stdbool.h>

/* Changes for step 1 --> */
typedef struct {
    int8_t optimizations;
    GrB_Index *reachable_srcs; // NULL means use random
    size_t start;
    size_t final;
} CFL_multsrc_PrepareData;

GrB_Info extract_reachable_sources(GrB_Matrix *outputs, size_t start_symbol_idx, size_t V, GrB_Index **srcs_out,
                                   size_t *srcs_count_out);
/* <-- Changes for step 1 */

// single method for sets source nodes
GrB_Info adapter_CFL_init_src_nodes_common(GrB_Index **srcs, size_t *source_count,
                                           GrB_Index *reachable_pool, size_t start, size_t final);
