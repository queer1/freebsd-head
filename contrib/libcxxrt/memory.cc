/* 
 * Copyright 2010-2011 PathScale, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * memory.cc - Contains stub definition of C++ new/delete operators.
 *
 * These definitions are intended to be used for testing and are weak symbols
 * to allow them to be replaced by definitions from a STL implementation.
 * These versions simply wrap malloc() and free(), they do not provide a
 * C++-specific allocator.
 */

#include <stddef.h>
#include <stdlib.h>
#include "stdexcept.h"

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if !__has_builtin(__sync_swap)
#define __sync_swap __sync_lock_test_and_set
#endif

namespace std
{
	struct nothrow_t {};
}


/// The type of the function called when allocation fails.
typedef void (*new_handler)();
/**
 * The function to call when allocation fails.  By default, there is no
 * handler and a bad allocation exception is thrown if an allocation fails.
 */
static new_handler new_handl;

namespace std
{
	/**
	 * Sets a function to be called when there is a failure in new.
	 */
	__attribute__((weak))
	new_handler set_new_handler(new_handler handler)
	{
		return __sync_swap(&new_handl, handler);
	}
}


__attribute__((weak))
void* operator new(size_t size)
{
	void * mem = malloc(size);
	while (0 == mem)
	{
		if (0 != new_handl)
		{
			new_handl();
		}
		else
		{
			throw std::bad_alloc();
		}
		mem = malloc(size);
	}

	return mem;
}

__attribute__((weak))
void* operator new(size_t size, const std::nothrow_t &) throw()
{
	void *mem = malloc(size);
	while (0 == mem)
	{
		if (0 != new_handl)
		{
			try
			{
				new_handl();
			}
			catch (...)
			{
				// nothrow operator new should return NULL in case of
				// std::bad_alloc exception in new handler
				return NULL;
			}
		}
		else
		{
			return NULL;
		}
		mem = malloc(size);
	}

	return mem;
}


__attribute__((weak))
void operator delete(void * ptr)
{
	free(ptr);
}


__attribute__((weak))
void * operator new[](size_t size)
{
	return ::operator new(size);
}


__attribute__((weak))
void operator delete[](void * ptr) throw()
{
	::operator delete(ptr);
}


