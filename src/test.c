#include "adapter_CFL.h"
#include "adapter_CFL_CFPQ_RSM.h"
#include "adapter_CFL_adv.h"
#include "adapter_CFL_all_path.h"
#include "adapter_CFL_all_path_adv.h"
#include "adapter_CFL_multsrc.h"
#include "adapter_CFL_multsrc_common.h"
#include "adapter_CFL_single_path.h"
#include "memory.h"
#include "parser.h"
#include "result_manager.h"
#include <GraphBLAS.h>
#include <LAGraph.h>
#include <LAGraphX.h>
#include <getopt.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define run_algorithm()                                                                                                \
    LAGraph_CFL_reachability_adv(outputs, adj_matrices, symbols_amount, grammar.rules, grammar.rules_count, msg,       \
                                 optimizations)

#define check_error(error)                                                                                             \
    {                                                                                                                  \
        retval = run_algorithm();                                                                                      \
        TEST_CHECK(retval == error);                                                                                   \
        TEST_MSG("retval = %d (%s)", retval, msg);                                                                     \
    }

#define check_result(result)                                                                                           \
    {                                                                                                                  \
        char *expected = output_to_str(0);                                                                             \
        TEST_CHECK(strcmp(result, expected) == 0);                                                                     \
        TEST_MSG("Wrong result. Actual: %s", expected);                                                                \
    }

#define TRY(GrB_method)                                                                                                \
    {                                                                                                                  \
        GrB_Info LG_GrB_Info = GrB_method;                                                                             \
        if (LG_GrB_Info < GrB_SUCCESS) {                                                                               \
            fprintf(stderr, "LAGraph failure (file %s, line %d): (%d) \n", __FILE__, __LINE__, LG_GrB_Info);           \
            return (LG_GrB_Info);                                                                                      \
        }                                                                                                              \
    }

GrB_Matrix *adj_matrices = NULL;
GrB_Matrix *outputs = NULL;
grammar_t grammar = {0, 0, NULL};
char msg[LAGRAPH_MSG_LEN];
size_t symbols_amount = 0;

void print_rules(Grammar grammar, SymbolList list) {
    for (size_t i = 0; i < grammar.rules_count; i++) {
        Rule rule = grammar.rules[i];
        if (rule.first != -1) {
            printf("%s ->", list.symbols[rule.first].label);
        }
        if (rule.second != -1) {
            printf(" %s", list.symbols[rule.second].label);
        }
        if (rule.third != -1) {
            printf(" %s", list.symbols[rule.third].label);
        }
        printf("\n");
    }
}

void print_list(SymbolList list, size_t *map) {
    if (map == NULL) {
        for (size_t i = 0; i < list.count; i++) {
            Symbol sym = list.symbols[i];
            printf("[%2ld] %s [%s] %s\n", i, sym.label, sym.is_nonterm ? "N" : "T", sym.is_indexed ? "[I]" : "");
        }
    } else {
        for (size_t i = 0; i < list.count; i++) {
            Symbol sym = list.symbols[i];
            printf("[%2ld] %s [%s] %s\n", map[i], sym.label, sym.is_nonterm ? "N" : "T", sym.is_indexed ? "[I]" : "");
        }
    }
}

#define RESET "\033[0m"
#define BLACK "\033[30m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

// Use your custom configuration for the benchmark (default is the xz.g graph
// and vf.cnf grammar)
#define configs_macro configs_java

#define OPT_EMPTY (1 << 0)
#define OPT_FORMAT (1 << 1)
#define OPT_LAZY (1 << 2)
#define OPT_BLOCK (1 << 3)

enum { HOT_OPTION = 1000 };

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s -c <config file> [options]\n"
            "\n"
            "Required:\n"
            "  -c <config file>  Path to benchmark config file\n"
            "\n"
            "Benchmark options:\n"
            "  -r <rounds>       Number of benchmark rounds (default: 10)\n"
            "  --hot             Enable HOT launch (warm-up run before measurements)\n"
            "  -a <algorithm>    Algorithm to use "
            "(default: CFL_adv; options: CFL_adv, CFL, CFL_single_path, CFL_all_path, CFL_all_path_adv, CFL_CFPQ_RSM, "
            "CFL_multsrc)\n"
            "\n"
            "Optimization flags:\n"
            "  -e                Enable empty optimization\n"
            "  -f                Enable format optimization\n"
            "  -l                Enable lazy optimization\n"
            "  -b                Enable block optimization\n"
            "\n"
            "Other:\n"
            "  -t                Enable test mode\n"
            "  -h                Print this help message\n"
            "\n"
            "Example:\n"
            "  %s -c configs/configs_my.csv -r 10 --hot\n",
            program_name, program_name);
}

