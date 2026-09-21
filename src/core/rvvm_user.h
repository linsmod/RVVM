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

// Opaque host context bound to a machine's userland instance. The host stores
// whatever it needs to reach from a guest's syscall path (VirtPass stores its
// vp_cmdpost_t there); the core only passes it back, and never dereferences it.
// One context per machine, so a second guest in the same process cannot be
// handed the first one's.
void  rvvm_user_set_host_ctx(rvvm_machine_t* machine, void* ctx);
void* rvvm_user_host_ctx(rvvm_machine_t* machine);

// The machine whose vCPU is running on the calling thread, or NULL when the
// thread is not servicing a guest. Lets a host map "the thread that tripped
// an event" back to the run it belongs to when several machines are alive.
// Note: prefer callbacks that carry the machine explicitly - the exit
// callback does - over TLS-based lookup, which only works on vCPU threads.
rvvm_machine_t* rvvm_user_current_machine(void);

// Callback type for guest exit event
// Called when the guest exits (sys_exit or sys_exit_group), and also when a
// host-initiated stop (rvvm_user_stop) unwinds the guest. The machine is
// passed explicitly: rvvm_user_stop may be called from any host thread, so
// the callback can fire off a vCPU thread where TLS-based identity lookup
// would find nothing.
typedef void (*rvvm_user_exit_callback)(rvvm_machine_t* machine, int exit_code);

// Set callback invoked when the guest exits
// Called with the exiting machine and its exit code. Fires on a guest vCPU
// thread for a guest-driven exit, or on the thread that called
// rvvm_user_stop() for a host-initiated stop.
void rvvm_user_set_exit_callback(rvvm_machine_t* machine, rvvm_user_exit_callback callback);

// --- Guest virtual TTY (libvterm-backed) ---
//
// When a host attaches a session (rvvm_tty_attach) and/or registers a TTY
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

// --- Host-owned terminal session ---
//
// A console is a session, not a property of a run. The session owns the screen
// matrix and the lock that serializes every access to it, and it outlives the
// guest - which is what lets a host keep showing, and scrolling through, the
// last screen after the run ends (Ctrl-C included). Open one for the lifetime
// of the host's console, attach it before each run, detach it once the guest
// thread is gone:
//
//     rvvm_tty_t* tty = rvvm_tty_open(24, 80);
//     rvvm_tty_attach(tty, machine);      // guest fd 1/2 parses into it
//     ... rvvm_user_linux_ex() ...
//     rvvm_tty_detach(tty, machine);      // screen, lock and size stay valid
//     rvvm_tty_close(tty);
//
// Everything a renderer needs hangs off the session and needs no machine: it
// owns the packed cells and the scrollback behind rvvm_tty_snapshot(), the view
// position behind rvvm_tty_scroll(), and the repaint serial. A host that wants
// to talk to libvterm itself (a diagnostic dump, say) can take the lock and go
// through rvvm_tty_vterm(). The grid size belongs to whoever opened the session
// (the Android console derives its row count from its viewport, for example)
// and the guest observes it through TIOCGWINSZ. Only when no session is
// attached does rvvm_user report its built-in default grid.
//
// Without an attached session rvvm_user creates and owns an internal one as
// soon as a TTY callback is registered, and frees it with the machine.
typedef struct rvvm_tty rvvm_tty_t;

// One screen cell as the renderers consume it. The layout is fixed at 4 x
// uint32 with no padding, which is exactly what the Android renderer reads out
// of its int[] - so a host can pass its own array straight to
// rvvm_tty_snapshot() without a conversion pass.
typedef struct {
    uint32_t cp;     // UCS-4 codepoint; 0 is a blank (or double-width gap) cell
    uint32_t fg;     // ARGB
    uint32_t bg;     // ARGB; the low 24 bits are 0 for the default background
    uint32_t flags;  // RVT_TTY_*, below
} rvvm_tty_cell_t;

// Cell flags, packed by rvvm_tty_snapshot() into rvvm_tty_cell_t.flags.
enum {
    RVT_TTY_BOLD      = 1 << 0,
    RVT_TTY_UNDERLINE = 1 << 1,
    RVT_TTY_REVERSE   = 1 << 2,  // already swapped into fg/bg
    RVT_TTY_WIDE      = 1 << 3,  // lead cell of a double-width glyph
    RVT_TTY_CURSOR    = 1 << 4,  // this is the cursor cell
};

