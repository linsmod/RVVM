/*
rvvm_user.h - RVVM Linux binary emulator
Copyright (C) 2024  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef RVVM_USER_H
#define RVVM_USER_H

#include <stddef.h>

// Callback type for guest I/O redirection
// Returns number of bytes written, or -1 on error
typedef ssize_t (*rvvm_user_io_callback)(int fd, const void* buf, size_t count);

// Set custom I/O callback for guest write syscalls
// If callback returns -1, the syscall will fail with errno
void rvvm_user_set_io_callback(rvvm_user_io_callback callback);

// Callback type for guest exit event
// Called when the guest exits (sys_exit or sys_exit_group)
typedef void (*rvvm_user_exit_callback)(int exit_code);

// Set callback invoked when the guest exits
// Called from the guest thread after rvvm_user_linux returns
void rvvm_user_set_exit_callback(rvvm_user_exit_callback callback);

// Just call this like main(), envp may be NULL
int rvvm_user_linux(int argc, char** argv, char** envp);

#endif
