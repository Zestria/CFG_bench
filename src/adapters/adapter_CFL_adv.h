#pragma once

#include "adapter.h"
#include <GraphBLAS.h>
#include <LAGraph.h>

typedef struct {
    int8_t optimizations;
} CFL_adv_PrepareData;

AdapterMethods adapter_CFL_adv_get_methods(void);

/* Changes for step 1 */
GrB_Matrix *adv_state_get_outputs();

GrB_Index adv_state_get_graph_size();
/*Changes for step 1 */
