/*
 * Copyright (C) 2025 Davide Usberti <usbertibox@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * =================== Informations About Program ========================
 *
 * This educational repository demonstrates how multithreading can improve 
 * efficiency when searching for words inside very large text files. 
 *
 * The program implements two approaches:
 *   1. Single-threaded search:
 *        - A straightforward algorithm that scans the entire file 
 *          sequentially and counts the number of occurrences of the 
 *          given word.
 *
 *   2. Multi-threaded search (using POSIX Threads):
 *        - The file is divided into chunks.
 *        - Each thread searches in its own chunk independently.
 *        - Partial results are collected and summed up in the main thread.
 *
 * By comparing the execution time of the two methods, the program clearly 
 * shows how threads can reduce the time required to process large inputs, 
 * especially on multi-core systems.
 *
 * This example is kept intentionally simple and educational, avoiding 
 * unnecessary complexity, while still showing synchronization and 
 * workload distribution.
 *
 * Compilation:
 *   gcc search.c -o tsearch -pthread
 *
 * Usage:
 *   ./tsearch <filename> <word> <num_threads>
 *
 * Example:
 *   ./tsearch biglog.txt ERROR 4
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#if defined(__unix__)
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#endif

#define LOG(str, ...) printf("LOG: " str "\n", ##__VA_ARGS__);
#define ERR(str, ...) fprintf(stderr, "ERR: " str "\n", ##__VA_ARGS__);

/* This macro converts a string to long, 
 * if the conversion result in error
 * retruns 0*/
#define STR_TO_LONG(val) ({ \
    char *e; \
    errno = 0; \
    long n = strtol(val, &e, 10); \
    (errno == ERANGE || *e != '\0') ? 0 : n; \
})

#define MAX_WORD_LENGTH 128
#define BUFFER_SIZE 4096
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Structure given at the end of the search as result */
struct search_result_t {
        time_t    elapsed_time;           /* The runtime of the search */
        char      word[MAX_WORD_LENGTH];  /* Word to search */
        uint64_t  occurrences;            /* Occurrence founds */
};

/* Simple and parser-inspired struct for a chunk, which scan a portion
 * of the entire text and finds occurrences of the word. */
typedef struct {
        int thread_id;
        char *filename;              /* We cannot use FILE * because of concurrency */
        long start_pos;              /* The start position of the chunk to read */
        long end_pos;                /* Then end position of the chunk to read */
        char word[MAX_WORD_LENGTH];
        uint64_t occurrences;
        int word_len;
} thread_data_t;

long elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000 +
           (end.tv_nsec - start.tv_nsec) / 1000000;
}

/* Function to count word occurrences in a string */
uint64_t count_word_occurrences(const char *text, size_t text_len, 
                               const char *word, int word_len) {
        uint64_t count = 0;
        const char *p = text;
        const char *end = text + text_len - word_len + 1;
    
        while (p < end) {
                if (strncmp(p, word, word_len) == 0) {
                        /* Check word boundaries */
                        if ((p == text || !isalnum((unsigned char)p[-1])) &&
                                (p + word_len >= text + text_len || !isalnum((unsigned char)p[word_len]))) {
                                count++;
                        }
               }
               p++;
        }
        return count;
}


/* Thread function to search in a chunk
 *
 * - Positions file pointer at chunk start.
 * - Adds overlap (word_len - 1) to avoid missing matches across boundaries.
 * - Reads chunk in BUFFER_SIZE blocks.
 * - Uses count_word_occurrences() to count matches in each block.
 * - Stores total matches in data->occurrences.
 *
 * Issues fixed:
 *      If a word is sliced between two chunks we can lose it,
 *      so this function make a overlap to word_len - 1 to catch
 *      all the word occurrences.
 */
void *search_chunk(void *arg) {
        thread_data_t* data = (thread_data_t*)arg;
        char buffer[BUFFER_SIZE];
        size_t bytes_read;
        long total_read = 0;
        long chunk_size = data->end_pos - data->start_pos;
    
        FILE *file = fopen(data->filename, "r");
        if (!file) {
                ERR("Thread %d: Failed to open file", data->thread_id);
                return NULL;
        }
        
        long actual_start = data->start_pos;
        long actual_chunk_size = chunk_size;
    
        long overlap = data->word_len - 1;
        if (data->thread_id > 0 && actual_start >= overlap) {
                actual_start -= overlap;
                actual_chunk_size += overlap;
        }

        if (fseek(file, actual_start, SEEK_SET) != 0) {
                ERR("Thread %d: Seek failed", data->thread_id);
                fclose(file);
                return NULL;
        }
    
        while (total_read < actual_chunk_size && 
               (bytes_read = fread(buffer, 1, 
                                  MIN(BUFFER_SIZE, actual_chunk_size - total_read), 
                                  file)) > 0) {
                
                data->occurrences += count_word_occurrences(buffer, bytes_read, 
                                                           data->word, data->word_len);
                total_read += bytes_read;
        }
    
        fclose(file);
        return NULL;
}

