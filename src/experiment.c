#define _POSIX_C_SOURCE 200809L

#include "experiment.h"
#include "registry.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ROUNDS = 3 };

static bool parse_uint64(const char *text, uint64_t *value) {
    if (text == NULL || *text == '\0' || text[strspn(text, "0123456789")] != '\0') {
        return false;
    }
    errno = 0;
    uintmax_t number = strtoumax(text, NULL, 10);
    if (errno != 0 || number > UINT64_MAX) {
        return false;
    }
    *value = (uint64_t)number;
    return true;
}

static uint64_t random_index(uint64_t *seed, uint64_t bound) {
    // Reject the short first bin so each index has the same probability.
    uint64_t threshold = -bound % bound;
    for (;;) {
        // SplitMix64 provides a reproducible 64-bit stream.
        *seed += UINT64_C(0x9e3779b97f4a7c15);
        uint64_t value = *seed;
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        if (value >= threshold) {
            return value % bound;
        }
    }
}

static GrB_Info check_destinations(const AdapterMethods *adapter, GrB_Vector *reference) {
    GrB_Vector actual = NULL;
    GrB_Info status = adapter->take_destinations(&actual);
    if (status == GrB_SUCCESS) {
        if (*reference == NULL) {
            *reference = actual;
            return GrB_SUCCESS;
        }
        bool equal = false;
        char msg[LAGRAPH_MSG_LEN];
        status = LAGraph_Vector_IsEqual(&equal, *reference, actual, msg);
        if (status == GrB_SUCCESS && !equal) {
            fprintf(stderr, "Reachable destination sets differ\n");
            status = GrB_INVALID_VALUE;
        }
    }
    GrB_Vector_free(&actual);
    return status;
}

static GrB_Info measure_round(const AdapterMethods *adapter, uint64_t *elapsed, GrB_Vector *reference) {
    GrB_Info status = adapter->init_outputs();
    if (status != GrB_SUCCESS) {
        return status;
    }

    struct timespec start, end;
    if (elapsed != NULL && clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        status = GrB_PANIC;
    } else {
        status = adapter->run();
    }
    if (status == GrB_SUCCESS) {
        status = adapter->wait_outputs();
    }
    if (status == GrB_SUCCESS && elapsed != NULL) {
        if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
            perror("clock_gettime");
            status = GrB_PANIC;
        } else {
            uint64_t seconds = (uint64_t)(end.tv_sec - start.tv_sec);
            *elapsed = seconds * UINT64_C(1000000000) + (uint64_t)end.tv_nsec - (uint64_t)start.tv_nsec;
            status = check_destinations(adapter, reference);
        }
    }

    GrB_Info cleanup = adapter->free_outputs();
    return status == GrB_SUCCESS ? cleanup : status;
}

static GrB_Info measure_algorithm(const AdapterMethods *adapter, const GrB_Index *sources, size_t count,
                                  uint64_t times[ROUNDS], GrB_Vector *reference) {
    GrB_Info status = adapter->set_sources(sources, count);
    if (status == GrB_SUCCESS) {
        status = measure_round(adapter, NULL, reference);
    }
    for (size_t i = 0; i < ROUNDS && status == GrB_SUCCESS; i++) {
        status = measure_round(adapter, &times[i], reference);
    }
    return status;
}

static GrB_Info measure_pairs(FILE *output, const AdapterMethods *cfl, const AdapterMethods *rsm,
                              const GrB_Index *samples, size_t source_size, size_t chunks) {
    fputs("query,sources,cfl_ns_1,cfl_ns_2,cfl_ns_3,rsm_ns_1,rsm_ns_2,rsm_ns_3\n", output);
    for (size_t i = 0; i < chunks; i++) {
        const GrB_Index *sources = samples + i * source_size;
        GrB_Vector reference = NULL;
        uint64_t times[2][ROUNDS];
        GrB_Info status = measure_algorithm(cfl, sources, source_size, times[0], &reference);
        if (status != GrB_SUCCESS) {
            fprintf(stderr, "CFL_multsrc_adv failed at query %zu\n", i + 1);
        } else {
            status = measure_algorithm(rsm, sources, source_size, times[1], &reference);
            if (status != GrB_SUCCESS) {
                fprintf(stderr, "CFL_CFPQ_RSM failed at query %zu\n", i + 1);
            }
        }
        GrB_Vector_free(&reference);
        if (status != GrB_SUCCESS) {
            return status;
        }

        fprintf(output, "%zu,\"%" PRIu64, i + 1, (uint64_t)sources[0]);
        for (size_t j = 1; j < source_size; j++) {
            fprintf(output, " %" PRIu64, (uint64_t)sources[j]);
        }
        fputc('"', output);
        for (size_t algorithm = 0; algorithm < 2; algorithm++) {
            for (size_t round = 0; round < ROUNDS; round++) {
                fprintf(output, ",%" PRIu64, times[algorithm][round]);
            }
        }
        fputc('\n', output);
        if (fflush(output) != 0 || ferror(output)) {
            perror("Writing experiment results");
            return GrB_PANIC;
        }
    }
    return GrB_SUCCESS;
}

