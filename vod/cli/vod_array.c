#include <stdlib.h>
#include "common.h"

int 
vod_array_init_impl(vod_array_t *array, unsigned n, size_t size)
{
    array->nelts = 0;
    array->size = size;
    array->nalloc = n;

    array->elts = malloc(n * size);
    if (array->elts == NULL) 
	{
        return VOD_ERROR;
    }

    return VOD_OK;
}

void*
vod_array_push(vod_array_t *a)
{
    void        *elt, *new_elts;

	if (a->nelts >= a->nalloc)
	{
		new_elts = realloc(a->elts, a->size * a->nalloc * 2);
		if (new_elts == NULL)
		{
			return NULL;
		}
		a->elts = new_elts;
		a->nalloc *= 2;
	}
	
    elt = (u_char *) a->elts + a->size * a->nelts;
    a->nelts++;

    return elt;
}

void 
vod_array_destroy(vod_array_t *a)
{
	free(a->elts);
	a->elts = NULL;
}
