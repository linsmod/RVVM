/*
cow_pgt.c - Per-machine copy-on-write page table for guest RAM
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// The design and its rationale are in cow_pgt.h. What follows is the set of
// choices that are not obvious from that, and the two alternatives that are
// worse, because those are the ones a reader will otherwise propose.

#include "cow_pgt.h"

#include "compiler.h"
#include "utils.h"
#include "vma_ops.h"

#include <stdlib.h>
#include <string.h>

/*
 * Per-machine layout
 * ------------------
 *   bitmap   one bit per guest page, 1 = private. Read on every translation and
 *            written exactly once per page, so a plain array of 64-bit words is
 *            the right structure and nothing faster is needed.
 *
 *   chunks   an array of chunk pointers indexed by *guest page number* divided
 *            by the chunk size, each entry NULL until that range is first
 *            dirtied. This is the one decision worth stating outright, because
 *            the obvious alternatives are worse:
 *
 *              - a flat table of host pointers per page would be 8 bytes per
 *                page - 1 MiB for a 512 MiB guest - and would have to be filled
 *                in for the whole range at fork, turning a 16 KiB fork into a
 *                2 MiB one. That is the linear cost moved somewhere else, which
 *                is not a solution.
 *
 *              - a bump-allocated arena plus a page-to-slot index needs a second
 *                per-page structure to answer "which slot is page N", or a
 *                popcount, which puts an O(pages) loop in the translation path.
 *
 *            Indexing the chunk array by page number makes a private
 *            translation a single indexed load, and costs one pointer per
 *            pages_per_chunk pages. It also makes chunk-internal contiguity a
 *            property of the address arithmetic rather than something to be
 *            verified, which is what cow_pgt_ptr_range() relies on.
 *
 *   arena    the pages themselves, mapped in chunks so that dirtying thousands of
 *            pages does not mean thousands of syscalls. Slots are never freed or
 *            reused within a machine's lifetime, which is what makes unshare
 *            O(1) and order-independent: a page that has been modified holds
 *            data no other machine may see, so there is nothing to reclaim even
 *            when the guest abandons it.
 *
 * Fork is then a direct page-for-page copy of the parent's dirty set, using the
 * bitmap to find the pages and the chunk array to find them. No ordering
 * invariant has to hold between the two structures, which is a good property to
 * have in a mechanism whose failure mode is a wrong answer rather than a crash.
 */

/* Host pages per arena chunk when the caller does not choose. See
 * cow_pgt.h for the tradeoff; 16 pages is 64 KiB, which bounds the damage at
 * 16x on a pathological scattered dirty set while keeping a 512 MiB guest's
 * chunk pointer table at 64 KiB. */
#define COW_PGT_CHUNK_PAGES 16

struct cow_pgt_region {
    uint8_t*        base;        // read-only, page aligned, guest_size bytes
    size_t          guest_size;
    size_t          page_size;
    uint64_t        pages;
    size_t          bitmap_words;
    size_t          chunk_pages;  // host pages per arena mapping
    size_t          chunk_slots;  // pages / chunk_pages, rounded up
    uint64_t        machines;     // live machines, for accounting
};

struct cow_pgt_machine {
    cow_pgt_region_t* region;
    uint64_t*         bitmap;
    uint8_t**         chunks;      // indexed by page / chunk_pages
    size_t            chunk_count;
    // Bounding interval of pages unshared since the last take, for TLB
    // invalidation. Two words rather than a queue: over-reporting costs a
    // redundant invalidation, under-reporting would be a missed one.
    size_t            dirty_first;
    size_t            dirty_last;
    bool              dirty_pending;
};

static inline size_t cow_pgt_align_up(size_t value, size_t align)
{
    size_t rem = value % align;
    return rem ? value + (align - rem) : value;
}

static inline bool cow_pgt_bit_test(const uint64_t* bitmap, size_t i)
{
    return (bitmap[i >> 6] >> (i & 63)) & 1u;
}

