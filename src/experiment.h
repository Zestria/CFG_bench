#pragma once

typedef struct {
    const char *source_size;
    const char *chunks;
    const char *seed;
    const char *results_file;
} ExperimentArgs;

int run_experiment(char *config_path, const ExperimentArgs *args);
