/*
cow_pgt.h - Per-machine copy-on-write page table for guest RAM
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_COW_PGT_H
#define RVVM_COW_PGT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Why this exists
 * ---------------
 * A userland guest process is a whole rvvm_userland_t inside one host process,
 * and rvvm-user runs many of them. Each one needs a contiguous guest-visible RAM
 * window, so rvvm_ram_t::data is one contiguous host allocation of the full
 * guest size. That makes the size a per-process cost of *host address space*,
 * which is the resource that actually runs out:
 *
 *   - a 64-bit host pays commit for the whole thing per process, so the cost
 *     grows as guest_size * processes (measured: 1 GiB per process, 1 child
 *     already 1.22 GB of VSZ with a few MiB resident);
 *   - a 32-bit host has ~2 GiB of user address space in total, so a handful of
 *     128 MiB windows is the ceiling and fork() starts failing outright.
 *
 * The obvious cheaper mechanism - a copy-on-write view of one shared section,
 * FILE_MAP_COPY on Windows, MAP_PRIVATE over a memfd on POSIX - does not help,
 * and it is worth being precise about why, because it looks like it should:
 *
 *   - It shares physical pages, but every machine still gets its own contiguous
 *     window, so the *address space* is still guest_size per process. That is
 *     the cost that has to go away.
 *   - On Windows it does not even save the commit: a FILE_MAP_COPY view is
 *     charged as private commit in full at MapViewOfFile time, exactly like
 *     VirtualAlloc. Measured, 8 views of 512 MiB cost 4104 MiB of PrivateUsage
 *     whether the section is pagefile-, temp-file- or file-backed - identical to
 *     8 x VirtualAlloc(MEM_RESERVE|MEM_COMMIT). SEC_COMMIT|SEC_RESERVE, the one
 *     mechanism that defers the charge, is rejected by CreateFileMapping with
 *     ERROR_INVALID_PARAMETER.
 *   - On POSIX, MAP_NORESERVE does avoid the commit, but commit was never the
 *     POSIX constraint; address space is.
 *
 * So the contiguous window has to be given up. This module keeps one shared,
 * physically-backed, *read-only* base covering the whole guest range, plus a
 * per-machine page table, and translates a guest offset to a host pointer one
 * page at a time:
 *
 *     still shared    base + offset        costs nothing per machine
 *     private         the machine's arena  costs one host page, once
 *
 * A page no machine ever writes is never faulted in, never committed, and costs
 * one bit and one pointer of bookkeeping. A page that is written costs exactly
 * one host page, the first time, for the rest of the machine's life.
 *
 * The base being read-only is the load-bearing part, not hardening. It turns
 * "a call site forgot to unshare before writing" from silent cross-machine
 * corruption into an access violation at the exact instruction that got it
 * wrong. A translation layer whose failure mode is an AV is auditable; one whose
 * failure mode is a wrong answer later is not. It also means every writer in the
 * tree has to be found and classified, which is a bounded, reviewable job rather
 * than an open-ended audit.
 *
 * Fork
 * ----
 * A forked machine inherits its parent's *dirty set*, not its parent's memory:
 * the child gets a copy of the parent's dirty bitmap and a private copy of
 * exactly those pages. Pages the parent never wrote are already zero in the
 * shared base, which is what a fresh machine must see, so they are not copied at
 * all. Cost is therefore
 *
 *     fork:         bitmap + the parent's dirty bytes
 *     steady state: one host page per page first written
 *
 * Note what that does *not* fix: a range the parent wrote and later munmap()ed
 * arrives in the child as a private copy holding stale data, where Linux says a
 * fresh mapping reads as zero. The fork path still has to zero those ranges
 * explicitly. That is a few pages of work, not a redesign, but it does not come
 * for free.
 *
 * Threading
 * ---------
 * A machine is not internally synchronised. It is owned by one thread at a time,
 * in the same way a TLB entry is. Concurrent access to *different* machines is
 * fully parallel and needs no lock, because they share only the immutable base;
 * concurrent access to the *same* machine needs the caller's own exclusion,
 * which is the machine lock RVVM already has. cow_pgt_machine_fork() and
 * cow_pgt_machine_free() are not safe against concurrent cow_pgt_ptr() on the
 * same machine.
 */

