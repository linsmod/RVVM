/*
 * vp_syscall.h - VirtPass private syscall numbers (shared ABI contract)
 *
 * Single source of truth for the 0x10000+ window that proxies Android NDK
 * APIs into the host. Both sides of the boundary include this header:
 *
 *   guest : src/virtpass/vp_ndk_stub.c (through virtpass/vp_android.h)
 *   host  : src/virtpass/vp_cmdpost.c / vp_cmdpost.h, src/core/rvvm_user.c
 *
 * Keep it dependency-free - plain #define only. It is compiled into the
 * RISC-V guest as well as every host backend, and must never pull in
 * platform headers.
 *
 * The AAudio numbers (BASE + 40..51) intentionally stay in
 * virtpass/vp_audio_ringbuf.h: they are published together with the shared
 * PCM ring layout they describe. The sensor numbers (BASE + 60..69) follow
 * the same rule and live in virtpass/vp_sensor_abi.h - which, unlike the old
 * sensor ring buffer header, is a pure wire protocol: the event data path is
 * a copy-out into the caller's ASensorEvent array.
 */
#ifndef VIRTPASS_SYSCALL_H
#define VIRTPASS_SYSCALL_H

/* The proxy range lives in a private window that cannot collide with the
 * Linux RISC-V syscall numbers (which top out around 439). */
#define SYS_ANDROID_BASE          0x10000
#define SYS_ANDROID_CALL          0x10022

/* Sub-commands passed in a0 for SYS_ANDROID_CALL.
 *
 * BASE + 1..4 are retired: the sensor proxy used to live there and now owns
 * the BASE + 60..69 block in virtpass/vp_sensor_abi.h, published together
 * with the event/descriptor wire structs it carries. */
#define SYS_ANDROID_WINDOW_INIT   (SYS_ANDROID_BASE + 5)
#define SYS_ANDROID_INPUT_INIT    (SYS_ANDROID_BASE + 6)
#define SYS_ANDROID_LIFECYCLE     (SYS_ANDROID_BASE + 7)
#define SYS_ANDROID_CONFIG        (SYS_ANDROID_BASE + 8)
#define SYS_ANDROID_LOOPER_INIT   (SYS_ANDROID_BASE + 9)
#define SYS_ANDROID_ASSET_OPEN    (SYS_ANDROID_BASE + 10)

/* Window lock/unlock (Phase 1: software rendering). */
#define SYS_ANDROID_WINDOW_LOCK      (SYS_ANDROID_BASE + 11)
#define SYS_ANDROID_WINDOW_UNLOCK    (SYS_ANDROID_BASE + 12)
#define SYS_ANDROID_WINDOW_GET_SIZE  (SYS_ANDROID_BASE + 13)
#define SYS_ANDROID_WINDOW_SET_BUF   (SYS_ANDROID_BASE + 14)

/* GameActivity (Phase 2: lifecycle + input). */
#define SYS_ANDROID_GAME_CREATE      (SYS_ANDROID_BASE + 20)
#define SYS_ANDROID_GAME_DESTROY     (SYS_ANDROID_BASE + 21)
#define SYS_ANDROID_GAME_POLL_CMD    (SYS_ANDROID_BASE + 22)
#define SYS_ANDROID_GAME_SWAP_INPUT  (SYS_ANDROID_BASE + 23)
#define SYS_ANDROID_GAME_CLEAR_INPUT (SYS_ANDROID_BASE + 24)

/* Answer for SYS_ANDROID_GAME_POLL_CMD when no lifecycle command is pending.
 * Deliberately non-negative: rvvm-user warns on every negative syscall result
 * and the guest polls this once per frame, so answering -1 would turn a normal
 * "nothing to do" into a frame-rate log flood. The value also sits far outside
 * the APP_CMD_* range, so it can never be mistaken for a real command;
 * android_app_read_cmd() maps it back to the -1 the NDK contract exposes. */
#define VP_GAME_CMD_NONE             0x7F22

/* Choreographer (Phase 4: display vsync source). */
#define SYS_ANDROID_CHOREOGRAPHER_INIT  (SYS_ANDROID_BASE + 25)
#define SYS_ANDROID_CHOREOGRAPHER_WAIT  (SYS_ANDROID_BASE + 26)
/* fd wakeup (方案 B): the guest hands the host the write end of the pipe its
 * Looper polls, and asks for exactly one vsync at a time. */
#define SYS_ANDROID_CHOREOGRAPHER_SET_FD        (SYS_ANDROID_BASE + 27)
#define SYS_ANDROID_CHOREOGRAPHER_REQUEST_VSYNC (SYS_ANDROID_BASE + 28)

/* Marshalled GL/EGL calls (Phase 3 hardware GL proxy).
 *
 * The guest passes a `gl_call*` in a0 (see virtpass/vp_gl.h for the struct
 * and the fn_id space) and nothing else; the host dispatches on the syscall
 * number and forwards a0 to cmdpost_dispatch().
 *
 * These are part of the shared ABI rather than living in the generated
 * vp_gl.h: rvvm_user.c has to name them in its syscall switch, and the
 * header generator must not be the only place they exist. */
#define SYS_GL_CALL_BASE  0x10020
#define SYS_GL_CALL       (SYS_GL_CALL_BASE + 0)
#define SYS_EGL_CALL      (SYS_GL_CALL_BASE + 1)

#endif /* VIRTPASS_SYSCALL_H */
