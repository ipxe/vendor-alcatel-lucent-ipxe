/* example.c -- usage example of the zlib compression library
 * Copyright (C) 1995-2006 Jean-loup Gailly.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#include <../zlib/zlib.h>
#include <stdio.h>

#if 0
#ifdef STDC
#  include <string.h>
#  include <stdlib.h>
#endif
#endif

#if defined(VMS) || defined(RISCOS)
#  define TESTFILE "foo-gz"
#else
#  define TESTFILE "foo.gz"
#endif

#define CHECK_ERR(err, msg) { \
    if (err != Z_OK) { \
        printf("%s error: %d\n", msg, err); \
        return(1); \
    } \
}

/* ===========================================================================
 * Test inflate_v() with small buffers
 */
int inflate_v(compr, uncompr, comprLen, uncomprLen)
    char *compr, *uncompr;
    uLong comprLen, *uncomprLen;
{
    int err;
    z_stream d_stream; /* decompression stream */

    d_stream.zalloc = (alloc_func)0;
    d_stream.zfree = (free_func)0;
    d_stream.opaque = (voidpf)0;

#if 1
    if ( Z_DEFLATED != *compr ) {
       return (-3); /* not deflated file */
    } 

    d_stream.next_in  = (Bytef *)(compr + 1); 
    d_stream.avail_in = comprLen - 1;
        d_stream.next_out = (Bytef *)uncompr;            /* discard the output */
        d_stream.avail_out = (uInt)*uncomprLen;

    printf(" compr %p comlen %d, uncom %p, uncomlen %d\n", compr, (unsigned int) comprLen, uncompr, (unsigned int)*uncomprLen);

    err = inflateInit(&d_stream);
    CHECK_ERR(err, "inflateInit");

   /*
    */
    for (;;) {
        d_stream.next_out = (Bytef *)uncompr;            /* discard the output */
        d_stream.avail_out = (uInt)*uncomprLen;
        err = inflate(&d_stream, Z_NO_FLUSH);
        if (err == Z_STREAM_END) break;
        CHECK_ERR(err, "large inflate");
    }

    err = inflateEnd(&d_stream);
    CHECK_ERR(err, "inflateEnd");

#else
    /* 
    printf(" compr %p comlen 0x%x, uncom %p, uncomlen 0x%x\n", compr, comprLen, uncompr, *uncomprLen);
    */

    d_stream.zalloc = (alloc_func)0;
    d_stream.zfree = (free_func)0;
    d_stream.opaque = (voidpf)0;

    d_stream.next_in  = compr;
    d_stream.avail_in = 0;
    d_stream.next_out = uncompr;

    err = inflateInit(&d_stream);
    CHECK_ERR(err, "inflateInit");

    while (d_stream.total_out < *uncomprLen && d_stream.total_in < comprLen) {
        d_stream.avail_in = d_stream.avail_out = 1; /* force small buffers */
        err = inflate(&d_stream, Z_NO_FLUSH);
        if (err == Z_STREAM_END) break;
        CHECK_ERR(err, "inflate");
    }

    err = inflateEnd(&d_stream);
    CHECK_ERR(err, "inflateEnd");
#endif
    
    *uncomprLen = d_stream.total_out;

    return 0;
}


