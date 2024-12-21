#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "array.h"

#define INITIAL_CAP 2

struct array *array_new(size_t item_size)
{
    struct array *arr = (struct array*)malloc(sizeof(struct array));
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
            return 0;
    }

    memcpy((char*)arr->data + arr->length * arr->item_size,
        item, arr->item_size);
    arr->length++;

    return arr->length;
}

void *array_get(struct array *arr, int index)
{
    if (index >= arr->length)
        return NULL;
    return (char*)arr->data + (index * arr->item_size);
}

void *array_remove(struct array *arr, int index)
{
    if (index >= arr->length)
        return NULL;

    char *target = (char*)arr->data + (index * arr->item_size);
    if (index < arr->length - 1)
        memmove(target, target + arr->item_size,
            (arr->length - index - 1) * arr->item_size);
    arr->length--;
}

int array_index(struct array *arr, void *item)
{
    for (uint32_t i = 0; i < arr->length; i++) {
        if (memcmp((char*)arr->data + i * arr->item_size, item, arr->item_size) == 0)
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