/* Step 2 */
void put_vertices_to_file(char *grammar, char *graph, GrB_Index *reachable, GrB_Index reachable_count) {
    char *grammar_name = strrchr(grammar, '/');
    size_t grammar_name_size = strlen(grammar_name) -4; // remove extension .cnf 
    
    char *graph_name = strrchr(graph, '/');
    size_t graph_name_size = strlen(graph_name) -2; // remove extension .g 
    
    size_t n = strlen("./computated_data") + grammar_name_size + graph_name_size + 1;
    
    char *filename = (char *)calloc(n,sizeof(char));
    
    strcat(filename, "./computated_data");
    strncat(filename, grammar_name, grammar_name_size);
    strncat(filename, graph_name, graph_name_size);
    // printf("debug: creating file %s\n", filename);

    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "error: failed to open file.");
        abort();
    }
    /*char number[20];
    memset(number, 0, 20);
    sprintf(number, "%zu", reachable_count);
    fwrite(number, strlen(number) * sizeof(char), 1, file);
    fwrite("\n", strlen("\n") * sizeof(char), 1, file);

    for (int i = 0; i < reachable_count; ++i) {
        memset(number, 0, 20);
        sprintf(number, "%zu", reachable[i]);
        fwrite(number, strlen(number) * sizeof(char), 1, file);
        fwrite(" ", strlen("\n") * sizeof(char), 1, file);
    }
    fwrite("\n", strlen("\n") * sizeof(char), 1, file);*/
    fwrite(&reachable_count, sizeof(GrB_Index), 1, file);
    fwrite(reachable, sizeof(GrB_Index), reachable_count, file);

    fclose(file);
}
/* Step 2 */
/* Step 3 */
void get_vertices_from_file(char *grammar, char *graph, GrB_Index **reachable, size_t *reachable_count) {
    char *grammar_name = strrchr(grammar, '/');
    size_t grammar_name_size = strlen(grammar_name) -4; // remove extension .cnf 
    
    char *graph_name = strrchr(graph, '/');
    size_t graph_name_size = strlen(graph_name) -2; // remove extension .g 
    
    size_t n = strlen("./computated_data") + grammar_name_size + graph_name_size + 1;
    
    char *filename = (char *)calloc(n,sizeof(char));
    
    strcat(filename, "./computated_data");
    strncat(filename, grammar_name, grammar_name_size);
    strncat(filename, graph_name, graph_name_size);
    // printf("debug: reading file %s...\n", filename);

    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "error: failed to open file.");
        abort();
    }
    fread(reachable_count, sizeof(size_t), 1, file);
    // printf("debug: reachable_count = %zu\n", *reachable_count);

    *reachable = (GrB_Index *)malloc(*reachable_count * sizeof(GrB_Index));
    // printf("debug: reachable array size %zu\n",sizeof(*reachable));

    size_t retval = fread(*reachable, sizeof(GrB_Index), *reachable_count, file);
    // printf("debug: retval = %zu\n", retval);
    if (retval != *reachable_count) {
        printf("error: fread failed. retval = %zu\n", retval);
    }

    fclose(file);
}

#define SEED 2430986565

#define MIN(A, B) (((A) < (B)) ? (A) : (B))

void permute_elements(GrB_Index **reachable, size_t reachable_count) {
    srand((unsigned int)SEED);
    
    for (size_t i = 0; i < reachable_count; ++i) {
        size_t j = i + rand() % (reachable_count - i);
        if (j >= reachable_count) {
            printf("error: segfault\n");
        }
        GrB_Index temp = (*reachable)[i];
        (*reachable)[i] = (*reachable)[j];
        (*reachable)[j] = temp;
    }
}
/* Step 3 */

