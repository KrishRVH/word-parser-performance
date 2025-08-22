// wordcount.c - Word frequency counter
// Build: gcc -O3 -march=native wordcount.c -o wordcount_c
// Usage: ./wordcount_c [filename]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#define HASH_SIZE 16384
#define MAX_WORD_LENGTH 100
#define TOP_WORDS 100

typedef struct WordNode {
    char* word;
    int count;
    struct WordNode* next;
} WordNode;

typedef struct {
    char* word;
    int count;
} WordCount;

static long total_words = 0;
static int unique_words = 0;

// FNV-1a hash function
static inline unsigned int hash_function(const char* str) {
    unsigned int hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 16777619;
    }
    return hash & (HASH_SIZE - 1);
}

static WordNode* create_node(const char* word) {
    WordNode* node = (WordNode*)malloc(sizeof(WordNode));
    node->word = strdup(word);
    node->count = 1;
    node->next = NULL;
    unique_words++;
    return node;
}

static void insert_word(WordNode** table, const char* word) {
    unsigned int index = hash_function(word);
    WordNode* node = table[index];
    
    WordNode* prev = NULL;
    while (node) {
        if (strcmp(node->word, word) == 0) {
            node->count++;
            total_words++;
            return;
        }
        prev = node;
        node = node->next;
    }
    
    WordNode* new_node = create_node(word);
    if (prev) {
        prev->next = new_node;
    } else {
        table[index] = new_node;
    }
    total_words++;
}

static inline int extract_word(char* dest, const char* src, int* pos) {
    int i = 0;
    int p = *pos;
    
    while (src[p] && !isalpha(src[p])) p++;
    
    while (src[p] && isalpha(src[p]) && i < MAX_WORD_LENGTH - 1) {
        dest[i++] = tolower(src[p++]);
    }
    
    dest[i] = '\0';
    *pos = p;
    return i;
}

// Comparison function for qsort
static int compare_words(const void* a, const void* b) {
    const WordCount* wa = (const WordCount*)a;
    const WordCount* wb = (const WordCount*)b;
    if (wb->count != wa->count) {
        return wb->count - wa->count;
    }
    return strcmp(wa->word, wb->word);
}

static void free_table(WordNode** table) {
    for (int i = 0; i < HASH_SIZE; i++) {
        WordNode* node = table[i];
        while (node) {
            WordNode* next = node->next;
            free(node->word);
            free(node);
            node = next;
        }
    }
}

static double get_file_size_mb(const char* filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size / (1024.0 * 1024.0);
    }
    return 0;
}

static void format_number(char* buffer, long num) {
    char temp[32];
    sprintf(temp, "%ld", num);
    int len = strlen(temp);
    int comma_count = (len - 1) / 3;
    int out_len = len + comma_count;
    buffer[out_len] = '\0';
    
    int temp_pos = len - 1;
    int out_pos = out_len - 1;
    int digit_count = 0;
    
    while (temp_pos >= 0) {
        if (digit_count == 3) {
            buffer[out_pos--] = ',';
            digit_count = 0;
        }
        buffer[out_pos--] = temp[temp_pos--];
        digit_count++;
    }
}

int main(int argc, char* argv[]) {
    const char* filename = (argc > 1) ? argv[1] : "book.txt";
    
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: File '%s' not found\n", filename);
        printf("Usage: %s [filename]\n\n", argv[0]);
        printf("To create a test file:\n");
        printf("curl https://www.gutenberg.org/files/2701/2701-0.txt -o book.txt\n");
        return 1;
    }
    
    printf("Processing file: %s\n", filename);
    
    clock_t start_time = clock();
    
    WordNode** table = (WordNode**)calloc(HASH_SIZE, sizeof(WordNode*));
    if (!table) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(file);
        return 1;
    }
    
    char buffer[8192];
    char word[MAX_WORD_LENGTH];
    
    while (fgets(buffer, sizeof(buffer), file)) {
        int pos = 0;
        int len = strlen(buffer);
        
        while (pos < len) {
            if (extract_word(word, buffer, &pos) > 0) {
                insert_word(table, word);
            }
        }
    }
    
    fclose(file);
    
    WordCount* words = (WordCount*)malloc(unique_words * sizeof(WordCount));
    if (!words) {
        fprintf(stderr, "Memory allocation failed\n");
        free_table(table);
        free(table);
        return 1;
    }
    
    int word_index = 0;
    for (int i = 0; i < HASH_SIZE; i++) {
        WordNode* node = table[i];
        while (node) {
            words[word_index].word = node->word;
            words[word_index].count = node->count;
            word_index++;
            node = node->next;
        }
    }
    
    qsort(words, unique_words, sizeof(WordCount), compare_words);
    
    clock_t end_time = clock();
    double execution_time = ((double)(end_time - start_time) / CLOCKS_PER_SEC) * 1000.0;
    
    double file_size = get_file_size_mb(filename);
    
    char total_str[32], unique_str[32];
    format_number(total_str, total_words);
    format_number(unique_str, unique_words);
    
    printf("\n=== Top 10 Most Frequent Words ===\n");
    for (int i = 0; i < 10 && i < unique_words; i++) {
        char count_str[32];
        format_number(count_str, words[i].count);
        printf("%2d. %-15s %9s\n", i + 1, words[i].word, count_str);
    }
    
    printf("\n=== Statistics ===\n");
    printf("File size:       %.2f MB\n", file_size);
    printf("Total words:     %s\n", total_str);
    printf("Unique words:    %s\n", unique_str);
    printf("Execution time:  %.2f ms\n", execution_time);
    printf("Hash table size: %d buckets\n", HASH_SIZE);
    printf("Compiler:        ");
    #ifdef __clang__
        printf("Clang %s\n", __clang_version__);
    #elif defined(__GNUC__)
        printf("GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    #else
        printf("Unknown\n");
    #endif
    
    char output_filename[256];
    snprintf(output_filename, sizeof(output_filename), "%.*s_c_results.txt",
             (int)(strrchr(filename, '.') - filename), filename);
    
    FILE* output = fopen(output_filename, "w");
    if (output) {
        fprintf(output, "Word Frequency Analysis - C Implementation\n");
        fprintf(output, "Input file: %s\n", filename);
        
        time_t now = time(NULL);
        fprintf(output, "Generated: %s", ctime(&now));
        fprintf(output, "Execution time: %.2f ms\n\n", execution_time);
        fprintf(output, "Total words: %ld\n", total_words);
        fprintf(output, "Unique words: %d\n\n", unique_words);
        fprintf(output, "Top 100 Most Frequent Words:\n");
        fprintf(output, "Rank  Word            Count     Percentage\n");
        fprintf(output, "----  --------------- --------- ----------\n");
        
        for (int i = 0; i < TOP_WORDS && i < unique_words; i++) {
            double percentage = (words[i].count * 100.0) / total_words;
            fprintf(output, "%4d  %-15s %9d %10.2f%%\n",
                    i + 1, words[i].word, words[i].count, percentage);
        }
        
        fclose(output);
        printf("\nResults written to: %s\n", output_filename);
    }
    
    free_table(table);
    free(table);
    free(words);
    
    return 0;
}