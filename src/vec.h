#ifndef HYPRWINDOWS_VEC_H
#define HYPRWINDOWS_VEC_H

#include <stdlib.h>
#include <string.h>

/*
 * Generic type-safe dynamic array macros.
 *
 * Usage:
 *   VEC_DEF(my_vec, int);           // defines struct my_vec { int *data; size_t len, cap; };
 *   struct my_vec v = {0};
 *   VEC_PUSH(&v, 42);               // appends 42
 *   v.data[0]                        // access element
 *   VEC_FREE(&v);                    // frees memory
 *
 * For pointer types (e.g. char*) that need individual freeing, call your
 * own loop before VEC_FREE.
 */

#define VEC_DEF(name, type) \
    struct name { type *data; size_t len, cap; }

/* Ensure capacity for at least one more element. Returns 0 on success, -1 on failure. */
#define VEC_GROW(v) \
    (((v)->len < (v)->cap) ? 0 : \
     _vec_grow_((void **)&(v)->data, &(v)->cap, sizeof(*(v)->data)))

/* Push a value. Evaluates to 0 on success, -1 on alloc failure. */
#define VEC_PUSH(v, val) \
    (VEC_GROW(v) == 0 ? ((v)->data[(v)->len++] = (val), 0) : -1)

/* Remove element at index i, shifting remaining down. */
#define VEC_REMOVE(v, i) do { \
    if ((i) < (v)->len) { \
        memmove(&(v)->data[(i)], &(v)->data[(i) + 1], \
                ((v)->len - (i) - 1) * sizeof(*(v)->data)); \
        (v)->len--; \
    } \
} while (0)

/* Free the array. Does NOT free pointed-to data for pointer types. */
#define VEC_FREE(v) do { free((v)->data); (v)->data = NULL; (v)->len = 0; (v)->cap = 0; } while (0)

/* Reset length without freeing. */
#define VEC_CLEAR(v) ((v)->len = 0)

static inline int _vec_grow_(void **data, size_t *cap, size_t elem_sz) {
    size_t new_cap = *cap == 0 ? 8 : *cap * 2;
    void *p = realloc(*data, new_cap * elem_sz);
    if (!p) return -1;
    *data = p;
    *cap = new_cap;
    return 0;
}

#endif
