/*
elf_load.h - ELF Loader
Copyright (C) 2023  LekKit <github.com/LekKit>
              2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef ELF_LOAD_H
#define ELF_LOAD_H

#include "blk_io.h"
#include <core/rvvm.h>

/* Padding past a fixed-address image, usable as guest brk heap.
 *
 * Same number as the userland layout's brk headroom (USERLAND_BRK_MARGIN), and
 * it has to be: the image is placed at USERLAND_DYN_BASE and the brk heap grows
 * into this margin, while mmap()ed ranges start at USERLAND_MMAP_BASE - which is
 * defined as the top of exactly this span. A host that shrinks the address space
 * therefore shrinks this with it, instead of the two disagreeing and a guest
 * brk()ing into the mmap arena.
 *
 * Only the fixed-host-address path below actually reserves it; in guest_window
 * mode (what rvvm-user uses) map_size stays 0 because the window owns the memory
 * and the margin is only headroom, never allocated. */
#define ELF_USERLAND_HEAP_MARGIN USERLAND_BRK_MARGIN

typedef struct {
    // Host pointer to the image: the objcopy caller's buffer, or the address the
    // image was mapped/written at. Filled on every path, and it means three
    // different things depending on the branch elf_load_file() took - which is
    // why the two fields below exist rather than this one carrying the answer.
    void*  base;
    // Objcopy buffer size
    size_t buf_size;

    // Address the image occupies in *guest* address space, and nonzero only when
    // the image actually lives in the userland window (guest_window set). This is
    // the field the userland side wants, and it is the one that has to stay
    // meaningful once guest RAM is no longer one contiguous host allocation:
    // deriving a guest address by subtracting mem.data from base stops working the
    // moment a page moves into a per-machine page table.
    //
    // Zero on the two paths where the image is not in the window: a standalone
    // vma_alloc() buffer for a relocatable image, and the legacy fixed-host-address
    // mode. Note that a stale base used to make userland_fork_memory() compute a
    // meaningless address and copy the wrong range; testing this field instead is
    // what removes that.
    size_t guest_base;

    // True when base is a caller-supplied objcopy buffer, so the image is
    // relocated into that buffer rather than mapped or copied into a window.
    // Was previously inferred from base being non-NULL, which conflated "loaded"
    // with "objcopy" - see the note on elf_unload_file().
    bool   objcopy;

    // Various loaded ELF info
    size_t entry;
    char*  interp_path;
    size_t phdr;
    size_t phnum;

    // Mapping extent allocated by elf_load_file() in userland mode (rounded
    // to the OS allocation granularity); zero in objcopy mode.
    // Released by elf_unload_file().
    size_t map_size;

    // Userland guest memory window: host pointer standing for guest address 0,
    // or NULL to keep mapping the ELF at its link-time *host* address (the
    // legacy identity-mapped mode).
    //
    // When set, the image is copied into window[link_addr ...] instead of being
    // mapped, so nothing has to be free at that address on the host. The window
    // owns the memory, so map_size stays 0 and elf_unload_file() won't free it.
    uint8_t* guest_window;

    // Guest address to place a relocatable (ET_DYN) image at when guest_window
    // is set; ignored otherwise. Zero means "pick one", which only works
    // without a window.
    size_t load_addr;

    // Entry symbol to jump to when the image carries no entry point of its own
    // (e_entry == 0, as in a shared object). NULL keeps the ELF entry.
    const char* entry_symbol;
} elf_desc_t;

bool elf_load_file(rvfile_t* file, elf_desc_t* elf);

// Guest address space an ELF image occupies: the memsz-inclusive extent of its
// PT_LOAD/PT_PHDR segments, rounded to the VMA allocation granularity (the same
// extent elf_load_file() reports via buf_size). The plain file size is NOT
// enough - it omits the trailing .bss, which is where musl's ldso keeps its
// internal locks, so an undersized reservation lets a later guest mmap() land
// on top of them. Returns 0 if the file can't be parsed.
size_t elf_image_extent(rvfile_t* file);

// Release the mapping made by a previous elf_load_file() userland load and
// reset the descriptor. Required before re-loading into the same descriptor.
//
// The contract used to be "a stale elf->base makes elf_load_file() take the
// objcopy path", which made "have I loaded into this descriptor?" a question
// about a pointer's nullness and forced every caller to remember to unload.
// The mode is now an explicit field, so zeroing the descriptor is the whole
// contract: this memsets it, which clears base, guest_base and objcopy together.
void elf_unload_file(elf_desc_t* elf);

bool bin_objcopy(rvfile_t* file, void* buffer, size_t size, bool try_elf);

#endif
