#pragma once

#include <stdlib.h>
#include <sys/types.h>

// Overrides the file the benchmark writes results to. The default is a
// timestamped file in results/. Must be called before the first save_result.
void set_result_file(const char *path);

// Saves the result of the analysis to a csv file
void save_result(const char *algo_name, char *cnf_str, char *graph_str, size_t source_size, size_t chunk, size_t result,
                 ssize_t max_memory_kb, double time_s);
