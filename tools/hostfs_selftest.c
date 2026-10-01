/* rvvm_hostfs's own checks, run against a real host directory.

   The memory filesystem is a data structure and can be tested as one. This one
   cannot: every answer it gives is the host's answer, and what it can get wrong is
   the part it adds on top - which path it built, whether it refused one that
   leaves the mount, whether it reported honestly that this host cannot hold a
   symlink. Those are exactly the things a guest-level test cannot see, because
   from a guest they look like ordinary ENOENTs on a host directory.

   So this runs the module against a scratch directory and asserts on its answers
   directly. It asks for the directory as argv[1] so the harness owns making and
   removing it - a test that picks its own idea of a temp directory is a test that
   leaves one behind on the wrong platform.

   Two things it is deliberate about:

     - what the host cannot do is asserted, not skipped. A host that cannot make a
       symlink must SAY SO: can_symlink() false and symlink() answering ENOTSUP.
       A silent false would have the module pretending to have made one.
     - the mode bits are only asserted where the host keeps them. Windows keeps a
       read-only attribute and synthesizes the rest, so "chmod 0600 then stat
       reports 0600" is a claim about the host and not about this module; it is
       checked only where it is true.

   Not part of the build. */

#include "core/rvvm_hostfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir((path), 0755)
#endif

static int fails = 0;
static int checks = 0;

static void stage(const char* what)
{
    fprintf(stderr, "== %s\n", what);
    fflush(stderr);
}

static void ck(int ok, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static void ck_rc(rvvm_hostfs_result_t got, rvvm_hostfs_result_t want, const char* what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    }
}

/* Everything below rejects a failure to mutate; asserting the exact errno would be
 * asserting the host's spelling of it (EISDIR on Linux, EACCES on Windows). */
static void ck_fail(rvvm_hostfs_result_t got, const char* what)
{
    checks++;
    if (got == RVVM_HOSTFS_OK) {
        fails++;
        printf("FAIL %s: it succeeded\n", what);
    }
}

