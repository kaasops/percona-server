/* Copyright (c) 2026, Alexey Bychko.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file include/ut0page_alloc.h
 Page-aligned, PFS-instrumented allocation for callers outside InnoDB.

 Declarations here are deliberately self-contained: they pull in neither
 univ.i nor ut0new.h, so that translation units outside storage/innobase/
 (sql/ in particular) can include this header by its root-relative path,
 the way plugin/fulltext/ngram_parser/plugin_ngram.cc includes
 storage/innobase/include/fts0tokenize.h. ut0new.h itself is not usable
 that way: it requires its own translation unit to include "univ.i" first.

 Contract:
 - the returned pointer is page-aligned; the underlying allocator maps
   round_up(size + CPU_PAGE_SIZE, CPU_PAGE_SIZE), not size rounded up alone
   -- a full extra page is reserved ahead of the returned pointer for the
   allocator's own metadata, so the mapped total (and the amount PFS
   accounts for) is size plus that page, not the requested size. On
   failure the functions return nullptr and never throw.
 - ut_free_page(nullptr) returns false and is safe to call.
 - @p key is an ordinary PFS memory key, registered by whichever server
   subsystem owns it; InnoDB forwards it as-is into PSI_MEMORY_CALL
   (memory_alloc) and keeps no key registry of its own. In a build with
   memory instrumentation disabled (-DDISABLE_PSI_MEMORY=ON, a real
   ADVANCED cmake option), the key is silently ignored: the SFINAE
   dispatcher inside ut0new.h selects a non-PFS implementation that takes
   the key as an unnamed parameter.
 - on mmap failure InnoDB itself logs a warning (ER_IB_MSG_856); the
   caller is still responsible for handling nullptr and reporting its own
   failure, and must not treat InnoDB's warning as a substitute.
 */

#ifndef ut0page_alloc_h
#define ut0page_alloc_h

#include <cstddef>

#include "mysql/psi/psi_memory.h"

/** Allocates page-aligned, PFS-instrumented storage on behalf of a caller
 outside storage/innobase/.
 @param[in] key PSI memory key to be used for PFS memory instrumentation.
 @param[in] size Size of storage (in bytes) requested to be allocated.
 @return Pointer to the page-aligned storage; nullptr if allocation failed.
 */
void *ut_malloc_page_withkey(PSI_memory_key key, std::size_t size) noexcept;

/** Releases storage obtained through ut_malloc_page_withkey().
 @param[in] ptr Pointer obtained through ut_malloc_page_withkey(), or nullptr.
 @return true if releasing the page-aligned memory was successful.
 */
bool ut_free_page(void *ptr) noexcept;

#endif /* ut0page_alloc_h */
