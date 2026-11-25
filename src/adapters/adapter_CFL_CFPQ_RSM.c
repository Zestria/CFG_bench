// code from DanyaLitva's CFG_Bench fork
// https://github.com/DanyaLitva/CFG_bench/blob/main/testAP.c

#include "adapter_CFL_CFPQ_RSM.h"
#include "GraphBLAS.h"
#include "LAGraph.h"
#include "adapter_CFL_common.h"
#include "adapter_CFL_multsrc_common.h"
#include "parser.h"
#include "rsm_file.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TRY(GrB_method)                                                                                                \
    {                                                                                                                  \
        GrB_Info LG_GrB_Info = GrB_method;                                                                             \
        if (LG_GrB_Info < GrB_SUCCESS) {                                                                               \
            fprintf(stderr, "LAGraph failure (file %s, line %d): (%d, msg: %s) \n", __FILE__, __LINE__, LG_GrB_Info,   \
                    state.msg);                                                                                        \
            return (LG_GrB_Info);                                                                                      \
        }                                                                                                              \
    }

// inner state of the adapter
typedef struct {
    RSM rsm;
    GrB_Vector *reachable;
    GrB_Matrix *adj_matrices;
    GrB_Index *sources;
    size_t sources_num;
    GrB_Index V; // Graph size
    char msg[LAGRAPH_MSG_LEN];
} state_t;

static state_t state;

// optional shared library loaded with --rsm-lib; when present its
// LAGraph_CFPQ_RSM replaces the one from liblagraphx
typedef int (*cfpq_rsm_fn)(GrB_Vector *, const RSM *, const GrB_Matrix *, const GrB_Index *, size_t, GrB_Index, char *);
static void *variant_lib = NULL;
static cfpq_rsm_fn variant_run = NULL;

// order-independent hash of the reachable set, used by the lab runner
static uint64_t result_hash = 0;

static uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t hash_reachable(GrB_Vector reachable) {
    GrB_Index nvals = 0;
    if (GrB_Vector_nvals(&nvals, reachable) != GrB_SUCCESS) {
        return 0;
    }

    uint64_t hash = splitmix64((uint64_t)nvals);
    if (nvals == 0) {
        return hash;
    }

    GrB_Index *indices = malloc((size_t)nvals * sizeof(*indices));
    bool *values = malloc((size_t)nvals * sizeof(*values));
    if (indices == NULL || values == NULL) {
        free(indices);
        free(values);
        return 0;
    }

    GrB_Index count = nvals;
    if (GrB_Vector_extractTuples_BOOL(indices, values, &count, reachable) != GrB_SUCCESS) {
        free(indices);
        free(values);
        return 0;
    }

    // XOR is order-independent, so the hash does not depend on the
    // GraphBLAS tuple extraction order.
    for (GrB_Index i = 0; i < count; i++) {
        if (values[i]) {
            hash ^= splitmix64((uint64_t)indices[i]);
        }
    }

    free(indices);
    free(values);
    return hash;
}

static GrB_Info adapter_CFL_setup(void) {
    TRY(LAGr_Init(GrB_NONBLOCKING, malloc, NULL, NULL, free, state.msg));

    return GrB_SUCCESS;
}