static inline void cow_pgt_bit_set(uint64_t* bitmap, size_t i)
{
    bitmap[i >> 6] |= (uint64_t)1 << (i & 63);
}

static uint64_t cow_pgt_popcount(const uint64_t* words, size_t n)
{
    size_t   i;
    uint64_t total = 0;
    for (i = 0; i < n; i++) {
        uint64_t v = words[i];
        while (v) {
            v &= v - 1;
            total++;
        }
    }
    return total;
}

/*
 * Region
 */

cow_pgt_region_t* cow_pgt_region_create(size_t guest_size, size_t page_size, size_t pages_per_chunk)
{
    cow_pgt_region_t* region;

    if (guest_size == 0) {
        rvvm_error("cow_pgt: guest size is zero");
        return NULL;
    }
    if (page_size == 0) {
        page_size = vma_page_size();
    }
    // A power of two, and not so small that one bitmap word spans a meaningless
    // number of pages.
    if (page_size < 64 || (page_size & (page_size - 1))) {
        rvvm_error("cow_pgt: translation granularity %llu is not a usable power of two",
                   (unsigned long long)page_size);
        return NULL;
    }
    if (pages_per_chunk == 0) {
        pages_per_chunk = COW_PGT_CHUNK_PAGES;
    }
    // A chunk larger than the whole region is not an error: it wastes some
    // address space on one mapping, which is cheaper than refusing to run.
    if (guest_size > (size_t)-1 - (page_size - 1)) {
        rvvm_error("cow_pgt: guest size overflows");
        return NULL;
    }
    guest_size = cow_pgt_align_up(guest_size, page_size);

    region = (cow_pgt_region_t*)calloc(1, sizeof *region);
    if (!region) {
        return NULL;
    }

    region->page_size    = page_size;
    region->guest_size   = guest_size;
    region->pages        = (uint64_t)(guest_size / page_size);
    region->bitmap_words = (size_t)((region->pages + 63) / 64);
    region->chunk_pages  = pages_per_chunk;
    region->chunk_slots  = (size_t)((region->pages + pages_per_chunk - 1) / pages_per_chunk);

    // Committed read-only from the start. This is the invariant the whole design
    // rests on: a missed unshare has to fault, not corrupt. It is requested here
    // and never lifted, which is also why there is no seeding entry point - see
    // the note in cow_pgt.h about execve() reloading an image.
    region->base = (uint8_t*)vma_alloc(NULL, guest_size, VMA_READ);
    if (!region->base) {
        rvvm_error("cow_pgt: cannot map a %llu byte read-only base",
                   (unsigned long long)guest_size);
        free(region);
        return NULL;
    }
    return region;
}

void cow_pgt_region_destroy(cow_pgt_region_t* region)
{
    if (!region) {
        return;
    }
    if (region->base) {
        vma_free(region->base, region->guest_size);
    }
    free(region);
}

const void* cow_pgt_region_base(const cow_pgt_region_t* region)
{
    return region ? (const void*)region->base : NULL;
}

uint64_t cow_pgt_region_bytes(const cow_pgt_region_t* region)
{
    return region ? (uint64_t)region->guest_size : 0;
}

/*
 * Arena
 */

/* Address of `page` in this machine's arena, or NULL if its chunk is not mapped
 * and create is false. A page whose chunk is absent is distinguishable from a
 * private one without consulting the bitmap twice. */
static uint8_t* cow_pgt_arena_page(cow_pgt_machine_t* machine, size_t page, bool create)
{
    size_t   ci   = page / machine->region->chunk_pages;
    size_t   ps   = machine->region->page_size;
    uint8_t** slot = &machine->chunks[ci];

    if (!*slot) {
        uint8_t* p;
        if (!create) {
            return NULL;
        }
        p = (uint8_t*)vma_alloc(NULL, ps * machine->region->chunk_pages, VMA_RDWR);
        if (!p) {
            return NULL;
        }
        *slot = p;
        machine->chunk_count++;
    }
    // A pure function of the page index into one contiguous per-chunk mapping.
    // This is what makes consecutive private pages in a chunk adjacent, and
    // therefore what cow_pgt_ptr_range() is allowed to rely on.
    return *slot + (page % machine->region->chunk_pages) * ps;
}