// Takes ownership of parsed. The sample array is independent of the parsed pool.
static GrB_Info run_point(FILE *output, ParserResult parsed, const GrB_Index *samples, size_t source_size,
                          size_t chunks) {
    const AlgorithmEntry *cfl = registry_find("CFL_multsrc_adv");
    const AlgorithmEntry *rsm = registry_find("CFL_CFPQ_RSM");
    AdapterMethods cfl_adapter = cfl->get_methods();
    AdapterMethods rsm_adapter = rsm->get_methods();
    AlgorithmOptions cfl_options = {.optimizations = OPT_EMPTY | OPT_FORMAT | OPT_LAZY | OPT_BLOCK,
                                    .use_start_nodes = true};
    AlgorithmOptions rsm_options = {.batch_terminal_transitions = true, .use_start_nodes = true};

    // GraphBLAS is initialized once for both adapters. Each prepares its own matrices.
    GrB_Info status = cfl_adapter.setup();
    if (status != GrB_SUCCESS) {
        free_parser_result(&parsed);
        return status;
    }
    ParserResult input = parsed;
    input.start_nodes = NULL;
    input.start_nodes_count = 0;
    status = cfl->prepare(&cfl_adapter, &input, &cfl_options);
    if (status == GrB_SUCCESS) {
        status = rsm->prepare(&rsm_adapter, &input, &rsm_options);
    }
    free_parser_result(&parsed);

    if (status == GrB_SUCCESS) {
        status = measure_pairs(output, &cfl_adapter, &rsm_adapter, samples, source_size, chunks);
    }
    GrB_Info cfl_cleanup = cfl_adapter.cleanup();
    GrB_Info rsm_cleanup = rsm_adapter.cleanup();
    GrB_Info teardown = cfl_adapter.teardown();
    if (status != GrB_SUCCESS) {
        return status;
    }
    if (cfl_cleanup != GrB_SUCCESS) {
        return cfl_cleanup;
    }
    return rsm_cleanup == GrB_SUCCESS ? teardown : rsm_cleanup;
}

int run_experiment(char *config_path, const ExperimentArgs *args) {
    uint64_t source_size, chunks, seed;
    if (config_path == NULL || args->results_file == NULL || !parse_uint64(args->source_size, &source_size) ||
        !parse_uint64(args->chunks, &chunks) || !parse_uint64(args->seed, &seed) || source_size == 0 ||
        source_size > INT32_MAX || chunks == 0 || chunks > SIZE_MAX / sizeof(GrB_Index) / source_size) {
        fprintf(stderr, "--experiment requires -c, --source-size, --chunks, --seed and --results-file\n"
                        "Source size and chunks must be positive and the sample array must fit in memory\n");
        return EXIT_FAILURE;
    }

    char *config_text;
    size_t count;
    config_row *configs = get_configs_from_file(config_path, &count, &config_text);
    if (count != 1 || configs[0].start_nodes_path == NULL) {
        fprintf(stderr, "--experiment requires one config row with an eligible pool in column four\n");
        free(configs);
        free(config_text);
        return EXIT_FAILURE;
    }
    ParserResult parsed = parser(configs[0], false);
    free(configs);
    free(config_text);

    if (parsed.start_nodes_count == 0 || source_size > parsed.graph.node_count) {
        fprintf(stderr, "Expected a nonempty pool and a source size no larger than the graph\n");
        free_parser_result(&parsed);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < parsed.start_nodes_count; i++) {
        if (parsed.start_nodes[i] >= parsed.graph.node_count ||
            (i > 0 && parsed.start_nodes[i - 1] >= parsed.start_nodes[i])) {
            fprintf(stderr, "Expected strictly increasing pool vertices within the graph\n");
            free_parser_result(&parsed);
            return EXIT_FAILURE;
        }
    }

    size_t sample_count = (size_t)chunks * (size_t)source_size;
    GrB_Index *samples = malloc(sample_count * sizeof(*samples));
    if (samples == NULL) {
        perror("Allocating experiment sources");
        free_parser_result(&parsed);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = parsed.start_nodes[random_index(&seed, parsed.start_nodes_count)];
    }

    FILE *output = fopen(args->results_file, "wx");
    if (output == NULL) {
        perror(args->results_file);
        free(samples);
        free_parser_result(&parsed);
        return EXIT_FAILURE;
    }
    GrB_Info status = run_point(output, parsed, samples, (size_t)source_size, (size_t)chunks);
    free(samples);
    if (fclose(output) != 0) {
        perror(args->results_file);
        return EXIT_FAILURE;
    }
    if (status != GrB_SUCCESS) {
        fprintf(stderr, "Experiment failed with GraphBLAS status %d\n", status);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