// prepare the adapter for use with the given parser result
// this may include converting the grammar and graph into a format suitable for the algorithm
//
// this modify ther inner state of the adapter
//
// adapter_CFL_prepare should be called just once for each config
static GrB_Info adapter_CFL_prepare(const ParserResult *parser_result, void *prepare_data) {
    CFL_CFPQ_RSM_PrepareData *data = (CFL_CFPQ_RSM_PrepareData *)prepare_data;

    if (variant_lib != NULL) {
        dlclose(variant_lib);
        variant_lib = NULL;
        variant_run = NULL;
    }
    if (data != NULL && data->rsm_lib != NULL) {
        variant_lib = dlopen(data->rsm_lib, RTLD_NOW | RTLD_LOCAL);
        if (variant_lib == NULL) {
            fprintf(stderr, "failed to load RSM variant library %s: %s\n", data->rsm_lib, dlerror());
            return GrB_INVALID_VALUE;
        }
        void *symbol = dlsym(variant_lib, "LAGraph_CFPQ_RSM");
        if (symbol == NULL) {
            fprintf(stderr, "LAGraph_CFPQ_RSM not found in %s: %s\n", data->rsm_lib, dlerror());
            dlclose(variant_lib);
            variant_lib = NULL;
            return GrB_INVALID_VALUE;
        }
        memcpy(&variant_run, &symbol, sizeof(symbol));
    }

    ParserResult working_copy = parser_result_copy(parser_result);
    Grammar *grammar = &working_copy.grammar;
    Graph *graph = &working_copy.graph;
    SymbolList *list = &working_copy.symbols;

    grammar_to_WCNF(grammar, list);
    SymbolList terms = symbol_list_create();
    SymbolList nonterms = symbol_list_create();

    grammar_split_terms_nonterms(grammar, list, &terms, &nonterms);

    // change term indecies in graph
    for (size_t i = 0; i < graph->edge_count; i++) {
        graph->edges[i].term_index = symbol_list_get_index_str(&terms, list->symbols[graph->edges[i].term_index].label);
    }

    explode_indices_CFL(grammar, graph, &nonterms, &terms);

    // the RSM takes over "terms" even when it fails to parse
    CFG_RSM *rsm = rsm_from_file(parser_result->rsm_path, parser_result->block_count, &terms);
    if (rsm == NULL) {
        symbol_list_free(&nonterms);
        free_parser_result(&working_copy);
        return GrB_INVALID_VALUE;
    }

    GrB_Matrix *prepared_adj_matrices = calloc(rsm->terms.count, sizeof(GrB_Matrix));
    for (size_t i = 0; i < rsm->terms.count; i++) {
        TRY(GrB_Matrix_new(prepared_adj_matrices + i, GrB_BOOL, graph->node_count, graph->node_count));
    }

    GrB_Scalar true_scalar;
    TRY(GrB_Scalar_new(&true_scalar, GrB_BOOL));
    TRY(GrB_Scalar_setElement_BOOL(true_scalar, true));

    GrB_Index *row = malloc(sizeof(GrB_Index) * graph->edge_count);
    GrB_Index *col = malloc(sizeof(GrB_Index) * graph->edge_count);
    for (size_t i = 0; i < rsm->terms.count; i++) {
        int count = 0;

        for (int j = 0; j < graph->edge_count; j++) {
            if (i == graph->edges[j].term_index) {
                row[count] = graph->edges[j].u;
                col[count] = graph->edges[j].v;
                count++;
            }
        }

        TRY(GxB_Matrix_build_Scalar(prepared_adj_matrices[i], row, col, true_scalar, count));
#ifdef DEBUG_parser
        GxB_print(prepared_adj_matrices[i], 1);
#endif
    }

    state.rsm = rsm_convert_to_lagraph(rsm);
    state.adj_matrices = prepared_adj_matrices;
    state.V = graph->node_count;

    if (data->use_start_nodes) {
        state.sources_num = parser_result->start_nodes_count;
        state.sources = NULL;

        if (state.sources_num > 0) {
            state.sources = malloc(state.sources_num * sizeof(*state.sources));
            if (state.sources == NULL) {
                fprintf(stderr, "out of memory\n");
                abort();
            }
            memcpy(state.sources, parser_result->start_nodes, state.sources_num * sizeof(*state.sources));
        }
    } else {
        TRY(adapter_CFL_init_src_nodes_common(&state.sources, &state.sources_num, graph->node_count));
    }

    free(row);
    free(col);
    GrB_free(&true_scalar);

    symbol_list_free(&nonterms);

    rsm_free(rsm);
    free_parser_result(&working_copy);

    return GrB_SUCCESS;
}