/* Read-only counterpart, for callers that hold a const machine (the fork source
 * and the const translation entry point). Split out rather than casting the
 * const away, so a read path can never reach a function that mutates.
 *
 * The caller must have established that `page` is private. That implies its
 * chunk is mapped, because the bitmap bit is only set after the arena mapping
 * succeeded - so there is no NULL case here to handle. */
static const uint8_t* cow_pgt_arena_page_const(const cow_pgt_machine_t* machine, size_t page)
{
    return machine->chunks[page / machine->region->chunk_pages] +
           (page % machine->region->chunk_pages) * machine->region->page_size;
}

/* Resolve a page-aligned offset to a page index. False if the offset does not
 * name a whole page inside the region: misaligned offsets are refused rather
 * than rounded (see cow_pgt.h), and one in the final partial page is out of
 * range rather than clamped to the base. */
static bool cow_pgt_page_of(const cow_pgt_region_t* region, size_t offset, size_t* page)
{
    if (offset & (region->page_size - 1)) {
        return false;
    }
    if (offset > region->guest_size || (region->guest_size - offset) < region->page_size) {
        return false;
    }
    *page = offset / region->page_size;
    return true;
}

/*
 * Machines
 */

static void cow_pgt_machine_teardown(cow_pgt_machine_t* machine)
{
    size_t i;
    for (i = 0; i < machine->region->chunk_slots; i++) {
        if (machine->chunks[i]) {
            vma_free(machine->chunks[i], machine->region->page_size * machine->region->chunk_pages);
        }
    }
    free(machine->chunks);
    free(machine->bitmap);
    free(machine);
}

static cow_pgt_machine_t* cow_pgt_machine_alloc(cow_pgt_region_t* region)
{
    cow_pgt_machine_t* machine = (cow_pgt_machine_t*)calloc(1, sizeof *machine);

    if (!machine) {
        return NULL;
    }
    machine->region = region;
    machine->bitmap = (uint64_t*)calloc(region->bitmap_words, sizeof(uint64_t));
    machine->chunks = (uint8_t**)calloc(region->chunk_slots, sizeof *machine->chunks);
    if (!machine->bitmap || !machine->chunks) {
        cow_pgt_machine_teardown(machine);
        return NULL;
    }
    return machine;
}

cow_pgt_machine_t* cow_pgt_machine_new(cow_pgt_region_t* region)
{
    cow_pgt_machine_t* machine;

    if (!region) {
        return NULL;
    }
    machine = cow_pgt_machine_alloc(region);
    if (!machine) {
        return NULL;
    }
    region->machines++;
    return machine;
}

cow_pgt_machine_t* cow_pgt_machine_fork(cow_pgt_region_t* region, const cow_pgt_machine_t* parent)
{
    cow_pgt_machine_t* machine;
    size_t   word;
    size_t   page = 0;

    if (!region) {
        return NULL;
    }
    if (!parent) {
        return cow_pgt_machine_new(region);
    }
    if (parent->region != region) {
        rvvm_error("cow_pgt: cannot fork across regions");
        return NULL;
    }

    machine = cow_pgt_machine_alloc(region);
    if (!machine) {
        return NULL;
    }

    // The child starts with the parent's dirty set. The bitmap is copied first
    // and in one piece: it is small, and doing it as a memcpy rather than bit by
    // bit means the child and the parent can never disagree about which pages
    // exist.
    if (region->bitmap_words) {
        memcpy(machine->bitmap, parent->bitmap, region->bitmap_words * sizeof(uint64_t));
    }

    // Then the dirty set is materialised into the child. Pages absent from the
    // bitmap are not copied: the base already holds what a freshly booted
    // machine must see, and the child shares it read-only until it writes. So
    // this loop is the complete copy, and its cost is the parent's dirty bytes
    // rather than the guest's size.
    for (word = 0; word < region->bitmap_words; word++) {
        uint64_t w = parent->bitmap[word];
        while (w) {
            uint8_t* dst;
            int      bit = 0;
            uint64_t rest = w;

            // Isolate the lowest set bit without a builtin: MSVC has no
            // __builtin_ctzll and this file has to build there too.
            while ((rest & 1u) == 0u) {
                rest >>= 1;
                bit++;
            }
            w &= w - 1u;
            page = word * 64 + (size_t)bit;

            dst = cow_pgt_arena_page(machine, page, true);
            if (!dst) {
                cow_pgt_machine_teardown(machine);
                return NULL;
            }
            // The bit is set, so the parent's chunk for this page is mapped.
            memcpy(dst, cow_pgt_arena_page_const(parent, page), region->page_size);
        }
    }

    region->machines++;
    return machine;
}

