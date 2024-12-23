#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "array.h"

#define INITIAL_CAP 2

struct array *array_new(size_t item_size)
{
    struct array *arr = (struct array*)malloc(sizeof(struct array*));
    if (!arr) {
        return NULL;
    }

    arr->data = malloc(item_size * INITIAL_CAP);
    if (!arr->data) {
        free(arr);
        return NULL;
    }

    arr->item_size = item_size;
    arr->length = 0;
    arr->capacity = INITIAL_CAP;
    return arr;
}

int array_append(struct array *arr, void *item)
{
    if (arr->length == arr->capacity) {
        arr->capacity *= 2;
        arr->data = realloc(arr->data, arr->capacity * arr->item_size);
        if (!arr->data)
            return -1;
    }

    arr->data[arr->length] = item;
    arr->length++;

    return arr->length;
}

void *array_get(struct array *arr, int index)
{
    if (index >= arr->length)
        return NULL;
    return arr->data[index];
}

int array_remove(struct array *arr, int index)
{
    if (index >= arr->length || index < 0)
        return -1;

    char *target = (char*)arr->data + (index * arr->item_size);
    for (int i = index + 1; i < arr->length; i++)
        arr->data[i - 1] = arr->data[i];
    arr->length--;

    return arr->length;
}

int array_find_index(struct array *arr, void *item)
{
    for (uint32_t i = 0; i < arr->length; i++) {
        if (arr->data[i] == item)
            return i;
    }
    return -1;
}

int array_free(struct array *arr)
{
    if (!arr)
        return 0;
    if (arr->data) {
        free(arr->data);
        arr->data = NULL;
    }
    free(arr);
    return 0;
}
