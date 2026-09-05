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

/** @file ut/ut0page_alloc.cc
 Page-aligned, PFS-instrumented allocation for callers outside InnoDB. See
 include/ut0page_alloc.h for the contract.
 */

#include "univ.i"

#include "ut0new.h"

#include "ut0page_alloc.h"

void *ut_malloc_page_withkey(PSI_memory_key key, std::size_t size) noexcept {
  return ut::malloc_page_withkey(ut::make_psi_memory_key(key), size);
}

bool ut_free_page(void *ptr) noexcept { return ut::free_page(ptr); }
