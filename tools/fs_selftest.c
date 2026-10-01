/* rvvm_fs's own checks: the same operations, asked of two filesystems that are
   nothing alike.

   The point of the table is that a caller does not know which provider it has. So
   this runs one set of assertions twice - once against a memory filesystem and
   once against a real host directory - through rvvm_fs_* and not through either
   provider's own entry points. Anything that only works for one of them is a slot
   the table got wrong: a shape claimed to be common that is not.

   It is not a retelling of the providers' own tests. Those ask whether memfs and
   hostfs are correct; this asks whether they are INTERCHANGEABLE, which is a
   different claim and the one the syscall layer is going to depend on.

   Not part of the build. */

#include "core/rvvm_fs.h"

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

static void ck(int ok, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static void ck_rc(rvvm_fs_result_t got, rvvm_fs_result_t want, const char* what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    }
}

/* One filesystem, asked the same questions in the same order. @label is only for
 * the failure lines; nothing here may depend on which provider it is. */
static void exercise(rvvm_fs_t* fs, const char* label)
{
    rvvm_fs_info_t info;
    char           buf[64];
    char           name[256];
    uint8_t        kind;
    uint64_t       ino;
    uint32_t       hnd;
    uint32_t       pos;
    size_t         done;
    char           what[256];

    ck(fs->ops != NULL && fs->ops->name != NULL, "the table is there");

    ck_rc(rvvm_fs_stat(fs, "/", true, &info), RVVM_FS_OK, "stat root");
    ck(info.kind == RVVM_FS_DIR, "the root is a directory");
    ck_rc(rvvm_fs_stat(fs, "/nope", true, &info), RVVM_FS_ENOENT, "a missing name is ENOENT");

    ck_rc(rvvm_fs_mkdir(fs, "/d", 0755), RVVM_FS_OK, "mkdir");
    ck_rc(rvvm_fs_isdir(fs, "/d"), RVVM_FS_OK, "isdir");
    ck_rc(rvvm_fs_create_file(fs, "/d/f", 0644), RVVM_FS_OK, "create");
    ck_rc(rvvm_fs_write(fs, "/d/f", 0, "hello", 5, &done), RVVM_FS_OK, "write");
    ck(done == 5, "all of it landed");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_fs_read(fs, "/d/f", 0, buf, 5, &done), RVVM_FS_OK, "read");
    ck(done == 5 && !memcmp(buf, "hello", 5), "and it says hello");
    ck_rc(rvvm_fs_stat(fs, "/d/f", true, &info), RVVM_FS_OK, "stat the file");
    ck(info.kind == RVVM_FS_REG && info.size == 5, "it is a 5-byte regular file");

    /* A link, which the two provide in completely different ways - one in its own
     * storage, one as a file on a host that cannot hold a real one - and which has
     * to look the same from here. */
    ck_rc(rvvm_fs_symlink(fs, "f", "/d/l"), RVVM_FS_OK, "symlink");
    ck_rc(rvvm_fs_stat(fs, "/d/l", false, &info), RVVM_FS_OK, "lstat the link");
    ck(info.kind == RVVM_FS_LNK, "it is a link");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_fs_readlink(fs, "/d/l", buf, sizeof(buf)), RVVM_FS_OK, "readlink");
    ck(!strcmp(buf, "f"), "and it names the target");
    ck_rc(rvvm_fs_stat(fs, "/d/l", true, &info), RVVM_FS_OK, "stat follows it");
    ck(info.kind == RVVM_FS_REG, "to the file");

    /* A descriptor names the file, not the name. */
    ck_rc(rvvm_fs_pin(fs, "/d/f", false, &hnd), RVVM_FS_OK, "pin");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_fs_read_at(fs, hnd, 1, buf, 3, &done), RVVM_FS_OK, "read through it");
    ck(done == 3 && !memcmp(buf, "ell", 3), "at the offset asked for");
    ck_rc(rvvm_fs_stat_ino(fs, hnd, &info), RVVM_FS_OK, "stat through it");
    ck(info.kind == RVVM_FS_REG && info.size == 5, "still the same file");
    rvvm_fs_unpin(fs, hnd);

    /* Listing. */
    pos = 0;
    kind = 0;
    {
        int found = 0;
        while (rvvm_fs_getdents(fs, "/d", &pos, name, sizeof(name), &kind, &ino) == RVVM_FS_OK) {
            if (!strcmp(name, "l")) {
                found = (kind == RVVM_FS_LNK);
                break;
            }
        }
        ck(found, "the listing shows the link as a link");
    }

    ck_rc(rvvm_fs_truncate(fs, "/d/f", 2), RVVM_FS_OK, "truncate");
    ck_rc(rvvm_fs_stat(fs, "/d/f", true, &info), RVVM_FS_OK, "stat after truncate");
    ck(info.size == 2, "it is 2 bytes now");
    ck_rc(rvvm_fs_touch(fs, "/d/f"), RVVM_FS_OK, "touch");
    ck_rc(rvvm_fs_chmod(fs, "/d/f", 0600), RVVM_FS_OK, "chmod");
    ck_rc(rvvm_fs_touch(fs, "/d/nothing"), RVVM_FS_ENOENT, "touch of a missing name");

    ck_rc(rvvm_fs_rename(fs, "/d/f", "/d/g"), RVVM_FS_OK, "rename");
    ck_rc(rvvm_fs_stat(fs, "/d/f", true, &info), RVVM_FS_ENOENT, "the old name is gone");
    ck_rc(rvvm_fs_unlink(fs, "/d/l", false), RVVM_FS_OK, "unlink the link");
    ck_rc(rvvm_fs_stat(fs, "/d/g", true, &info), RVVM_FS_OK, "the renamed file is there");
    ck_rc(rvvm_fs_unlink(fs, "/d/g", false), RVVM_FS_OK, "unlink it");
    ck_rc(rvvm_fs_unlink(fs, "/d", true), RVVM_FS_OK, "rmdir");

    /* Read-only is a flag on the filesystem, asked through the same table. */
    ck(!rvvm_fs_read_only(fs), "the mount is writable");
    rvvm_fs_set_read_only(fs, true);
    ck(rvvm_fs_read_only(fs), "and now it is not");
    ck_rc(rvvm_fs_create_file(fs, "/d2", 0644), RVVM_FS_EROFS, "create is EROFS");
    ck_rc(rvvm_fs_mkdir(fs, "/d2", 0755), RVVM_FS_EROFS, "mkdir is EROFS");
    rvvm_fs_set_read_only(fs, false);
    ck_rc(rvvm_fs_mkdir(fs, "/d2", 0755), RVVM_FS_OK, "and writable again");
    ck_rc(rvvm_fs_unlink(fs, "/d2", true), RVVM_FS_OK, "clean up");

    snprintf(what, sizeof(what), "[%s] %s", label, "done");
    (void)what;
}

int main(int argc, char** argv)
{
    rvvm_memfs_t*  mem;
    rvvm_hostfs_t* host;
    rvvm_fs_t      view;

    if (argc < 2) {
        printf("usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    MKDIR(argv[1]);

    mem = rvvm_memfs_create(false, 0);
    ck(mem != NULL, "a memory filesystem");
    if (!mem) {
        return 1;
    }
    view = rvvm_fs_view_memfs(mem);
    fprintf(stderr, "== through rvvm_fs: %s\n", view.ops->name);
    exercise(&view, view.ops->name);
    rvvm_memfs_free(mem);

    host = rvvm_hostfs_create(argv[1], false);
    ck(host != NULL, "a host filesystem");
    if (!host) {
        return 1;
    }
    view = rvvm_fs_view_hostfs(host);
    fprintf(stderr, "== through rvvm_fs: %s\n", view.ops->name);
    exercise(&view, view.ops->name);
    rvvm_hostfs_free(host);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