// initialize output matrices
//
// this should be called before each run of the algorithm
static GrB_Info adapter_CFL_init_outputs(void) {
    state.reachable = calloc(1, sizeof(GrB_Vector));
    return GrB_SUCCESS;
}

// run the algorithm
//
// this should be called after adapter_CFL_adv_init_outputs
static GrB_Info adapter_CFL_run(void) {
    int info = variant_run != NULL ? variant_run(state.reachable, &state.rsm, state.adj_matrices, state.sources,
                                                 state.sources_num, state.V, state.msg)
                                   : LAGraph_CFPQ_RSM(state.reachable, &state.rsm, state.adj_matrices, state.sources,
                                                      state.sources_num, state.V, state.msg);
    if (info != GrB_SUCCESS) {
        return info;
    }

    // The algorithm leaves the final GrB_mxv pending. Wait for it here so the
    // benchmark timer includes the materialization of the result.
    TRY(GrB_Vector_wait(*state.reachable, GrB_MATERIALIZE));

    return GrB_SUCCESS;
}

// check if the result of the algorithm is valid
//
// TODO: now check only count of reachibility pairs, make this more generic for other adapters
static ResultType adapter_CFL_is_result_valid(size_t valid_result) {
    (void)valid_result;
    return RESULT_UNKNOWN;
    // bool is_valid = false;
    // TRY(adapter_CFL_is_result_valid_common(state.outputs[0], valid_result, &is_valid));
    // return is_valid;
}

// TODO: now this is the same as adapter_CFL_adv_is_result_valid, make this more generic for other adapters
static size_t adapter_CFL_get_result(void) {
    GrB_Index result = 0;
    TRY(GrB_Vector_nvals(&result, *state.reachable));
    result_hash = hash_reachable(*state.reachable);
    return result;
}

static uint64_t adapter_CFL_get_result_hash(void) { return result_hash; }

static GrB_Info adapter_CFL_CFPQ_RSM_take_destinations(GrB_Vector *destinations) {
    *destinations = *state.reachable;
    *state.reachable = NULL;
    return GrB_SUCCESS;
}

// free output matrices
//
// this should be called after each run of the algorithm
static GrB_Info adapter_CFL_free_outputs(void) {
    TRY(GrB_free(state.reachable));
    free(state.reachable);

    return GrB_SUCCESS;
}

// free all resources allocated by the adapter except outputs
//
// this should be called after all runs of the algorithm for the given config
static GrB_Info adapter_CFL_cleanup(void) {
    TRY(adapter_CFL_cleanup_common(&state.adj_matrices, state.rsm.terminal_count, (void **)NULL));
    rsm_lagraph_rsm_free(&state.rsm);
    free(state.sources);
    state.sources = NULL;
    state.sources_num = 0;

    if (variant_lib != NULL) {
        dlclose(variant_lib);
        variant_lib = NULL;
        variant_run = NULL;
    }

    return GrB_SUCCESS;
}

// free LAGraph\GraphBLAS resources
static GrB_Info adapter_CFL_teardown(void) {
    if (variant_lib != NULL) {
        dlclose(variant_lib);
        variant_lib = NULL;
        variant_run = NULL;
    }
    TRY(LAGraph_Finalize(state.msg));
    return GrB_SUCCESS;
}

// get the methods of the adapter
AdapterMethods adapter_CFL_CFPQ_RSM_get_methods(void) {
    AdapterMethods methods = {.setup = adapter_CFL_setup,
                              .teardown = adapter_CFL_teardown,
                              .init_outputs = adapter_CFL_init_outputs,
                              .free_outputs = adapter_CFL_free_outputs,
                              .run = adapter_CFL_run,
                              .prepare = adapter_CFL_prepare,
                              .cleanup = adapter_CFL_cleanup,
                              .is_result_valid = adapter_CFL_is_result_valid,
                              .get_result = adapter_CFL_get_result,
                              .take_destinations = adapter_CFL_CFPQ_RSM_take_destinations,
                              .get_result_hash = adapter_CFL_get_result_hash};
    return methods;
}