// Where a snapshot's cells came from, and where the view sits in history.
typedef struct {
    int32_t rows, cols;              // the grid the cells belong to
    int32_t scroll;                  // lines the view sits above the live bottom
    int32_t scrollback_lines;        // lines stored in history
    int32_t cursor_row, cursor_col;  // -1/-1 when no cursor is drawn
    int32_t serial;                  // the serial this snapshot was taken at
} rvvm_tty_view_t;

rvvm_tty_t* rvvm_tty_open(int rows, int cols);  // NULL on allocation failure
void        rvvm_tty_close(rvvm_tty_t* tty);    // no guest may be attached
void        rvvm_tty_attach(rvvm_tty_t* tty, rvvm_machine_t* machine);
void        rvvm_tty_detach(rvvm_tty_t* tty, rvvm_machine_t* machine);
void        rvvm_tty_reset(rvvm_tty_t* tty);    // wipe the screen for a new run
void        rvvm_tty_resize(rvvm_tty_t* tty, int rows, int cols);
void        rvvm_tty_get_size(rvvm_tty_t* tty, int* rows, int* cols);  // caller holds the lock

// Serialize access to the session's VTerm. The guest thread parses its own
// fd 1/2 output into it, so a host that reads the screen matrix (a snapshot,
// say) must hold this lock for the duration of that access. Hold it briefly -
// it is a spinlock the guest thread contends for on every write - so do not
// block, allocate or render while holding it. rvvm_tty_resize() and
// rvvm_tty_reset() take it themselves; the VTerm calls made through
// rvvm_tty_vterm() require the caller to.
void        rvvm_tty_lock(rvvm_tty_t* tty);
void        rvvm_tty_unlock(rvvm_tty_t* tty);

// The libvterm `VTerm*` behind the session, for the rendering calls (flush the
// damage queue, read cells). Cast it after including <vterm.h>. Every use must
// be inside rvvm_tty_lock()/rvvm_tty_unlock().
void*       rvvm_tty_vterm(rvvm_tty_t* tty);

// Snapshot the view for a renderer: fills up to out_cells cells (rows*cols of
// the session's current grid, in view->rows/view->cols) plus the view they came
// from, in one locked pass - so the cells and the cursor can never disagree by
// a frame. Takes the lock itself; nothing needs to be held by the caller.
//
// The cells are already packed (colors resolved to ARGB, double-width gaps
// zeroed, cursor cell flagged): a renderer walks that array, it does not touch
// libvterm. A window scrolled into history is a copy out of the session's own
// scrollback, which is why the result is the same whether the line is on screen
// or has already scrolled off.
//
// Returns the number of cells written, or 0 when the session has no grid yet -
// or when out_cells is too small, in which case view->rows/view->cols are still
// filled so the caller can grow its buffer and call again.
int         rvvm_tty_snapshot(rvvm_tty_t* tty, rvvm_tty_cell_t* out, int out_cells, rvvm_tty_view_t* view);

// Drag the view through the scrollback, in whole rows: positive looks back into
// history. Clamped to what the session stored, and dragging back past the live
// bottom re-pins the view there. A view parked in history stays on the lines
// being read while output keeps arriving; typing brings it home.
void        rvvm_tty_scroll(rvvm_tty_t* tty, int lines);

// Repaint hint: bumped on output, resize, reset and scroll. Read without the
// lock (it is only ever compared), so a poller can call it at any rate.
int         rvvm_tty_serial(rvvm_tty_t* tty);

// Push host keyboard input toward the guest's virtual TTY - the input half of
// the console described above.
//
// `buf`/`len` is the byte sequence a real terminal receives from the keyboard:
// printable UTF-8, '\r' for Enter, 0x7F for Backspace, "\x1b[A"/"\x1b[B"/
// "\x1b[C"/"\x1b[D" for the arrow keys, 0x03/0x04 for Ctrl-C/Ctrl-D. The bytes
// run through the line discipline the guest's termios advertises:
//
//   - ICRNL      '\r' is translated to '\n'
//   - ISIG       Ctrl-C (0x03) discards the pending line and stops the guest.
//                The emulator cannot run a guest-installed SIGINT handler, so
//                the signal takes its default disposition: the run ends like
//                rvvm_user_stop() ends it, with status 128 + SIGINT
//   - ICANON     lines are assembled until Enter, Backspace erases and Ctrl-D
//                ends the line / reports EOF; when the guest clears ICANON the
//                bytes pass through raw, which is how a full-screen app gets
//                individual key presses
//   - ECHO/ECHOE typing is echoed into the VTerm, so the host's next snapshot
//                shows it (the guest cannot echo: the input never came from it)
//
// The cooked result is what the guest's read(0, ...) / readv(0, ...) returns:
// with a TTY attached, fd 0 is the console instead of the host process's own
// stdin. Safe to call from any thread; a no-op when no TTY is attached.
void rvvm_user_tty_input(rvvm_machine_t* machine, const void* buf, size_t len);

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

