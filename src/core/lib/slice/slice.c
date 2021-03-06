/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include <string.h>

grpc_slice gpr_empty_slice(void) {
  grpc_slice out;
  out.refcount = 0;
  out.data.inlined.length = 0;
  return out;
}

grpc_slice grpc_slice_ref(grpc_slice slice) {
  if (slice.refcount) {
    slice.refcount->ref(slice.refcount);
  }
  return slice;
}

void grpc_slice_unref(grpc_slice slice) {
  if (slice.refcount) {
    slice.refcount->unref(slice.refcount);
  }
}

/* grpc_slice_from_static_string support structure - a refcount that does
   nothing */
static void noop_ref_or_unref(void *unused) {}

static grpc_slice_refcount noop_refcount = {noop_ref_or_unref,
                                            noop_ref_or_unref};

grpc_slice grpc_slice_from_static_string(const char *s) {
  grpc_slice slice;
  slice.refcount = &noop_refcount;
  slice.data.refcounted.bytes = (uint8_t *)s;
  slice.data.refcounted.length = strlen(s);
  return slice;
}

/* grpc_slice_new support structures - we create a refcount object extended
   with the user provided data pointer & destroy function */
typedef struct new_slice_refcount {
  grpc_slice_refcount rc;
  gpr_refcount refs;
  void (*user_destroy)(void *);
  void *user_data;
} new_slice_refcount;

static void new_slice_ref(void *p) {
  new_slice_refcount *r = p;
  gpr_ref(&r->refs);
}

static void new_slice_unref(void *p) {
  new_slice_refcount *r = p;
  if (gpr_unref(&r->refs)) {
    r->user_destroy(r->user_data);
    gpr_free(r);
  }
}

grpc_slice grpc_slice_new_with_user_data(void *p, size_t len,
                                         void (*destroy)(void *),
                                         void *user_data) {
  grpc_slice slice;
  new_slice_refcount *rc = gpr_malloc(sizeof(new_slice_refcount));
  gpr_ref_init(&rc->refs, 1);
  rc->rc.ref = new_slice_ref;
  rc->rc.unref = new_slice_unref;
  rc->user_destroy = destroy;
  rc->user_data = user_data;

  slice.refcount = &rc->rc;
  slice.data.refcounted.bytes = p;
  slice.data.refcounted.length = len;
  return slice;
}

grpc_slice grpc_slice_new(void *p, size_t len, void (*destroy)(void *)) {
  /* Pass "p" to *destroy when the slice is no longer needed. */
  return grpc_slice_new_with_user_data(p, len, destroy, p);
}

/* grpc_slice_new_with_len support structures - we create a refcount object
   extended with the user provided data pointer & destroy function */
typedef struct new_with_len_slice_refcount {
  grpc_slice_refcount rc;
  gpr_refcount refs;
  void *user_data;
  size_t user_length;
  void (*user_destroy)(void *, size_t);
} new_with_len_slice_refcount;

static void new_with_len_ref(void *p) {
  new_with_len_slice_refcount *r = p;
  gpr_ref(&r->refs);
}

static void new_with_len_unref(void *p) {
  new_with_len_slice_refcount *r = p;
  if (gpr_unref(&r->refs)) {
    r->user_destroy(r->user_data, r->user_length);
    gpr_free(r);
  }
}

grpc_slice grpc_slice_new_with_len(void *p, size_t len,
                                   void (*destroy)(void *, size_t)) {
  grpc_slice slice;
  new_with_len_slice_refcount *rc =
      gpr_malloc(sizeof(new_with_len_slice_refcount));
  gpr_ref_init(&rc->refs, 1);
  rc->rc.ref = new_with_len_ref;
  rc->rc.unref = new_with_len_unref;
  rc->user_destroy = destroy;
  rc->user_data = p;
  rc->user_length = len;

  slice.refcount = &rc->rc;
  slice.data.refcounted.bytes = p;
  slice.data.refcounted.length = len;
  return slice;
}

grpc_slice grpc_slice_from_copied_buffer(const char *source, size_t length) {
  grpc_slice slice = grpc_slice_malloc(length);
  memcpy(GRPC_SLICE_START_PTR(slice), source, length);
  return slice;
}

grpc_slice grpc_slice_from_copied_string(const char *source) {
  return grpc_slice_from_copied_buffer(source, strlen(source));
}

typedef struct {
  grpc_slice_refcount base;
  gpr_refcount refs;
} malloc_refcount;

static void malloc_ref(void *p) {
  malloc_refcount *r = p;
  gpr_ref(&r->refs);
}

static void malloc_unref(void *p) {
  malloc_refcount *r = p;
  if (gpr_unref(&r->refs)) {
    gpr_free(r);
  }
}

grpc_slice grpc_slice_malloc(size_t length) {
  grpc_slice slice;

  if (length > sizeof(slice.data.inlined.bytes)) {
    /* Memory layout used by the slice created here:

       +-----------+----------------------------------------------------------+
       | refcount  | bytes                                                    |
       +-----------+----------------------------------------------------------+

       refcount is a malloc_refcount
       bytes is an array of bytes of the requested length
       Both parts are placed in the same allocation returned from gpr_malloc */
    malloc_refcount *rc = gpr_malloc(sizeof(malloc_refcount) + length);

    /* Initial refcount on rc is 1 - and it's up to the caller to release
       this reference. */
    gpr_ref_init(&rc->refs, 1);

    rc->base.ref = malloc_ref;
    rc->base.unref = malloc_unref;

    /* Build up the slice to be returned. */
    /* The slices refcount points back to the allocated block. */
    slice.refcount = &rc->base;
    /* The data bytes are placed immediately after the refcount struct */
    slice.data.refcounted.bytes = (uint8_t *)(rc + 1);
    /* And the length of the block is set to the requested length */
    slice.data.refcounted.length = length;
  } else {
    /* small slice: just inline the data */
    slice.refcount = NULL;
    slice.data.inlined.length = (uint8_t)length;
  }
  return slice;
}

