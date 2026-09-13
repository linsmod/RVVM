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
// When a host injects a VTerm (rvvm_user_set_tty0) and/or registers a TTY
// callback, guest writes to fd 1/2 are fed through the libvterm instance as
// well: the bytes are parsed into a screen matrix (CR, ANSI escapes,
// scrolling, ...) that the host renders itself. This is what makes in-place
// updates (e.g. progress counters using '\r') work consistently across hosts,
// instead of depending on a real tty being attached.
//
// The TTY is not an alternative sink: the same bytes also continue to the
// host's io_callback (rvvm_user_set_io_callback) or, without one, to the host
// fd. That is what keeps the guest console in the host log - logcat under the
// RVVM-GUEST tag plus the Java console bridge on Android, host stdout on
// win32. A host that wants the TTY to be the only sink can consume fd 1/2 in
// its own io_callback.
//
// The opaque `tty` pointer passed to the callback is a libvterm `VTerm*`; cast
// it back after including <vterm.h>. Registering NULL disables TTY parsing.
typedef void (*rvvm_user_tty_callback)(void* userdata, int fd, void* tty);

void rvvm_user_set_tty_callback(rvvm_machine_t* machine, rvvm_user_tty_callback callback, void* userdata);

// Attach a host-owned virtual TTY (a libvterm `VTerm*`, opaque here) to the
// machine. Guest fd 1/2 output is fed into it (with ONLCR emulation) in
// addition to the host fd / io_callback path. Ownership stays with the host:
// the VTerm survives guest exit (keeping the last screen renderable) and
// rvvm_user never frees it. Without this call rvvm_user creates and owns an
// internal VTerm, freed with the machine. Must be called before
// rvvm_user_linux_ex().
void rvvm_user_set_tty0(rvvm_machine_t* machine, void* tty);

// Override the guest's filesystem prefix - the directory guest absolute paths
// are resolved against.
//
//   prefix = NULL  -> host paths pass through unchanged (no sandbox)
//   prefix = "/x"  -> guest "/foo" becomes "/x/foo"; "/dev", "/sys", "/proc",
//                     "/tmp", "/var/tmp" and relative paths still pass through
//
// Passing NULL is how a host asks for passthrough. Setting the environment
// variable RVVM_USER_PREFIX to an empty string means the same, but Win32
// cannot express "set it to empty": MinGW's putenv("NAME=") *removes* the
// variable, and a removed variable means "keep the build-time default". A host
// in that situation must call this instead.
//
// The string is copied and may be freed by the caller. Must be called before
// rvvm_user_linux_ex().
void rvvm_user_set_prefix(rvvm_machine_t* machine, const char* prefix);

// Read back the prefix in effect (NULL when host paths pass through). The
// pointer is owned by the machine.
const char* rvvm_user_get_prefix(rvvm_machine_t* machine);

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