int main(int argc, char** argv)
{
    rvvm_hostfs_t* fs;
    rvvm_hostfs_info_t info;
    char buf[64];
    char name[256];
    uint8_t kind;
    uint64_t ino;
    uint32_t pos;
    uint32_t hnd;
    size_t done;
    int     saw_link;

    if (argc < 2) {
        printf("usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    MKDIR(argv[1]);

    stage("lifetime");
    /* A root that is not there is not made: whether a backing directory should
     * exist is the caller's question, and inventing one is how a mount of an
     * existing tree silently becomes a mount of an empty one. */
    ck(rvvm_hostfs_create(argv[1], false) != NULL, "create over an existing directory");
    ck(rvvm_hostfs_create("/no/such/directory/at/all", false) == NULL,
       "create over a missing directory is refused");
    ck(rvvm_hostfs_create(NULL, false) == NULL, "create with no root is refused");

    fs = rvvm_hostfs_create(argv[1], false);
    ck(fs != NULL, "create");
    if (!fs) {
        return 1;
    }
    ck(rvvm_hostfs_root(fs) && !strcmp(rvvm_hostfs_root(fs), argv[1]), "root is the directory");

    stage("root");
    ck_rc(rvvm_hostfs_stat(fs, "/", true, &info), RVVM_HOSTFS_OK, "stat root");
    ck(info.kind == RVVM_HOSTFS_DIR, "root is a directory");
    ck(!strcmp(info.path, "/"), "root is named /");
    ck_rc(rvvm_hostfs_stat(fs, "/nope", true, &info), RVVM_HOSTFS_ENOENT, "stat missing is ENOENT");

    stage("normalize");
    ck_rc(rvvm_hostfs_mkdir(fs, "/a", 0755), RVVM_HOSTFS_OK, "mkdir /a");
    ck_rc(rvvm_hostfs_stat(fs, "//a", true, &info), RVVM_HOSTFS_OK, "//a == /a");
    ck_rc(rvvm_hostfs_stat(fs, "/a/", true, &info), RVVM_HOSTFS_OK, "/a/ == /a");
    ck_rc(rvvm_hostfs_stat(fs, "/./a", true, &info), RVVM_HOSTFS_OK, "/./a == /a");
    ck_rc(rvvm_hostfs_stat(fs, "/x/../a", true, &info), RVVM_HOSTFS_OK, "/x/../a == /a (lexical)");
    ck_rc(rvvm_hostfs_stat(fs, "/../a", true, &info), RVVM_HOSTFS_EINVAL, "/../a leaves the mount");
    ck_rc(rvvm_hostfs_stat(fs, "/a/../..", true, &info), RVVM_HOSTFS_EINVAL, "above the root");
    ck_rc(rvvm_hostfs_stat(fs, "a", true, &info), RVVM_HOSTFS_EINVAL, "a relative path is refused");
    /* Escape attempts have to be refused on the mutating side too, not just by not
     * finding anything: mkdir that reached the host's parent would create there. */
    ck_rc(rvvm_hostfs_mkdir(fs, "/../escaped", 0755), RVVM_HOSTFS_EINVAL, "mkdir outside is refused");
    ck_rc(rvvm_hostfs_stat(fs, "/../escaped", true, &info), RVVM_HOSTFS_EINVAL,
          "and nothing was made outside");

    stage("mkdir");
    /* Not recursive, exactly as on Linux: a filesystem that invented the parent
     * would be answering for a directory the caller did not name. */
    ck_rc(rvvm_hostfs_mkdir(fs, "/no/such/parent", 0755), RVVM_HOSTFS_ENOENT,
          "mkdir under a missing parent is ENOENT, not a silent -p");
    ck_rc(rvvm_hostfs_mkdir(fs, "/a", 0755), RVVM_HOSTFS_EEXIST, "mkdir existing is EEXIST");
    ck_rc(rvvm_hostfs_isdir(fs, "/a"), RVVM_HOSTFS_OK, "isdir /a");
    ck_rc(rvvm_hostfs_isdir(fs, "/nope"), RVVM_HOSTFS_ENOENT, "isdir missing is ENOENT");

    stage("write and read");
    ck_rc(rvvm_hostfs_create_file(fs, "/a/f", 0644), RVVM_HOSTFS_OK, "create /a/f");
    ck_rc(rvvm_hostfs_stat(fs, "/a/f", true, &info), RVVM_HOSTFS_OK, "stat /a/f");
    ck(info.kind == RVVM_HOSTFS_REG, "it is a regular file");
    ck(info.size == 0, "and empty");
    ck_rc(rvvm_hostfs_write(fs, "/a/f", 0, "hello", 5, &done), RVVM_HOSTFS_OK, "write 5 bytes");
    ck(done == 5, "all 5 landed");
    ck_rc(rvvm_hostfs_stat(fs, "/a/f", true, &info), RVVM_HOSTFS_OK, "stat after write");
    ck(info.size == 5, "size is 5");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_hostfs_read(fs, "/a/f", 0, buf, 5, &done), RVVM_HOSTFS_OK, "read it back");
    ck(done == 5 && !memcmp(buf, "hello", 5), "and it says hello");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_hostfs_read(fs, "/a/f", 3, buf, 5, &done), RVVM_HOSTFS_OK, "read past the end");
    ck(done == 2, "a read past the end is short, not an error");
    /* A write does not create: create and write are two operations. */
    ck_rc(rvvm_hostfs_write(fs, "/a/nothing", 0, "x", 1, &done), RVVM_HOSTFS_ENOENT,
          "write to a name that is not there is ENOENT");
    ck_rc(rvvm_hostfs_read(fs, "/a/nothing", 0, buf, 1, &done), RVVM_HOSTFS_ENOENT,
          "read of a name that is not there is ENOENT");

    stage("truncate");
    ck_rc(rvvm_hostfs_truncate(fs, "/a/f", 16), RVVM_HOSTFS_OK, "grow to 16");
    ck_rc(rvvm_hostfs_stat(fs, "/a/f", true, &info), RVVM_HOSTFS_OK, "stat after grow");
    ck(info.size == 16, "size is 16");
    memset(buf, 'x', sizeof(buf));
    ck_rc(rvvm_hostfs_read(fs, "/a/f", 5, buf, 4, &done), RVVM_HOSTFS_OK, "read the grown part");
    ck(done == 4 && buf[0] == 0 && buf[3] == 0, "growing zero-fills");
    ck_rc(rvvm_hostfs_truncate(fs, "/a/f", 2), RVVM_HOSTFS_OK, "shrink to 2");
    ck_rc(rvvm_hostfs_stat(fs, "/a/f", true, &info), RVVM_HOSTFS_OK, "stat after shrink");
    ck(info.size == 2, "size is 2");
    /* Refused, but the host picks the word: EISDIR where a directory cannot be
     * opened for writing, EACCES where it cannot be opened at all. */
    ck_fail(rvvm_hostfs_truncate(fs, "/a", 0), "truncate a directory fails");

    stage("rename");
    ck_rc(rvvm_hostfs_rename(fs, "/a/f", "/a/g"), RVVM_HOSTFS_OK, "rename a file");
    ck_rc(rvvm_hostfs_stat(fs, "/a/f", true, &info), RVVM_HOSTFS_ENOENT, "the old name is gone");
    ck_rc(rvvm_hostfs_stat(fs, "/a/g", true, &info), RVVM_HOSTFS_OK, "the new name is there");
    ck_rc(rvvm_hostfs_rename(fs, "/a", "/b"), RVVM_HOSTFS_OK, "rename a directory");
    ck_rc(rvvm_hostfs_stat(fs, "/b/g", true, &info), RVVM_HOSTFS_OK, "its contents came with it");
    ck_rc(rvvm_hostfs_rename(fs, "/nope", "/c"), RVVM_HOSTFS_ENOENT, "rename a missing name");

    stage("remove");
    ck_rc(rvvm_hostfs_create_file(fs, "/b/h", 0644), RVVM_HOSTFS_OK, "make /b/h");
    ck_fail(rvvm_hostfs_unlink(fs, "/b", false), "unlink a directory as a file fails");
    ck_fail(rvvm_hostfs_unlink(fs, "/b", true), "rmdir a non-empty directory fails");
    ck_rc(rvvm_hostfs_unlink(fs, "/", true), RVVM_HOSTFS_EINVAL, "the root cannot be removed");
    ck_rc(rvvm_hostfs_unlink(fs, "/b/h", false), RVVM_HOSTFS_OK, "unlink the file");
    ck_rc(rvvm_hostfs_unlink(fs, "/b/g", false), RVVM_HOSTFS_OK, "unlink the other file");
    ck_rc(rvvm_hostfs_unlink(fs, "/b", true), RVVM_HOSTFS_OK, "rmdir the now-empty directory");
    ck_rc(rvvm_hostfs_stat(fs, "/b", true, &info), RVVM_HOSTFS_ENOENT, "and it is gone");

    stage("listing");
    ck_rc(rvvm_hostfs_mkdir(fs, "/d", 0755), RVVM_HOSTFS_OK, "mkdir /d");
    ck_rc(rvvm_hostfs_create_file(fs, "/d/one", 0644), RVVM_HOSTFS_OK, "make /d/one");
    ck_rc(rvvm_hostfs_create_file(fs, "/d/two", 0644), RVVM_HOSTFS_OK, "make /d/two");
    ck_rc(rvvm_hostfs_create_file(fs, "/d/three", 0644), RVVM_HOSTFS_OK, "make /d/three");
    pos = 0;
    ck_rc(rvvm_hostfs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_OK, "first entry");
    ck(strcmp(name, ".") && strcmp(name, ".."), "the first entry is not . or ..");
    pos = 1;
    ck_rc(rvvm_hostfs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_OK, "second entry");
    pos = 2;
    ck_rc(rvvm_hostfs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_OK, "third entry");
    pos = 3;
    ck_rc(rvvm_hostfs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_EOF, "and the walk ends");
    /* The cursor is a count of entries handed out, so asking for one past the end
     * again must not wrap or invent. */
    pos = 99;
    ck_rc(rvvm_hostfs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_EOF, "a cursor past the end is still the end");
    ck_rc(rvvm_hostfs_getdents(fs, "/nope", &pos, name, sizeof(name), &kind, &ino),
          RVVM_HOSTFS_ENOENT, "listing a name that is not there is ENOENT");

    stage("symlink");
    /* What this host can do is asserted, not skipped: if it cannot hold a link,
     * the module has to say so rather than return a bare failure. */
    saw_link = rvvm_hostfs_can_symlink(fs);
    fprintf(stderr, "   (this host can%s hold a symlink)\n", saw_link ? "" : "not");
    if (saw_link) {
        ck_rc(rvvm_hostfs_symlink(fs, "one", "/d/link"), RVVM_HOSTFS_OK, "symlink");
        ck_rc(rvvm_hostfs_stat(fs, "/d/link", false, &info), RVVM_HOSTFS_OK, "lstat the link");
        ck(info.kind == RVVM_HOSTFS_LNK, "lstat says it is a link");
        ck_rc(rvvm_hostfs_stat(fs, "/d/link", true, &info), RVVM_HOSTFS_OK, "stat follows it");
        ck(info.kind == RVVM_HOSTFS_REG, "stat says it is the file it points at");
        memset(buf, 0, sizeof(buf));
        ck_rc(rvvm_hostfs_readlink(fs, "/d/link", buf, sizeof(buf)), RVVM_HOSTFS_OK, "readlink");
        ck(!strcmp(buf, "one"), "and it says what it points at");
        ck_rc(rvvm_hostfs_readlink(fs, "/d/one", buf, sizeof(buf)), RVVM_HOSTFS_EINVAL,
              "readlink of something that is not a link is EINVAL");
        ck_rc(rvvm_hostfs_unlink(fs, "/d/link", false), RVVM_HOSTFS_OK, "unlink the link");
    } else {
        ck_rc(rvvm_hostfs_symlink(fs, "one", "/d/link"), RVVM_HOSTFS_ENOTSUP,
              "symlink says this host cannot hold one");
        ck_rc(rvvm_hostfs_stat(fs, "/d/link", false, &info), RVVM_HOSTFS_ENOENT,
              "and nothing was made");
    }
    ck_rc(rvvm_hostfs_readlink(fs, "/d/nope", buf, sizeof(buf)), RVVM_HOSTFS_ENOENT,
          "readlink of a name that is not there is ENOENT");

    stage("hard link");
    if (rvvm_hostfs_can_link(fs)) {
        ck_rc(rvvm_hostfs_link(fs, "/d/one", "/d/hard"), RVVM_HOSTFS_OK, "link");
        ck_rc(rvvm_hostfs_stat(fs, "/d/one", true, &info), RVVM_HOSTFS_OK, "stat one name");
#if !defined(_WIN32)
        /* Only where the host reports it: Windows' stat has no link count, so
         * nlink there is a number this module was given and not a claim it made. */
        ck(info.nlink >= 2, "nlink counts both names");
#endif
        ck_rc(rvvm_hostfs_write(fs, "/d/hard", 0, "xy", 2, &done), RVVM_HOSTFS_OK,
              "write through the second name");
        memset(buf, 0, sizeof(buf));
        ck_rc(rvvm_hostfs_read(fs, "/d/one", 0, buf, 2, &done), RVVM_HOSTFS_OK,
              "read through the first");
        ck(!memcmp(buf, "xy", 2), "a hard link is not a copy");
        ck_rc(rvvm_hostfs_unlink(fs, "/d/hard", false), RVVM_HOSTFS_OK, "unlink one name");
        ck_rc(rvvm_hostfs_stat(fs, "/d/one", true, &info), RVVM_HOSTFS_OK, "the other survives");
    } else {
        ck_rc(rvvm_hostfs_link(fs, "/d/one", "/d/hard"), RVVM_HOSTFS_ENOTSUP,
              "link says this host cannot make one");
    }

    stage("touch");
    /* The negative answer is the one that matters: touch(1) asks utimensat() first
     * and only creates when it hears ENOENT, so a touch that answered success for
     * a missing name left touch reporting success with no file behind it. */
    ck_rc(rvvm_hostfs_touch(fs, "/d/no-such-file"), RVVM_HOSTFS_ENOENT,
          "touch of a name that is not there is ENOENT");
    ck_rc(rvvm_hostfs_touch(fs, "/d/one"), RVVM_HOSTFS_OK, "touch of one that is");
#if !defined(_WIN32)
    /* Where the host allows it. On Windows utime() on a directory is refused
     * outright, which is a fact about the host and not about this module. */
    ck_rc(rvvm_hostfs_touch(fs, "/d"), RVVM_HOSTFS_OK, "touch of a directory");
#endif

    stage("chmod");
    ck_rc(rvvm_hostfs_chmod(fs, "/d/one", 0755), RVVM_HOSTFS_OK, "chmod");
    ck_rc(rvvm_hostfs_stat(fs, "/d/one", true, &info), RVVM_HOSTFS_OK, "stat after chmod");
#if !defined(_WIN32)
    /* Only where the host keeps modes. Windows keeps a read-only attribute and
     * synthesizes the rest, so this is a claim about the host and not about this
     * module - asserting it there would be asserting the shim. */
    ck((info.mode & 0777) == 0755, "the mode is what was set");
#endif
    ck_rc(rvvm_hostfs_chmod(fs, "/d/nope", 0755), RVVM_HOSTFS_ENOENT, "chmod a missing name");

    stage("descriptors");
    ck_rc(rvvm_hostfs_write(fs, "/d/one", 0, "abcdef", 6, &done), RVVM_HOSTFS_OK, "write 6 bytes");
    ck_rc(rvvm_hostfs_pin(fs, "/d/one", false, &hnd), RVVM_HOSTFS_OK, "pin it");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_hostfs_read_at(fs, hnd, 2, buf, 3, &done), RVVM_HOSTFS_OK, "read through the handle");
    ck(done == 3 && !memcmp(buf, "cde", 3), "at the offset asked for");
    ck_rc(rvvm_hostfs_stat_ino(fs, hnd, &info), RVVM_HOSTFS_OK, "stat through the handle");
    ck(info.kind == RVVM_HOSTFS_REG, "it is the same file");
    ck(info.size == 6, "with the same size");
    /* A handle opened readable cannot write: the host's descriptor says so, and
     * reporting it is better than surprising the host with EBADF. */
    ck_fail(rvvm_hostfs_write_at(fs, hnd, 0, "z", 1, &done), "a readable handle cannot write");
    rvvm_hostfs_unpin(fs, hnd);
    ck_fail(rvvm_hostfs_read_at(fs, hnd, 0, buf, 1, &done), "a closed handle reads nothing");
    ck_rc(rvvm_hostfs_pin(fs, "/d/one", true, &hnd), RVVM_HOSTFS_OK, "pin it writable");
    ck_rc(rvvm_hostfs_write_at(fs, hnd, 0, "XY", 2, &done), RVVM_HOSTFS_OK,
          "write through the handle");
    ck_rc(rvvm_hostfs_truncate_ino(fs, hnd, 3), RVVM_HOSTFS_OK, "truncate through the handle");
    ck_rc(rvvm_hostfs_stat_ino(fs, hnd, &info), RVVM_HOSTFS_OK, "stat through the handle");
    ck(info.size == 3, "size is 3");
