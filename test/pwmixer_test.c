#include "array.h"
#include <stdio.h>
#include <assert.h>

struct array_item {
    int n;
};

static void test_array()
{
    uint32_t i, max = 10;
    struct array_item aitem[max], *titem;
    struct array *arr = array_new(sizeof(struct array_item*));

    assert(sizeof(arr) == sizeof(struct array*));
    assert(arr->item_size == sizeof(struct array_item*));
    assert(arr->length == 0);

    for (i = 0; i < max; i++) {
        aitem[i].n = i;
        assert(array_append(arr, &aitem[i]) == i + 1);
    }

    assert(arr->length == max);

    for (i = 0; i < arr->length; i++) {
        titem = array_get(arr, i);
        assert(titem->n == aitem[i].n);
    }

    array_remove(arr, 5);
    titem = array_get(arr, 5);
    assert(titem->n == aitem[6].n);
    assert(array_index(arr, &aitem[9]) == 8);

    assert(array_free(arr) == 0);
}

int main(int argc, char *argv[])
{
    test_array();
}
