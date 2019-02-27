/* libSoX Memory allocation functions
 *
 * Copyright (c) 2005-2006 Reuben Thomas.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef LSX_MALLOC_H
#define LSX_MALLOC_H

#include <stddef.h>
#include <string.h>

static inline void *_lsx_realloc(void *ptr, size_t newsize, const char *file, const char *func, const int line)
{
  if (ptr && newsize == 0) {
    free(ptr);
    return NULL;
  }

  if ((ptr = realloc(ptr, newsize)) == NULL) {
    lsx_fail("out of memory, newsize was %lu from %s %s:%d", newsize, file, func, line);
    exit(2);
  }

  return ptr;
}

#define lsx_realloc(PTR, NEWSIZE) _lsx_realloc((PTR), (NEWSIZE), __FILE__, __func__, __LINE__)
#define lsx_malloc(size) _lsx_realloc(NULL, (size), __FILE__, __func__, __LINE__)
#define lsx_calloc(n,s) (((n)*(s))? memset(lsx_malloc((n)*(s)),0,(n)*(s)) : NULL)
#define lsx_Calloc(v,n)  v = lsx_calloc(n,sizeof(*(v)))
#define lsx_strdup(p) ((p)? strcpy((char *)lsx_malloc(strlen(p) + 1), p) : NULL)
#define lsx_memdup(p,s) ((p)? memcpy(lsx_malloc(s), p, s) : NULL)
#define lsx_valloc(v,n)  v = lsx_malloc((n)*sizeof(*(v)))
#define lsx_revalloc(v,n)  v = _lsx_realloc(v, (n)*sizeof(*(v)), __FILE__, __func__, __LINE__)

#endif
