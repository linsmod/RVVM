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
} elf_desc_t;

bool elf_load_file(rvfile_t* file, elf_desc_t* elf);

// Release the mapping made by a previous elf_load_file() userland load and
// reset the descriptor. Required before re-loading into the same descriptor:
// a stale elf->base makes elf_load_file() take the objcopy path and produces
// a wrongly relocated entry.
void elf_unload_file(elf_desc_t* elf);

bool bin_objcopy(rvfile_t* file, void* buffer, size_t size, bool try_elf);

#endif
