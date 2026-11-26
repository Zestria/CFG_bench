#pragma once

#include <GraphBLAS.h>
#include <LAGraph.h>
#include <stdbool.h>

// single method for sets source nodes
GrB_Info adapter_CFL_init_src_nodes_common(GrB_Index **srcs, size_t *source_count, size_t graph_size);
GrB_Info adapter_CFL_set_src_nodes_common(GrB_Index **srcs, size_t *source_count, const GrB_Index *sources,
                                          size_t count);