void cow_pgt_machine_free(cow_pgt_machine_t* machine)
{
    if (!machine) {
        return;
    }
    if (machine->region && machine->region->machines) {
        machine->region->machines--;
    }
    cow_pgt_machine_teardown(machine);
}

/*
 * Translation
 */

void* cow_pgt_ptr(cow_pgt_machine_t* machine, size_t offset, bool write)
{
    cow_pgt_region_t* region;
    size_t            page;

    if (!machine) {
        return NULL;
    }
    region = machine->region;
    if (!cow_pgt_page_of(region, offset, &page)) {
        return NULL;
    }

    if (cow_pgt_bit_test(machine->bitmap, page)) {
        // Private already. Idempotent, so a caller need not know whether a page
        // has been unshared before.
        return cow_pgt_arena_page(machine, page, false);
    }
    if (!write) {
        // The hot path: still shared, so the base pointer is the answer and
        // nothing is allocated.
        return region->base + offset;
    }

    {
        uint8_t* slot = cow_pgt_arena_page(machine, page, true);
        if (!slot) {
            // Left shared, nothing else changed: the caller may treat NULL as
            // out of memory and propagate it.
            return NULL;
        }
        memcpy(slot, region->base + page * region->page_size, region->page_size);
        // Bit and bump advance together with nothing between them that can
        // return early.
        cow_pgt_bit_set(machine->bitmap, page);

        if (!machine->dirty_pending) {
            machine->dirty_first = page;
            machine->dirty_last  = page;
            machine->dirty_pending = true;
        } else {
            if (page < machine->dirty_first) {
                machine->dirty_first = page;
            }
            if (page > machine->dirty_last) {
                machine->dirty_last = page;
            }
        }
        return slot;
    }
}

const void* cow_pgt_const_ptr(const cow_pgt_machine_t* machine, size_t offset)
{
    size_t page;

    if (!machine || !cow_pgt_page_of(machine->region, offset, &page)) {
        return NULL;
    }
    // Deliberately not cow_pgt_ptr() with the const cast off: this entry point
    // must not be able to unshare, so it cannot share code with a path that can.
    if (cow_pgt_bit_test(machine->bitmap, page)) {
        return cow_pgt_arena_page_const(machine, page);
    }
    return machine->region->base + offset;
}