/* Search for a word occourrences by giving a file pointer */
struct search_result_t *tsearch(char *filename, char word[MAX_WORD_LENGTH], uint8_t threads) {
        struct search_result_t *res = malloc(sizeof(struct search_result_t));
        if (res == NULL) return NULL;
        memset(res, 0, sizeof(*res));

        strncpy(res->word, word, MAX_WORD_LENGTH - 1);
        res->word[MAX_WORD_LENGTH - 1] = '\0';
        
        /* Start time counter */
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        /* File opening for each threads to avoid race conditions */
        FILE *fp = fopen(filename, "r");
        if (!fp) {
                ERR("Failed to open file '%s'", filename);
                free(res);
                return NULL;
        }

        fseek(fp, 0, SEEK_END); /* Move cursor to the EOF */
        long file_size = ftell(fp); /* Get the position of the cursor (bytes) */
        rewind(fp); /* Move cursor on the top of file */
        
        /* If file is small or single-threaded is requested, use simple approch */
        if (file_size < BUFFER_SIZE || threads <= 1) {
                LOG("Using single threaded search");

                char buffer[BUFFER_SIZE];
                size_t bytes_read;
                int word_len = strlen(word);
                
                /* Search and count word occurences */
                while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
                        res->occurrences += count_word_occurrences(buffer, bytes_read, word, word_len);
                }
                
                /* stop timer */
                clock_gettime(CLOCK_MONOTONIC, &end);
                res->elapsed_time = elapsed_ms(start, end);
                fclose(fp);
                return res;
        }

        fclose(fp);
        
        /* Thread allocation and error handling */
        pthread_t *thread_list = malloc(threads * sizeof(pthread_t));
        thread_data_t *thread_data = malloc(threads * sizeof(thread_data_t));

        if (!thread_data || !thread_list) {
                ERR("Memory allocation failed for threads");
                goto cleanup;
        }
        
        /* Chunk size evaluation */
        long chunk_size = file_size / threads; 
        int word_len = strlen(word); 

        for (int i = 0; i < threads; i++) {

                /* thread_data initialization */
                thread_data[i].thread_id = i;
                thread_data[i].filename = strdup(filename);
                if (!thread_data[i].filename) {
                        ERR("Failed to duplicate filename for thread %d", i);
                        for (int j = 0; j < i; j++) {
                                pthread_join(thread_list[j], NULL);
                                free(thread_data[j].filename); 
                        }
                        goto cleanup;
                }
                thread_data[i].start_pos = i * chunk_size;
                thread_data[i].end_pos = (i == threads - 1) ? file_size : (i + 1) * chunk_size;
                thread_data[i].occurrences = 0;
                thread_data[i].word_len = word_len;
                strncpy(thread_data[i].word, word, MAX_WORD_LENGTH);

                /* threads creation */
                if (pthread_create(&thread_list[i], NULL, search_chunk, &thread_data[i]) != 0)  {
                        ERR("Failed to create thread %d", i);
                        free(thread_data[i].filename); 
                        /* wait for other threads before join */
                        for (int j = 0; j < i; j++) {
                                pthread_join(thread_list[j], NULL);
                                free(thread_data[j].filename); 
                        }
                        goto cleanup;
                }
        }

        for (int i = 0; i < threads; i++) {
                /* wait other threads */
                pthread_join(thread_list[i], NULL);
                /* get occurrences */
                res->occurrences += thread_data[i].occurrences;
                free(thread_data[i].filename);
        }
        
        /* stop timer */
        clock_gettime(CLOCK_MONOTONIC, &end);
        res->elapsed_time = elapsed_ms(start, end);

       
        free(thread_list);
        free(thread_data);
        return res;

cleanup:
        if (thread_list && thread_data) {
                for (int i = 0; i < threads; i++) {
                        pthread_join(thread_list[i], NULL);
                        free(thread_data[i].filename);
                }
                free(thread_list);
                free(thread_data);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        res->elapsed_time = elapsed_ms(start, end);
        return res;
}


int main(int argc, char **argv) {
#if defined(__unix__) 
        /* Args checking */
        if (argc != 4) {
                ERR("You need to provide `./tsearch <filename> <word> <num_threads>`");
                goto cleanup;
        }
        
        /* Get the word to search */
        char word[MAX_WORD_LENGTH];
        strncpy(word, argv[2], sizeof(word) - 1);
        word[sizeof(word) - 1] = '\0';

        uint8_t threads = (uint8_t) STR_TO_LONG(argv[3]);

        LOG("Searching for word '%s' in '%s' using %d threads", 
                        word, argv[1], threads);
        
        /* Initialize the search and get the result */
        struct search_result_t *res = tsearch(argv[1], word, threads);

        if (res) {
                LOG("Found %lu occurrences in %ld ms", 
                res->occurrences, res->elapsed_time);
        } else {
                ERR("Failed to return a result");
                goto cleanup;
        }

        free(res);

        return 0;

cleanup:
        return 1;
#else 
        ERR("This program uses `pthread` system calls, so you need a unix system.");
        return 1;
#endif
}