#if !defined(_WIN32)
    /* A handle is a descriptor, so a rename between two calls must not change what
     * it answers - on a host that lets a name move while the file is open. On
     * Windows an open file cannot be renamed at all, which is the difference the
     * module's header comment names; renaming there is EPERM, not a lost file. */
    ck_rc(rvvm_hostfs_rename(fs, "/d/one", "/d/moved"), RVVM_HOSTFS_OK, "rename it underneath");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_hostfs_read_at(fs, hnd, 0, buf, 6, &done), RVVM_HOSTFS_OK,
          "the handle still reads it");
    ck(!memcmp(buf, "XYcdef", 6), "and reads the same bytes");
#else
    ck_fail(rvvm_hostfs_rename(fs, "/d/one", "/d/moved"),
            "renaming an open file is refused on this host");
    (void)done;
#endif
    rvvm_hostfs_unpin(fs, hnd);

    stage("read-only");
    ck_rc(rvvm_hostfs_mkdir(fs, "/ro", 0755), RVVM_HOSTFS_OK, "mkdir /ro");
    ck(!rvvm_hostfs_read_only(fs), "the mount is writable");
    rvvm_hostfs_set_read_only(fs, true);
    ck(rvvm_hostfs_read_only(fs), "and now it is not");
    ck_rc(rvvm_hostfs_create_file(fs, "/ro/f", 0644), RVVM_HOSTFS_EROFS, "create is EROFS");
    ck_rc(rvvm_hostfs_mkdir(fs, "/ro/sub", 0755), RVVM_HOSTFS_EROFS, "mkdir is EROFS");
    ck_rc(rvvm_hostfs_unlink(fs, "/d/two", false), RVVM_HOSTFS_EROFS, "unlink is EROFS");
    ck_rc(rvvm_hostfs_rename(fs, "/d/two", "/d/x"), RVVM_HOSTFS_EROFS, "rename is EROFS");
    ck_rc(rvvm_hostfs_write(fs, "/d/two", 0, "x", 1, &done), RVVM_HOSTFS_EROFS, "write is EROFS");
    ck_rc(rvvm_hostfs_touch(fs, "/d/two"), RVVM_HOSTFS_EROFS, "touch is EROFS");
    ck_rc(rvvm_hostfs_symlink(fs, "x", "/d/l2"), RVVM_HOSTFS_EROFS, "symlink is EROFS");
    /* Reading still works: read-only is not "gone". */
    ck_rc(rvvm_hostfs_stat(fs, "/d/two", true, &info), RVVM_HOSTFS_OK, "stat still works");
    ck_rc(rvvm_hostfs_read(fs, "/d/one", 0, buf, 3, &done), RVVM_HOSTFS_OK, "read still works");
    /* And the capability probe does not claim an answer it cannot act on. */
    ck(!rvvm_hostfs_can_symlink(fs), "a read-only mount does not claim symlinks");
    rvvm_hostfs_set_read_only(fs, false);
    ck_rc(rvvm_hostfs_create_file(fs, "/ro/f", 0644), RVVM_HOSTFS_OK, "and writable again");

    stage("references");
    ck(rvvm_hostfs_ref(fs) == fs, "ref");
    rvvm_hostfs_free(fs);
    ck_rc(rvvm_hostfs_stat(fs, "/d", true, &info), RVVM_HOSTFS_OK, "still there after one free");
    rvvm_hostfs_free(fs);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