int main(int argc, char **argv) {
    GrB_Info retval = GrB_SUCCESS;
    int8_t optimizations = 0;
    int opt;
    bool is_test = false;
    bool is_hot_enabled = false;
    bool is_config = false;
    char *algo = NULL;
    bool is_algo_chosen = false;
    char *input_config = NULL;
    size_t rounds_count = 10;

    /* Step 2 */
    const size_t source_sizes[] = {1, 10, 100, 0};
    const size_t chunks_num[] = {30, 30, 30};
    /* Step 2 */

    AdapterMethods adapter = {0};

    static struct option long_options[] = {{"hot", no_argument, 0, HOT_OPTION}, {0, 0, 0, 0}};

    while ((opt = getopt_long(argc, argv, "eflbthr:c:a:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'e':
            optimizations |= OPT_EMPTY;
            break;
        case 'f':
            optimizations |= OPT_FORMAT;
            break;
        case 'l':
            optimizations |= OPT_LAZY;
            break;
        case 'b':
            optimizations |= OPT_BLOCK;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        case HOT_OPTION:
            is_hot_enabled = true;
            break;
        case 't':
            is_test = true;
            break;
        case 'r':
            rounds_count = strtoul(optarg, NULL, 10);
            if (rounds_count == 0) {
                fprintf(stderr, "Rounds count must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            printf("Choosen rounds count: %zu\n", rounds_count);
            break;
        case 'c':
            is_config = true;
            input_config = optarg;
            printf("Choosen config: %s\n", input_config);
            break;
        case 'a':
            is_algo_chosen = true;
            algo = optarg;
            printf("Choosen algorithm: %s\n", algo);

            if (strcmp(algo, "CFL_adv") == 0) {
                adapter = adapter_CFL_adv_get_methods();
            } else if (strcmp(algo, "CFL") == 0) {
                adapter = adapter_CFL_get_methods();
            } else if (strcmp(algo, "CFL_single_path") == 0) {
                adapter = adapter_CFL_single_path_get_methods();
            } else if (strcmp(algo, "CFL_all_path") == 0) {
                adapter = adapter_CFL_all_paths_get_methods();
            } else if (strcmp(algo, "CFL_CFPQ_RSM") == 0) {
                adapter = adapter_CFL_CFPQ_RSM_get_methods();
            } else if (strcmp(algo, "CFL_multsrc") == 0) {
                adapter = adapter_CFL_multsrc_get_methods();
            } else if (strcmp(algo, "CFL_all_path_adv") == 0) {
                adapter = adapter_CFL_all_path_adv_get_methods();
            } else {
                fprintf(stderr, "Unknown algorithm: %s\n", algo);
                exit(EXIT_FAILURE);
            }
            break;
        default:
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    GrB_init(GrB_NONBLOCKING);

    // Check current thread limit
    pthread_t nthreads;
    GxB_Global_Option_get(GxB_GLOBAL_NTHREADS, &nthreads);
    printf("GraphBLAS max threads allowed: %zu\n", nthreads);

    if (!is_algo_chosen) {
        adapter = adapter_CFL_adv_get_methods();
        algo = "CFL_adv";
        printf("No algorithm chosen, using CFL_adv by default\n");
    }

    TRY(adapter.setup());

    /* Step 1 */
    AdapterMethods adv_adapter = {0};
    adv_adapter = adapter_CFL_adv_get_methods();
    TRY(adv_adapter.setup());
    /* Step 1 */

    if (!is_config) {
        fprintf(stderr, "Need to choose config by flag -c [config file]\n");
        exit(EXIT_FAILURE);
    }

    size_t configs_count = 0;
    config_row *configs = calloc(1000, sizeof(config_row));
    char *config_text;
    get_configs_from_file(input_config, &configs_count, configs, &config_text);

    printf("Start bench\n");
    fflush(stdout);

    for (size_t i = 0; i < configs_count; i++) {
        config_row config = configs[i];
        printf("CONFIG: grammar: %s, graph: %s\n", config.grammar, config.graph);
        fflush(stdout);

        GrB_Index *reachable = NULL;
        size_t reachable_count = 0;

        /* Step 1 start. Run CFL_adv to get reachable vertices */
/*
        ParserResult parser_result_adv = parser(config);
        adv_adapter.prepare(parser_result_adv, &(CFL_adv_PrepareData){.optimizations = optimizations});

        TRY(adv_adapter.init_outputs());

        malloc_trim(0);
        if (mem_peak_reset() != 0) {
            fprintf(stderr, "Failed to reset memory peak\n");
            exit(EXIT_FAILURE);
        }

        retval = adv_adapter.run();

        TRY(extract_reachable_sources(adv_state_get_outputs(), 0, adv_state_get_graph_size(), &reachable,
                                      &reachable_count));

        TRY(adv_adapter.free_outputs());
        TRY(adv_adapter.cleanup());
*/
        /* Step 1 end. */

        /* Step 2 start. Save start_vertices to the file. */
/*
        printf("debug: write to file |reachable| = %zu\n", reachable_count);
        put_vertices_to_file(config.grammar, config.graph, reachable, reachable_count);
        free(reachable);
        reachable_count = 0;
*/
        /* Step 2 end. */

        /* Step 3 start. */
        get_vertices_from_file(config.grammar, config.graph, &reachable, &reachable_count);
        // printf("debug: got vertices from file\n"); 
        // a random permutation
        permute_elements(&reachable, reachable_count);

        // number of vertices for quering
        size_t queried_vertices_num = reachable_count;
        if (reachable_count > 3000) {
            queried_vertices_num = 3000;
        } 
        // printf("DEBUG: start cycle\n"); 

        bool is_hot = is_hot_enabled;

        for (size_t j = 0; source_sizes[j] != 0; ++j) {
            size_t _chunk_num_A = queried_vertices_num/source_sizes[j];
            size_t _chunk_num_B = chunks_num[j];
            for (size_t m = 0; m < MIN(_chunk_num_A, _chunk_num_B); ++m) {
                ParserResult parser_result = parser(config);
                adapter.prepare(parser_result, &(CFL_multsrc_PrepareData){.optimizations = optimizations,
                                                                          .reachable_srcs = reachable,
                                                                          .start = m*source_sizes[j],
                                                                          .final = (m+1)*source_sizes[j]-1});

                double *start = calloc(rounds_count, sizeof(double));
                double *end = calloc(rounds_count, sizeof(double));
                if (start == NULL || end == NULL) {
                    fprintf(stderr, "Failed to allocate memory for benchmark rounds\n");
                    free(start);
                    free(end);
                    exit(EXIT_FAILURE);
                }
                size_t result = 0;
                ssize_t max_memory_kb = 0;
                for (size_t k = 0; k < rounds_count; k++) {
                    TRY(adapter.init_outputs());

                    // in some cases free don't change memory usage, so we need to reset it manually
                    malloc_trim(0);
                    if (mem_peak_reset() != 0) {
                        fprintf(stderr, "Failed to reset memory peak\n");
                        exit(EXIT_FAILURE);
                    }

                    start[k] = LAGraph_WallClockTime();
#ifndef CI
                    retval = adapter.run();
#endif
                    end[k] = LAGraph_WallClockTime();
                    max_memory_kb = mem_get_peak_kb();

                    if (is_test) {
                        size_t result = adapter.get_result();
                        ResultType result_type = adapter.is_result_valid(config.valid_result);
                        char status[256];
                        switch (result_type) {
                        case RESULT_OK:
                            snprintf(status, sizeof(status), GREEN "[OK]" RESET);
                            break;
                        case RESULT_ERROR:
                            snprintf(status, sizeof(status), RED "[Wrong] (Result must be %ld)" RESET,
                                     config.valid_result);
                            break;
                        case RESULT_UNKNOWN:
                            snprintf(status, sizeof(status), YELLOW "[Unknown]" RESET);
                            break;
                        default:
                            fprintf(stderr, "Unknown result type: %d\n", result_type);
                            abort();
                        }

                        printf("\tResult: %ld (Return code: %d) %s", result, retval, status);

                        if (retval != 0) {
                            printf("\t(MSG: %s)", msg);
                        }
                        printf(" (%.4f sec)", end[k] - start[k]);

                        TRY(adapter.free_outputs());
                        break;
                    }

                    if (is_hot) {
                        is_hot = false;
                        k--;
                        TRY(adapter.free_outputs());
                        continue;
                    }

                    printf("\t%.3fs", end[k] - start[k]);
                    fflush(stdout);

                    result = adapter.get_result();
                    TRY(adapter.free_outputs());
                    // save_result(algo, config.grammar, config.graph, source_sizes[j], result, max_memory_kb,
                    //             (size_t)((end[k] - start[k]) * 1000));
                    // in some cases free don't change memory usage, so we need to reset it manually
                    malloc_trim(0);
                }
                printf("\n");

                if (rounds_count > 0) {
                    size_t min_round_time = (size_t)lround((end[0] - start[0])*1000.0);
                    for (size_t k = 0; k < rounds_count; ++k) {
                        min_round_time = MIN(min_round_time, (size_t)lround((end[k] - start[k])*1000.0));
                    }
                    save_result(algo, config.grammar, config.graph, source_sizes[j], 0, 0, min_round_time);
                }

                if (is_test) {
                    free(start);
                    free(end);
                    adapter.cleanup();

                    fflush(stdout);
                    continue;
                }

                double sum = 0;
                for (size_t k = 0; k < rounds_count; k++) {
                    sum += end[k] - start[k];
                }
                printf("\tTime elapsed (avg): %.6f seconds. %zd KB max memory. Result: %ld (return code "
                       "%d) (%s)\n\n",
                       sum / rounds_count, max_memory_kb, result, retval, msg);

                free(start);
                free(end);

                TRY(adapter.cleanup());

                fflush(stdout);
            }
        }
    }

    free(configs);
    free(config_text);
    TRY(adapter.teardown());
    return 0;
}