// --- Bundled assets (the /assets directory) ---
//
// The APK's assets/ tree is not reachable through the guest's file system, so a
// host that has one makes it visible as a mount: guest "/assets/foo/bar.png" is
// a read-only path, and the host supplies how to open a name from it here. The
// guest then uses plain open()/read().
//
// The mount is matched on the guest's own absolute path and is independent of
// rvvm_user_set_prefix(): a host that disables the prefix (Android) still gets
// the mount, and a host that uses one (win32) keeps both.
//
// The host owns everything behind the fd it returns - a plain seekable file, a
// pipe fed from a decompressor, ... The core only routes the path and refuses the
// mutating open flags with EROFS. lseek()/fstat() then report whatever POSIX says
// about that kind of fd (ESPIPE for a pipe), which is how a guest can tell them
// apart when it has to.
typedef struct {
    // Open @name - a path relative to the mount, e.g. "fonts/x.ttf" - for reading
    // and return a host fd the guest will read() from, or a negative guest errno
    // in Linux UAPI numbering (-ENOENT for an unknown name). Called on the
    // guest's vCPU thread, from its openat() path, so it should return promptly.
    //
    // The fd may be a *stream*: a pipe the host fills on demand is what this is
    // for - an asset need not be resident anywhere on the host - and the guest is
    // free to read it a chunk at a time. Non-seekable is then the honest answer
    // (lseek() reports ESPIPE, exactly like a real pipe), and a guest that needs
    // random access has to be handed a seekable source instead.
    //
    // The core reaps the fd when the run ends. A guest that exits without closing
    // its descriptors is the ordinary case and the emulator runs no process
    // teardown, so a host may hang background work off the fd - a thread feeding
    // that pipe - and rely on the run end unblocking it rather than stranding it.
    int (*open_fd)(void* userdata, const char* name);

    // The length of @name in bytes, or a negative guest errno. Answers stat() and
    // access() without the host having to materialize anything: a guest that
    // stats before it opens would otherwise pay for a full open_fd() just to
    // learn a size.
    int64_t (*size)(void* userdata, const char* name);

    // Directory enumeration, for opendir()/readdir() inside the mount. The tree
    // is not a directory anywhere on the host, so there is nothing to open() -
    // the host hands back an opaque handle it can walk instead, one entry name at
    // a time. Enumerate immediate children only: the core does not recurse, and a
    // name that is itself a directory shows up as an entry like any other.
    //
    //   open_dir  - handle for directory @name ("" is the mount root), or NULL
    //               when there is no such directory.
    //   dir_next  - the next entry name, or NULL at the end. The pointer only has
    //               to stay valid until the next call / dir_close. "." and ".."
    //               need not be reported - the core supplies those - and one that
    //               is reported anyway is skipped.
    //   dir_close - release the handle.
    //
    // A host without these simply has no enumerable directories: stat() of a
    // directory then fails, and opendir() answers ENOENT.
    void*       (*open_dir)(void* userdata, const char* name);
    const char* (*dir_next)(void* userdata, void* dir);
    void        (*dir_close)(void* userdata, void* dir);
} rvvm_asset_ops_t;

// Mount the host's asset tree. Without this every open under the mount answers
// ENOENT, which is also what a host with no asset tree should see. @ops must
// outlive the run; call this before rvvm_user_linux_ex().
void rvvm_user_set_assets(rvvm_machine_t* machine, const rvvm_asset_ops_t* ops, void* userdata);

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

// True once every guest vCPU has actually reached the park point in response
// to rvvm_user_suspend(). Unlike rvvm_user_is_suspended() (which reports the
// request), this reports the quiesced state: a vCPU blocked in a host syscall
// (read(), AAudio, EGL) has not parked yet, so this returns false until that
// syscall returns and the vCPU reaches the next interpreter boundary. Host
// code that tears down a resource the guest may still be touching (a window,
// an audio device, ...) should gate on this, not on the request.
bool rvvm_user_is_parked(rvvm_machine_t* machine);

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
