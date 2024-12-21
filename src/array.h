#ifndef PWMIXER_ARRAY_H
#define PWMIXER_ARRAY_H

#include <stddef.h>
#include <stdint.h>

struct array {
    void *data;
    size_t item_size;
    int length;
    int capacity;
};

struct array *array_new(size_t item_size);

int array_append(struct array *array, void *item);

void *array_get(struct array *array, int index);

void *array_remove(struct array *array, int index);

int array_index(struct array *array, void *item);

int array_free(struct array *array);

#endif
