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

typedef struct {
    // Pass a buffer for objcopy, NULL for userland loading
    // Receive base ELF address for userland
    void*  base;
    // Objcopy buffer size
    size_t buf_size;

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
// reset the descriptor. Required before re-loading into the same descriptor:
// a stale elf->base makes elf_load_file() take the objcopy path and produces
// a wrongly relocated entry.
void elf_unload_file(elf_desc_t* elf);

bool bin_objcopy(rvfile_t* file, void* buffer, size_t size, bool try_elf);

#endif