void* cow_pgt_ptr_range(cow_pgt_machine_t* machine, size_t offset, size_t size, bool write)
{
    cow_pgt_region_t* region;
    size_t            page;
    size_t            last;
    size_t            i;

    if (!machine || size == 0) {
        return NULL;
    }
    region = machine->region;
    if (offset & (region->page_size - 1)) {
        return NULL;
    }
    if (offset > region->guest_size || size > region->guest_size - offset) {
        return NULL;
    }

    page = offset / region->page_size;
    last = (offset + size - 1) / region->page_size;

    if (write) {
        // Unshare the whole range first. This may leave the range unshared even
        // when the call goes on to return NULL, which is safe: the caller falls
        // back to a page loop that would have unshared the same pages.
        for (i = page; i <= last; i++) {
            if (!cow_pgt_ptr(machine, i * region->page_size, true)) {
                return NULL;
            }
        }
        // Now every page is private, so the answer depends only on whether they
        // share a chunk.
        if (last / region->chunk_pages != page / region->chunk_pages) {
            return NULL; // spans two chunks
        }
        return cow_pgt_arena_page(machine, page, false) + (offset - page * region->page_size);
    }

    // Read path: no state changes, so the first page decides. If it is shared
    // they are all shared (a page is only ever unshared, never re-shared), and
    // the range lies in the base. If it is private, the range is contiguous only
    // within one chunk.
    if (!cow_pgt_bit_test(machine->bitmap, page)) {
        for (i = page + 1; i <= last; i++) {
            if (cow_pgt_bit_test(machine->bitmap, i)) {
                return NULL; // straddles the shared/private boundary
            }
        }
        return region->base + offset;
    }
    if (last / region->chunk_pages != page / region->chunk_pages) {
        return NULL;
    }
    for (i = page + 1; i <= last; i++) {
        if (!cow_pgt_bit_test(machine->bitmap, i)) {
            return NULL;
        }
    }
    return cow_pgt_arena_page(machine, page, false) + (offset - page * region->page_size);
}

const void* cow_pgt_const_ptr_range(const cow_pgt_machine_t* machine, size_t offset, size_t size)
{
    // Same test as the writable read path, on const data. See the note on
    // cow_pgt_ptr_range() for why contiguity holds in exactly these two cases.
    const cow_pgt_region_t* region;
    size_t                  page;
    size_t                  last;
    size_t                  i;

    if (!machine || size == 0) {
        return NULL;
    }
    region = machine->region;
    if (offset & (region->page_size - 1)) {
        return NULL;
    }
    if (offset > region->guest_size || size > region->guest_size - offset) {
        return NULL;
    }
    page = offset / region->page_size;
    last = (offset + size - 1) / region->page_size;

    if (!cow_pgt_bit_test(machine->bitmap, page)) {
        for (i = page + 1; i <= last; i++) {
            if (cow_pgt_bit_test(machine->bitmap, i)) {
                return NULL;
            }
        }
        return region->base + offset;
    }
    if (last / region->chunk_pages != page / region->chunk_pages) {
        return NULL;
    }
    for (i = page + 1; i <= last; i++) {
        if (!cow_pgt_bit_test(machine->bitmap, i)) {
            return NULL;
        }
    }
    return cow_pgt_arena_page_const(machine, page) + (offset - page * region->page_size);
}

bool cow_pgt_take_unshared(cow_pgt_machine_t* machine, size_t* first_page, size_t* last_page)
{
    if (!machine || !machine->dirty_pending) {
        return false;
    }
    if (first_page) {
        *first_page = machine->dirty_first;
    }
    if (last_page) {
        *last_page = machine->dirty_last;
    }
    machine->dirty_pending = false;
    return true;
}

bool cow_pgt_stats(const cow_pgt_machine_t* machine, cow_pgt_stats_t* out)
{
    uint64_t priv;

    if (!machine || !out) {
        return false;
    }
    priv = cow_pgt_popcount(machine->bitmap, machine->region->bitmap_words);

    memset(out, 0, sizeof *out);
    out->guest_size        = (uint64_t)machine->region->guest_size;
    out->page_size         = (uint64_t)machine->region->page_size;
    out->pages_total       = machine->region->pages;
    out->pages_private     = priv;
    out->pages_shared      = machine->region->pages - priv;
    // Rounded up to the chunk, so the slack is reported rather than hidden.
    out->arena_committed   = (uint64_t)machine->chunk_count *
                              machine->region->chunk_pages * machine->region->page_size;
    out->bitmap_bytes      = (uint64_t)machine->region->bitmap_words * sizeof(uint64_t);
    out->chunk_table_bytes = (uint64_t)machine->region->chunk_slots * sizeof(uint8_t*);
    return true;
}