/* One shared region: the base plus its own bookkeeping. One per host process in
 * the common case; machines from different regions may coexist. */
typedef struct cow_pgt_region cow_pgt_region_t;

/* One machine's translation table. Holds no guest-visible address, only the
 * answer to "is page N still shared". */
typedef struct cow_pgt_machine cow_pgt_machine_t;

/* Counters, never estimates, so these are usable as an acceptance measurement.
 * Every field is per-machine except the two region-wide ones. */
typedef struct {
    uint64_t guest_size;        /* bytes of guest range covered              */
    uint64_t page_size;         /* translation granularity (host page size)  */
    uint64_t pages_total;       /* guest_size / page_size                   */
    uint64_t pages_private;     /* pages this machine has unshared          */
    uint64_t pages_shared;      /* pages_total - pages_private              */
    uint64_t arena_committed;   /* host bytes committed for the arena       */
    uint64_t bitmap_bytes;      /* per-machine page bitmap                  */
    uint64_t chunk_table_bytes; /* per-machine chunk pointer table          */
} cow_pgt_stats_t;

/*
 * Create a region covering guest_size bytes of guest address space, zero-filled.
 *
 *   page_size       translation granularity; 0 selects the host page size. Must
 *                   be a power of two of at least 64 bytes. This is a *host* page
 *                   size, not a guest one: a guest page larger than this is
 *                   unshared more finely than strictly necessary, which is
 *                   correct and costs a few extra host pages.
 *   pages_per_chunk host pages per arena mapping; 0 selects a default. The arena
 *                   is mapped in chunks so that dirtying thousands of pages does
 *                   not mean thousands of syscalls, and the price is that a chunk
 *                   costs pages_per_chunk even when only one page in it is dirty -
 *                   paid per chunk *touched*, not per page. A guest dirty set is
 *                   normally a few contiguous runs (heap, stack, its own page
 *                   tables), where the waste is one chunk per run. Pass 1 for
 *                   exact accounting if the dirty set is scattered.
 *
 * guest_size is rounded up to a multiple of the page size. Returns NULL on
 * failure.
 */
cow_pgt_region_t* cow_pgt_region_create(size_t guest_size, size_t page_size, size_t pages_per_chunk);

/* Release a region. Every machine created from it must already be freed; a live
 * machine's translations would dangle. NULL is a no-op. */
void cow_pgt_region_destroy(cow_pgt_region_t* region);

/* The shared base, read-only. Exposed for the ELF loader and diagnostics. Do not
 * write through it, and do not cache the pointer across a region destroy. */
const void* cow_pgt_region_base(const cow_pgt_region_t* region);

/* Address space the region itself costs, excluding per-machine tables and
 * arenas: the base, once. Useful as the fixed overhead to subtract before
 * comparing two designs. */
uint64_t cow_pgt_region_bytes(const cow_pgt_region_t* region);

/* A machine with every page shared, i.e. a freshly booted guest. NULL on
 * failure. */
cow_pgt_machine_t* cow_pgt_machine_new(cow_pgt_region_t* region);

/* A machine forked from `parent`: it starts with a private copy of every page
 * the parent has written, and shares the rest. A NULL parent is equivalent to
 * cow_pgt_machine_new(). On failure nothing is changed and NULL is returned. */
cow_pgt_machine_t* cow_pgt_machine_fork(cow_pgt_region_t* region, const cow_pgt_machine_t* parent);

/* Release a machine and its arena. Host pointers previously returned for it
 * become invalid. NULL is a no-op. */
void cow_pgt_machine_free(cow_pgt_machine_t* machine);

