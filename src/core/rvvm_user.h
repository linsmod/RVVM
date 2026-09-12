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
#include <stdint.h>
#include <stdbool.h>

#include <rvvm/rvvm_base.h> /* rvvm_machine_t handle */

// Callback type for guest I/O redirection
// Returns number of bytes written, or -1 on error
typedef ssize_t (*rvvm_user_io_callback)(int fd, const void* buf, size_t count);

// Set custom I/O callback for guest write syscalls
// If callback returns -1, the syscall will fail with errno
void rvvm_user_set_io_callback(rvvm_machine_t* machine, rvvm_user_io_callback callback);

// Callback type for guest exit event
// Called when the guest exits (sys_exit or sys_exit_group)
typedef void (*rvvm_user_exit_callback)(int exit_code);

// Set callback invoked when the guest exits
// Called from the guest thread after the guest unwinds
void rvvm_user_set_exit_callback(rvvm_machine_t* machine, rvvm_user_exit_callback callback);

// --- Guest virtual TTY (libvterm-backed) ---
//
// When a host registers a TTY callback, guest writes to fd 1/2 are fed through
// a libvterm instance instead of being written straight to the host terminal.
// libvterm parses the raw byte stream (CR, ANSI escapes, scrolling, ...) and
// the host renders the resulting screen state itself. This is what makes
// in-place updates (e.g. progress counters using '\r') work consistently across
// hosts, instead of depending on a real tty being attached (which only happens
// on the win32 host today).
//
// The opaque `tty` pointer passed to the callback is a libvterm `VTerm*`; cast
// it back after including <vterm.h>. Registering NULL disables TTY parsing and
// restores the default behaviour (raw write to the host fd).
typedef void (*rvvm_user_tty_callback)(void* userdata, int fd, void* tty);

void rvvm_user_set_tty_callback(rvvm_machine_t* machine, rvvm_user_tty_callback callback, void* userdata);

// Create a userland machine instance without starting it.
//
// This is the multi-instance entry point: everything the guest needs (memory,
// harts, syscall state) hangs off the returned machine, so several instances
// can coexist in one process. Set callbacks on it, then hand it to
// rvvm_user_linux_ex() from a dedicated thread.
//
// Returns NULL when userland emulation is unavailable (RVVM_USER_TEST not
// defined) or on allocation failure.
rvvm_machine_t* rvvm_user_create(void);

// Destroy a machine obtained from rvvm_user_create() and release its context.
//
// Do NOT call this on an instance that rvvm_user_linux_ex() has already
// returned from - that function owns the machine and frees it itself.
void rvvm_user_free(rvvm_machine_t* machine);

// Run a guest on @machine until it exits. Blocks the calling thread; the
// machine (and its context) is freed before returning.
// Just call this like main(), envp may be NULL. Returns the guest exit status.
int rvvm_user_linux_ex(rvvm_machine_t* machine, int argc, char** argv, char** envp);

// Convenience single-instance entry point: creates a machine, runs the guest
// on it and returns the guest exit status. Just call this like main(), envp
// may be NULL.
int rvvm_user_linux(int argc, char** argv, char** envp);

// Stop a guest from host code (e.g. a GUI thread), for guests that ignore the
// cooperative teardown the host normally asks for.
//
// This is NOT TerminateThread(): every guest vCPU is kicked out of the
// interpreter and unhooked from its run loop, so the emulator unwinds exactly
// like a guest sys_exit_group(exit_code) would - the exit callback fires, each
// guest thread runs its own cleanup, and rvvm_user_linux_ex() returns on its own.
//
// Safe to call from any thread while rvvm_user_linux_ex() is running. A no-op
// when no guest is running. The exit callback, if set, is invoked on the
// calling thread instead of a guest thread.
void rvvm_user_stop(rvvm_machine_t* machine, int exit_code);

// Suspend a running guest from host code (e.g. a GUI thread).
//
// Every guest vCPU is stopped at an instruction boundary and parked until
// rvvm_user_resume(). This is reversible: nothing is torn down, no exit
// callback fires, and on resume each vCPU continues exactly where it stopped.
//
// Safe to call from any thread while rvvm_user_linux_ex() is running; a no-op
// (and true) when the guest is already suspended, or when no guest is running.
//
// Blocks (bounded) until every vCPU has parked and returns true. It returns
// false when a guest thread is stuck in a blocking host syscall - it will park
// as soon as that syscall returns, so the guest is not left half-paused, just
// not stopped yet.
bool rvvm_user_suspend(rvvm_machine_t* machine);

// Resume a guest suspended with rvvm_user_suspend(). No-op otherwise.
void rvvm_user_resume(rvvm_machine_t* machine);

// True while the guest is suspended. Note this reports the requested state:
// right after rvvm_user_suspend() returns false, the guest is suspended but
// some vCPU may still be draining a blocking host syscall.
bool rvvm_user_is_suspended(rvvm_machine_t* machine);

// Translate a guest virtual address into a host pointer.
//
// Guest memory is a private buffer of the userland machine, so a guest address
// is NOT a host address. Host code that reads or writes guest memory directly
// (syscall structs, the Android NDK proxy, ...) must go through this.
//
// Returns NULL when @addr is not a mappable guest address (NULL, or outside
// the guest address space - e.g. a host handle the guest passed back).
void* rvvm_user_guest_ptr(uint64_t addr);

// Inverse of rvvm_user_guest_ptr(): host pointer inside guest memory -> guest
// address. Returns 0 when @ptr does not belong to guest memory.
uint64_t rvvm_user_host_ptr(const void* ptr);

#endif
