#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} str_list;

void str_list_init(str_list *list);
void str_list_push(str_list *list, const char *value);
void str_list_free(str_list *list);

char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warn_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

char *path_join(const char *a, const char *b);
char *read_text_file(const char *path, size_t *len_out);
char *read_first_line(const char *path);
void write_text_file(const char *path, const char *text);
bool file_exists(const char *path);
bool dir_exists(const char *path);
void ensure_dir(const char *path);
void ensure_dir_tree(const char *path);
char *vm_home_dir(void);
char *vm_instances_dir(void);
char *vm_builders_dir(void);
char *vm_cache_dir(void);
char *current_executable_path(void);
char *absolute_existing_dir(const char *path);
int run_process(char *const argv[], const char *cwd, char *const envp_extra[], bool quiet);