/*
 * Translate one page-aligned guest offset to a host pointer.
 *
 *   write = false  the page is still shared, so the base pointer is returned and
 *                  nothing is allocated. This is the hot path: one bitmap test.
 *   write = true   the page is unshared first if needed - one host page mapped
 *                  and the shared contents copied in - and the operation is
 *                  idempotent, so a caller need not know whether a page is
 *                  already private.
 *
 * Reads must pass false. Passing true for a read is correct but forces an
 * unshare that may not have been needed, and a single pass of unsharing a whole
 * guest would cost exactly the memory this module exists to avoid. Writes may
 * pass true unconditionally, which is what makes the integration a mechanical
 * change at every host entry point rather than a per-callsite audit of which
 * ones read and which ones write.
 *
 * The offset must be page-aligned and cover a whole page; a misaligned offset is
 * refused rather than rounded, because a write requested at offset 100 and a
 * read requested at offset 0 name the same page and rounding would hand back a
 * private pointer and a base pointer respectively - a stale read with nothing to
 * explain it.
 *
 * Stability: a pointer for an already-private page stays valid for the life of
 * the machine, because arena slots are never reused. A pointer for a page that
 * is still shared does *not* - after anything writes that page, the base pointer
 * for it is stale, and a cache holding it (a TLB read slot) will keep returning
 * pre-write contents. That is the one hazard in integrating this, and it is
 * cache coherence, not lifetime. See cow_pgt_take_unshared().
 *
 * Returns NULL if the offset is out of range or misaligned, or if a page could
 * not be mapped. On allocation failure the page is left shared and nothing else
 * has changed, so NULL means "out of memory" and is safe to propagate.
 */
void* cow_pgt_ptr(cow_pgt_machine_t* machine, size_t offset, bool write);

/* Read-only variant, for callers that must not be able to unshare. */
const void* cow_pgt_const_ptr(const cow_pgt_machine_t* machine, size_t offset);

/*
 * Translate a whole range, but only if it is genuinely contiguous in host
 * memory. This is the primitive that lets a large guest-supplied range stay a
 * single pointer instead of a copy.
 *
 * Contiguity holds in exactly two cases, and both are structural properties of
 * the layout rather than coincidences:
 *
 *   - every page in the range is still shared, so the range lies in the base; or
 *   - every page in the range is already private *and* they all fall in the same
 *     chunk, because a chunk is one contiguous mapping and a page's address
 *     within it is a pure function of the page index
 *     (chunks[page / pages_per_chunk] + (page % pages_per_chunk) * page_size).
 *     Consecutive private pages in one chunk are therefore adjacent by
 *     construction, and this is what makes the branch usable rather than
 *     accidental.
 *
 * Anything else - a range straddling the shared/private boundary, or spanning
 * two chunks - returns NULL, and the caller is expected to copy it a page at a
 * time instead. That is not a failure: it is the answer. A read of a large
 * untouched range costs nothing at all here, and a write costs only the pages it
 * really writes.
 *
 * With write = true the range is unshared first. It may therefore be left
 * unshared even when this returns NULL, which is safe: a caller that gets NULL
 * falls back to a page loop, which would have unshared the same pages anyway.
 *
 * offset must be page-aligned, and the range must lie inside the region.
 */
void* cow_pgt_ptr_range(cow_pgt_machine_t* machine, size_t offset, size_t size, bool write);

/* Read-only form of the above. Separate rather than a const_cast of the writable
 * one, for the same reason cow_pgt_const_ptr() is separate: an entry point that
 * cannot unshare should not share code with a path that can. */
const void* cow_pgt_const_ptr_range(const cow_pgt_machine_t* machine, size_t offset, size_t size);

/*
 * Collect the guest page range that this machine has unshared since the last
 * call, as [first_page, last_page] inclusive. Returns false if nothing has been
 * unshared, in which case the outputs are untouched.
 *
 * This exists for the TLB. A cached translation for a page that was still shared
 * goes stale the moment anything writes that page, and a TLB read slot holding
 * base + offset will keep returning pre-write contents - which in a guest means
 * a spinlock that never appears to be taken, not a crash. The caller is
 * responsible for invalidating those entries; this only says which ones.
 *
 * It is a bounding interval, not an exact set: a range reported here may contain
 * pages that were unshared earlier and are not themselves newly dirty. That is
 * deliberate - it keeps the bookkeeping two words instead of a queue, and
 * over-reporting costs a redundant invalidation, never a missed one.
 */
bool cow_pgt_take_unshared(cow_pgt_machine_t* machine, size_t* first_page, size_t* last_page);

/* Counters for one machine. Returns false if either argument is NULL. */
bool cow_pgt_stats(const cow_pgt_machine_t* machine, cow_pgt_stats_t* out);

#endif // RVVM_COW_PGT_H
