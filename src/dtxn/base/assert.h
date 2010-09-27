// Copyright 2008,2009,2010 Massachusetts Institute of Technology.
// All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef ASSERT_H__
#define ASSERT_H__

#include <cassert>
#include <cstdio>
#include <cstdlib>

// Wraps the standard assert macro to avoids "unused variable" warnings when compiled away.
// Inspired by: http://cnicholson.net/2009/02/stupid-c-tricks-adventures-in-assert/
// This is not the "default" because it does not conform to the requirements of the C standard,
// which requires that the NDEBUG version be ((void) 0).
#ifdef NDEBUG
#define ASSERT(x) do { (void)sizeof(x); } while(0)
#else
#define ASSERT(x) assert(x)
#endif


// CHECK is always enabled. May only work with gcc because I stole this from assert.h
// TODO: Add our own .cc implementation to avoid duplicating this all over?
// TODO: Use a namespaced function to avoid namespace/class member conflicts?
// TODO: Add unit tests.
#define CHECK(x) do { \
    if (!(x)) { \
        ::fprintf(stderr, "CHECK failed: %s at %s:%d in function %s\n", \
                #x, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
        ::abort(); \
    } \
} while (0)

#define CHECK_M(x, message, args...) do { \
    if (!(x)) { \
        ::fprintf(stderr, "CHECK failed: %s at %s:%d in function %s\n" message "\n", \
                #x, __FILE__, __LINE__, __PRETTY_FUNCTION__, ##args); \
        ::abort(); \
    } \
} while (0)


#endif
