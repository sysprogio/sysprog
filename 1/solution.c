#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "libcoro.h"

struct coroutine_context {
    char *coroutine_name;
    char **file_list;
    int number_of_files;
    int *current_file_index;
    int **array_pointer;
    int *array_size;
    int start_time_sec;
    int start_time_nsec;
    int finish_time_sec;
    int finish_time_nsec;
    int total_time_sec;
    int total_time_nsec;
    int time_limit_nsec;
};

static struct coroutine_context *create_coroutine_context(const char *name, char **file_list, int number_of_files,
                                                          int *current_index, int **data_pointer, int *size_pointer, int limit) {
    struct coroutine_context *ctx = malloc(sizeof(*ctx));
    ctx->coroutine_name = strdup(name);
    ctx->file_list = file_list;
    ctx->current_file_index = current_index;
    ctx->number_of_files = number_of_files;
    ctx->array_pointer = data_pointer;
    ctx->array_size = size_pointer;
    ctx->time_limit_nsec = limit;
    return ctx;
}

static void delete_coroutine_context(struct coroutine_context *ctx) {
    free(ctx->coroutine_name);
    free(ctx);
}

static void stop_timer(struct coroutine_context *ctx) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    ctx->finish_time_sec = time.tv_sec;
    ctx->finish_time_nsec = time.tv_nsec;
}

static void start_timer(struct coroutine_context *ctx) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    ctx->start_time_sec = time.tv_sec;
    ctx->start_time_nsec = time.tv_nsec;
}

static void calculate_total_time(struct coroutine_context *ctx) {
    if (ctx->finish_time_sec >= ctx->start_time_sec) {
        ctx->total_time_sec += ctx->finish_time_sec - ctx->start_time_sec;
        ctx->total_time_nsec += ctx->finish_time_nsec - ctx->start_time_nsec;
    } else {
        ctx->total_time_sec += ctx->finish_time_sec - ctx->start_time_sec - 1;
        ctx->total_time_nsec += 1000000000 + ctx->finish_time_nsec - ctx->start_time_nsec;
    }
}

static bool is_exceeded_time_limit(struct coroutine_context *ctx) {
    stop_timer(ctx);
    int current_quant = (ctx->finish_time_sec - ctx->start_time_sec) * 1000000000 +
                        (ctx->finish_time_nsec - ctx->start_time_nsec);
    return current_quant > ctx->time_limit_nsec ? true : false;
}

void insertion_sort(int *array, int size, struct coroutine_context *ctx) {
    for (int i = 1; i < size; i++) {
        int key = array[i];
        int j = i - 1;

        while (j >= 0 && array[j] > key) {
            array[j + 1] = array[j];
            j = j - 1;
        }
        array[j + 1] = key;

        if (is_exceeded_time_limit(ctx)) {
            stop_timer(ctx);
            calculate_total_time(ctx);
            coro_yield();
            start_timer(ctx);
        }
    }
}

static int coroutine_function(void *context) {
    struct coro *current_coroutine = coro_this();
    struct coroutine_context *coroutine_context = context;
    start_timer(coroutine_context);

    while (*coroutine_context->current_file_index != coroutine_context->number_of_files) {
        char *filename = coroutine_context->file_list[*coroutine_context->current_file_index];
        FILE *file = fopen(filename, "r");
        if (!file) {
            delete_coroutine_context(coroutine_context);
            return 1;
        }

        int size = 0;
        int capacity = 100;
        int *array = malloc(capacity * sizeof(int));

        while (fscanf(file, "%d", array + size) == 1) {
            ++size;
            if (size == capacity) {
                capacity *= 2;
                array = realloc(array, capacity * sizeof(int));
            }
        }

        capacity = size;
        array = realloc(array, capacity * sizeof(int));

        fclose(file);

        coroutine_context->array_pointer[*coroutine_context->current_file_index] = array;
        coroutine_context->array_size[*coroutine_context->current_file_index] = size;

        ++(*coroutine_context->current_file_index);

        insertion_sort(array, size, coroutine_context);
    }

    stop_timer(coroutine_context);
    calculate_total_time(coroutine_context);

    printf("%s \nswitches %lld\ntime %.6f seconds\n\n",coroutine_context->coroutine_name,
           coro_switch_count(current_coroutine),
           (coroutine_context->total_time_sec + coroutine_context->total_time_nsec / 1e9)
    );

    delete_coroutine_context(coroutine_context);
    return 0;
}

int merge(int **data, int *size, int *index, int count) {
    int min_index = -1;
    int current_min = INT_MAX;
    for (int i = 0; i < count; ++i) {
        if ((size[i] > index[i]) && (data[i][index[i]] < current_min)) {
            current_min = data[i][index[i]];
            min_index = i;
        }
    }
    return min_index;
}

int main(int argc, char **argv) {
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    coro_sched_init();

    int number_of_files = argc - 3;
    int number_of_coroutines = atoi(argv[2]);

    if (!number_of_coroutines || !number_of_files) {
        fprintf(stderr, "Invalid command line arguments. Usage %s T N <file1> <fileX>\n", argv[0]);
        fprintf(stderr, "T - target latency, N - coroutines count\n");
        return 1;
    }

    int *pointer_to_arrays[number_of_files];
    int array_sizes[number_of_files];
    int file_index = 0;

    for (int i = 0; i < number_of_coroutines; ++i) {
        char name[16];
        sprintf(name, "coro_%d", i);
        coro_new(coroutine_function,
                 create_coroutine_context(name, argv + 3, number_of_files, &file_index, pointer_to_arrays, array_sizes,
                                          atoi(argv[1]) * 1000 / number_of_files));
    }

    struct coro *current_coroutine;
    while ((current_coroutine = coro_sched_wait()) != NULL) {
        coro_delete(current_coroutine);
    }

    int current_index[number_of_files];
    for (int i = 0; i < number_of_files; ++i) {
        current_index[i] = 0;
    }

    FILE *output_file = fopen("out.txt", "w");

    int min_index = 0;
    while (min_index != -1) {
        min_index = merge(pointer_to_arrays, array_sizes, current_index, number_of_files);
        if (min_index != -1) {
            fprintf(output_file, "%d ", pointer_to_arrays[min_index][current_index[min_index]]);
            current_index[min_index] += 1;
        }
    }
    fclose(output_file);

    for (int i = 0; i < number_of_files; ++i) {
        free(pointer_to_arrays[i]);
    }

    struct timespec finish_time;
    clock_gettime(CLOCK_MONOTONIC, &finish_time);

    printf("total time: %.6f seconds\n",
           (finish_time.tv_sec - start_time.tv_sec) + (finish_time.tv_nsec - start_time.tv_nsec) / 1e9);

    return 0;
}
