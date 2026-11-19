#pragma once

#include "adapter.h"
#include <GraphBLAS.h>
#include <LAGraph.h>
#include <stdbool.h>

typedef struct {
    bool use_start_nodes;
    // optional path to a shared library that replaces LAGraph_CFPQ_RSM
    const char *rsm_lib;
} CFL_CFPQ_RSM_PrepareData;

AdapterMethods adapter_CFL_CFPQ_RSM_get_methods(void);