grpc_slice grpc_slice_sub_no_ref(grpc_slice source, size_t begin, size_t end) {
  grpc_slice subset;

  GPR_ASSERT(end >= begin);

  if (source.refcount) {
    /* Enforce preconditions */
    GPR_ASSERT(source.data.refcounted.length >= end);

    /* Build the result */
    subset.refcount = source.refcount;
    /* Point into the source array */
    subset.data.refcounted.bytes = source.data.refcounted.bytes + begin;
    subset.data.refcounted.length = end - begin;
  } else {
    /* Enforce preconditions */
    GPR_ASSERT(source.data.inlined.length >= end);
    subset.refcount = NULL;
    subset.data.inlined.length = (uint8_t)(end - begin);
    memcpy(subset.data.inlined.bytes, source.data.inlined.bytes + begin,
           end - begin);
  }
  return subset;
}

grpc_slice grpc_slice_sub(grpc_slice source, size_t begin, size_t end) {
  grpc_slice subset;

  if (end - begin <= sizeof(subset.data.inlined.bytes)) {
    subset.refcount = NULL;
    subset.data.inlined.length = (uint8_t)(end - begin);
    memcpy(subset.data.inlined.bytes, GRPC_SLICE_START_PTR(source) + begin,
           end - begin);
  } else {
    subset = grpc_slice_sub_no_ref(source, begin, end);
    /* Bump the refcount */
    subset.refcount->ref(subset.refcount);
  }
  return subset;
}

grpc_slice grpc_slice_split_tail(grpc_slice *source, size_t split) {
  grpc_slice tail;

  if (source->refcount == NULL) {
    /* inlined data, copy it out */
    GPR_ASSERT(source->data.inlined.length >= split);
    tail.refcount = NULL;
    tail.data.inlined.length = (uint8_t)(source->data.inlined.length - split);
    memcpy(tail.data.inlined.bytes, source->data.inlined.bytes + split,
           tail.data.inlined.length);
    source->data.inlined.length = (uint8_t)split;
  } else {
    size_t tail_length = source->data.refcounted.length - split;
    GPR_ASSERT(source->data.refcounted.length >= split);
    if (tail_length < sizeof(tail.data.inlined.bytes)) {
      /* Copy out the bytes - it'll be cheaper than refcounting */
      tail.refcount = NULL;
      tail.data.inlined.length = (uint8_t)tail_length;
      memcpy(tail.data.inlined.bytes, source->data.refcounted.bytes + split,
             tail_length);
    } else {
      /* Build the result */
      tail.refcount = source->refcount;
      /* Bump the refcount */
      tail.refcount->ref(tail.refcount);
      /* Point into the source array */
      tail.data.refcounted.bytes = source->data.refcounted.bytes + split;
      tail.data.refcounted.length = tail_length;
    }
    source->data.refcounted.length = split;
  }

  return tail;
}

grpc_slice grpc_slice_split_head(grpc_slice *source, size_t split) {
  grpc_slice head;

  if (source->refcount == NULL) {
    GPR_ASSERT(source->data.inlined.length >= split);

    head.refcount = NULL;
    head.data.inlined.length = (uint8_t)split;
    memcpy(head.data.inlined.bytes, source->data.inlined.bytes, split);
    source->data.inlined.length =
        (uint8_t)(source->data.inlined.length - split);
    memmove(source->data.inlined.bytes, source->data.inlined.bytes + split,
            source->data.inlined.length);
  } else if (split < sizeof(head.data.inlined.bytes)) {
    GPR_ASSERT(source->data.refcounted.length >= split);

    head.refcount = NULL;
    head.data.inlined.length = (uint8_t)split;
    memcpy(head.data.inlined.bytes, source->data.refcounted.bytes, split);
    source->data.refcounted.bytes += split;
    source->data.refcounted.length -= split;
  } else {
    GPR_ASSERT(source->data.refcounted.length >= split);

    /* Build the result */
    head.refcount = source->refcount;
    /* Bump the refcount */
    head.refcount->ref(head.refcount);
    /* Point into the source array */
    head.data.refcounted.bytes = source->data.refcounted.bytes;
    head.data.refcounted.length = split;
    source->data.refcounted.bytes += split;
    source->data.refcounted.length -= split;
  }

  return head;
}

int grpc_slice_cmp(grpc_slice a, grpc_slice b) {
  int d = (int)(GRPC_SLICE_LENGTH(a) - GRPC_SLICE_LENGTH(b));
  if (d != 0) return d;
  return memcmp(GRPC_SLICE_START_PTR(a), GRPC_SLICE_START_PTR(b),
                GRPC_SLICE_LENGTH(a));
}

int grpc_slice_str_cmp(grpc_slice a, const char *b) {
  size_t b_length = strlen(b);
  int d = (int)(GRPC_SLICE_LENGTH(a) - b_length);
  if (d != 0) return d;
  return memcmp(GRPC_SLICE_START_PTR(a), b, b_length);
}
