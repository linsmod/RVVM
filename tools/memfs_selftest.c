/* Throwaway harness for rvvm_memfs, run before the module is wired into the
 * syscall layer. Not part of the build. */
#include "core/rvvm_memfs.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static int checks = 0;

static rvvm_memfs_t* g_fs;
static void stage(const char* what)
{
    char why[256];
    if (g_fs && rvvm_memfs_selftest_consistency(g_fs, why, sizeof(why))) {
        fprintf(stderr, "== %-14s MAP BROKEN: %s\n", what, why);
    } else {
        fprintf(stderr, "== %s\n", what);
    }
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

static void ck_rc(rvvm_memfs_result_t got, rvvm_memfs_result_t want, const char* what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    }
}

int main(void)
{
    rvvm_memfs_t* fs = rvvm_memfs_create(false, 0);
    g_fs = fs;
    rvvm_memfs_info_t info;
    char name[256];
    uint8_t kind;
    uint64_t ino;
    uint32_t pos;
    char buf[64];
    size_t done;

    ck(fs != NULL, "create");
    ck_rc(rvvm_memfs_stat(fs, "/", true, &info), RVVM_MEMFS_OK, "stat root");
    ck(info.kind == RVVM_MEMFS_DIR, "root is a dir");
    ck(info.nlink == 2, "root nlink 2");
    ck_rc(rvvm_memfs_stat(fs, "/nope", true, &info), RVVM_MEMFS_ENOENT, "stat missing");

    stage("normalize");
    /* --- normalization, including the ".." fix --- */
    ck_rc(rvvm_memfs_mkdir(fs, "/a", 0755), RVVM_MEMFS_OK, "mkdir /a");
    ck_rc(rvvm_memfs_stat(fs, "//a", true, &info), RVVM_MEMFS_OK, "//a == /a");
    ck_rc(rvvm_memfs_stat(fs, "/a/", true, &info), RVVM_MEMFS_OK, "/a/ == /a");
    ck_rc(rvvm_memfs_stat(fs, "/./a", true, &info), RVVM_MEMFS_OK, "/./a == /a");
    ck_rc(rvvm_memfs_stat(fs, "/x/../a", true, &info), RVVM_MEMFS_OK, "/x/../a == /a");
    ck_rc(rvvm_memfs_stat(fs, "/../a", true, &info), RVVM_MEMFS_EINVAL, "/../a refused");
    ck_rc(rvvm_memfs_stat(fs, "/a/../..", true, &info), RVVM_MEMFS_EINVAL, "walk above root");
    ck_rc(rvvm_memfs_mkdir(fs, "/a/b", 0755), RVVM_MEMFS_OK, "mkdir /a/b");
    ck_rc(rvvm_memfs_mkdir(fs, "/a/b/c", 0755), RVVM_MEMFS_OK, "mkdir /a/b/c");
    /* mkdir is not recursive, exactly as on Linux: the parent has to exist, and
     * saying so is the difference between ENOENT and a filesystem that invented
     * a directory the guest did not ask for. */
    ck_rc(rvvm_memfs_mkdir(fs, "/no/such/parent", 0755), RVVM_MEMFS_ENOENT,
          "mkdir under a missing parent is ENOENT, not a silent -p");
    ck_rc(rvvm_memfs_stat(fs, "/a/b/../b/c", true, &info), RVVM_MEMFS_OK, "pop back down");
    ck_rc(rvvm_memfs_mkdir(fs, "/a", 0755), RVVM_MEMFS_EEXIST, "mkdir existing is EEXIST");
    ck_rc(rvvm_memfs_mkdir(fs, "/a/b/c/d", 0755), RVVM_MEMFS_OK, "deep mkdir");
    /* The depth cap, and that it is a refusal rather than a truncation. */
    {
        char deep[512];
        size_t o = 0;
        int i;
        deep[0] = '\0';
        for (i = 0; i < RVVM_MEMFS_MAX_SEGS + 2; i++) {
            int n = snprintf(deep + o, sizeof(deep) - o, "/d%d", i);
            if (n < 0 || (size_t)n >= sizeof(deep) - o) {
                break; /* snprintf reports what it WOULD have written */
            }
            o += (size_t)n;
        }
        ck(i > RVVM_MEMFS_MAX_SEGS, "the deep path really is deeper than the cap");
        ck_rc(rvvm_memfs_mkdir(fs, deep, 0755), RVVM_MEMFS_EINVAL, "too deep refused");
    }

    stage("files");
    /* --- files --- */
    ck_rc(rvvm_memfs_create_file(fs, "/a/f", 0644), RVVM_MEMFS_OK, "create /a/f");
    ck_rc(rvvm_memfs_write(fs, "/a/f", 0, "hello", 5, &done), RVVM_MEMFS_OK, "write");
    ck(done == 5, "write count");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_memfs_read(fs, "/a/f", 0, buf, sizeof(buf), &done), RVVM_MEMFS_OK, "read");
    ck(done == 5 && !strcmp(buf, "hello"), "read back");
    ck_rc(rvvm_memfs_read(fs, "/a/f", 99, buf, sizeof(buf), &done), RVVM_MEMFS_OK, "read past end");
    ck(done == 0, "read past end is 0 not an error");
    /* A sparse write has to leave zeros in the gap. */
    ck_rc(rvvm_memfs_create_file(fs, "/a/sparse", 0644), RVVM_MEMFS_OK, "create sparse");
    ck_rc(rvvm_memfs_write(fs, "/a/sparse", 4, "X", 1, &done), RVVM_MEMFS_OK, "sparse write");
    memset(buf, 0, sizeof(buf));
    ck_rc(rvvm_memfs_read(fs, "/a/sparse", 0, buf, 5, &done), RVVM_MEMFS_OK, "sparse read");
    ck(done == 5 && !memcmp(buf, "\0\0\0\0X", 5), "sparse gap is zeros");
    ck_rc(rvvm_memfs_create_file(fs, "/a", 0644), RVVM_MEMFS_EISDIR, "create over a dir");
    ck_rc(rvvm_memfs_create_file(fs, "/a/f/nope", 0644), RVVM_MEMFS_ENOTDIR, "under a file");

    stage("listing");
    /* --- listing --- */
    pos = 0;
    ck_rc(rvvm_memfs_getdents(fs, "/a", &pos, name, sizeof(name), &kind, &ino),
          RVVM_MEMFS_OK, "getdents /a[0]");
    ck_rc(rvvm_memfs_getdents(fs, "/a", &pos, name, sizeof(name), &kind, &ino),
          RVVM_MEMFS_OK, "getdents /a[1]");
    ck_rc(rvvm_memfs_getdents(fs, "/a", &pos, name, sizeof(name), &kind, &ino),
          RVVM_MEMFS_OK, "getdents /a[2]");
    ck_rc(rvvm_memfs_getdents(fs, "/a", &pos, name, sizeof(name), &kind, &ino),
          RVVM_MEMFS_EOF, "getdents /a done (3 entries: b, f, sparse)");
    ck_rc(rvvm_memfs_getdents(fs, "/a/f", &pos, name, sizeof(name), &kind, &ino),
          RVVM_MEMFS_ENOTDIR, "getdents on a file");

    stage("symlinks");
    /* --- symlinks --- */
    ck_rc(rvvm_memfs_symlink(fs, "f", "/a/link"), RVVM_MEMFS_OK, "symlink relative");
    ck_rc(rvvm_memfs_readlink(fs, "/a/link", buf, sizeof(buf)), RVVM_MEMFS_OK, "readlink");
    ck(!strcmp(buf, "f"), "readlink content");
    ck_rc(rvvm_memfs_stat(fs, "/a/link", false, &info), RVVM_MEMFS_OK, "lstat link");
    ck(info.kind == RVVM_MEMFS_LNK, "lstat says link");
    ck_rc(rvvm_memfs_stat(fs, "/a/link", true, &info), RVVM_MEMFS_OK, "stat follows");
    ck(info.kind == RVVM_MEMFS_REG, "stat follows to the file");
    ck(info.size == 5, "stat reports the target's size");
    ck_rc(rvvm_memfs_readlink(fs, "/a/f", buf, sizeof(buf)), RVVM_MEMFS_EINVAL,
          "readlink a non-link");
    ck_rc(rvvm_memfs_readlink(fs, "/a/nope", buf, sizeof(buf)), RVVM_MEMFS_ENOENT,
          "readlink missing");
    ck_rc(rvvm_memfs_symlink(fs, "loop", "/a/loop"), RVVM_MEMFS_OK, "symlink loop");
    ck_rc(rvvm_memfs_stat(fs, "/a/loop", true, &info), RVVM_MEMFS_ELOOP, "self loop is ELOOP");
    ck_rc(rvvm_memfs_symlink(fs, "b1", "/a/b1"), RVVM_MEMFS_OK, "symlink b1");
    ck_rc(rvvm_memfs_symlink(fs, "b2", "/a/b2"), RVVM_MEMFS_OK, "symlink b2");
    /* Relative resolution has to compose with the directory, not the mount. */
    ck_rc(rvvm_memfs_symlink(fs, "/a/f", "/abs"), RVVM_MEMFS_OK, "symlink absolute");
    ck_rc(rvvm_memfs_stat(fs, "/abs", true, &info), RVVM_MEMFS_OK, "absolute link resolves");
    ck(info.size == 5, "absolute link target size");

    stage("hardlink");
    /* --- hard links: one inode, several names --- */
    {
        uint64_t ino_h;
        uint32_t names_before;
        ck_rc(rvvm_memfs_create_file(fs, "/a/h0", 0644), RVVM_MEMFS_OK, "create /a/h0");
        ck_rc(rvvm_memfs_write(fs, "/a/h0", 0, "hello", 5, &done), RVVM_MEMFS_OK, "write /a/h0");
        ck_rc(rvvm_memfs_stat(fs, "/a/h0", true, &info), RVVM_MEMFS_OK, "stat /a/h0");
        ino_h = info.ino;
        ck(info.nlink == 1, "one name to start");
        names_before = rvvm_memfs_count(fs);
        ck_rc(rvvm_memfs_link(fs, "/a/h0", "/a/h1"), RVVM_MEMFS_OK, "link /a/h0 -> /a/h1");
        ck(rvvm_memfs_count(fs) == names_before, "a hard link adds no node");
        ck_rc(rvvm_memfs_stat(fs, "/a/h1", true, &info), RVVM_MEMFS_OK, "stat the second name");
        ck(info.ino == ino_h, "the two names share an inode");
        ck(info.nlink == 2, "nlink counts both names");
        ck_rc(rvvm_memfs_stat(fs, "/a/h0", true, &info), RVVM_MEMFS_OK, "stat the first name");
        ck(info.nlink == 2, "nlink is the same from either name");
        /* A write through the second name is visible through the first - the
         * property a copy would get wrong, and the whole reason a hard link
         * exists. */
        ck_rc(rvvm_memfs_write(fs, "/a/h1", 5, "!", 1, &done), RVVM_MEMFS_OK, "write through the link");
        memset(buf, 0, sizeof(buf));
        ck_rc(rvvm_memfs_read(fs, "/a/h0", 0, buf, sizeof(buf), &done), RVVM_MEMFS_OK, "read the original");
        ck(done == 6 && !strcmp(buf, "hello!"), "the write is visible through the other name");
        /* One name going leaves the other, and drops nlink. */
        ck_rc(rvvm_memfs_unlink(fs, "/a/h0", false), RVVM_MEMFS_OK, "unlink one name");
        ck_rc(rvvm_memfs_stat(fs, "/a/h0", true, &info), RVVM_MEMFS_ENOENT, "the removed name is gone");
        ck_rc(rvvm_memfs_stat(fs, "/a/h1", true, &info), RVVM_MEMFS_OK, "the other name survives");
        ck(info.nlink == 1, "nlink fell to one");
        ck(info.size == 6, "and the contents are intact");
        ck_rc(rvvm_memfs_unlink(fs, "/a/h1", false), RVVM_MEMFS_OK, "unlink the last name");
        ck_rc(rvvm_memfs_stat(fs, "/a/h1", true, &info), RVVM_MEMFS_ENOENT, "the inode went with the last name");

        /* Errors. */
        ck_rc(rvvm_memfs_link(fs, "/a/nope", "/a/x"), RVVM_MEMFS_ENOENT, "link a missing source");
        ck_rc(rvvm_memfs_create_file(fs, "/a/t", 0644), RVVM_MEMFS_OK, "create a name to collide with");
        ck_rc(rvvm_memfs_create_file(fs, "/a/src2", 0644), RVVM_MEMFS_OK, "a real source");
        ck_rc(rvvm_memfs_link(fs, "/a/src2", "/a/t"), RVVM_MEMFS_EEXIST, "link onto a taken name");
        ck_rc(rvvm_memfs_link(fs, "/a", "/a/dirlink"), RVVM_MEMFS_EPERM, "no hard link to a directory");
        /* A link to a symlink links the symlink itself (Linux linkat, no flags),
         * not what it points at. */
        ck_rc(rvvm_memfs_link(fs, "/a/link", "/a/link2"), RVVM_MEMFS_OK, "link a symlink");
        ck_rc(rvvm_memfs_stat(fs, "/a/link2", false, &info), RVVM_MEMFS_OK, "lstat the linked symlink");
        ck(info.kind == RVVM_MEMFS_LNK, "it is still a symlink");
        ck(info.nlink == 2, "the symlink's own nlink counted the name");
        ck_rc(rvvm_memfs_readlink(fs, "/a/link2", buf, sizeof(buf)), RVVM_MEMFS_OK, "readlink it");
        ck(!strcmp(buf, "f"), "it points where the original did");

        /* Renaming one name of a hard-linked file moves that name only. */
        ck_rc(rvvm_memfs_create_file(fs, "/a/one", 0644), RVVM_MEMFS_OK, "create /a/one");
        ck_rc(rvvm_memfs_link(fs, "/a/one", "/a/two"), RVVM_MEMFS_OK, "link /a/one -> /a/two");
        ck_rc(rvvm_memfs_rename(fs, "/a/two", "/a/three"), RVVM_MEMFS_OK, "rename the second name");
        ck_rc(rvvm_memfs_stat(fs, "/a/one", true, &info), RVVM_MEMFS_OK, "the first name is untouched");
        ck(info.nlink == 2, "still two names");
        ck_rc(rvvm_memfs_stat(fs, "/a/three", true, &info), RVVM_MEMFS_OK, "the renamed name is there");
        ck_rc(rvvm_memfs_stat(fs, "/a/two", true, &info), RVVM_MEMFS_ENOENT, "the old name is gone");
    }

    stage("dirnlink");
    /* --- a directory's nlink is 2 + its subdirectories --- */
    {
        uint32_t root_before;
        ck_rc(rvvm_memfs_stat(fs, "/", true, &info), RVVM_MEMFS_OK, "stat root");
        root_before = info.nlink;
        ck_rc(rvvm_memfs_mkdir(fs, "/dn", 0755), RVVM_MEMFS_OK, "mkdir /dn");
        ck_rc(rvvm_memfs_stat(fs, "/", true, &info), RVVM_MEMFS_OK, "stat root again");
        ck(info.nlink == root_before + 1, "a subdirectory adds a link to its parent");
        ck_rc(rvvm_memfs_stat(fs, "/dn", true, &info), RVVM_MEMFS_OK, "stat the new dir");
        ck(info.nlink == 2, "an empty directory is 2");
        ck_rc(rvvm_memfs_mkdir(fs, "/dn/s", 0755), RVVM_MEMFS_OK, "mkdir /dn/s");
        ck_rc(rvvm_memfs_stat(fs, "/dn", true, &info), RVVM_MEMFS_OK, "stat /dn");
        ck(info.nlink == 3, "each subdirectory is one more link");
        /* Renaming a directory into a different parent moves the link. */
        ck_rc(rvvm_memfs_mkdir(fs, "/other", 0755), RVVM_MEMFS_OK, "mkdir /other");
        ck_rc(rvvm_memfs_rename(fs, "/dn/s", "/other/s"), RVVM_MEMFS_OK, "move a subdirectory");
        ck_rc(rvvm_memfs_stat(fs, "/dn", true, &info), RVVM_MEMFS_OK, "stat the old parent");
        ck(info.nlink == 2, "the old parent lost a link");
        ck_rc(rvvm_memfs_stat(fs, "/other", true, &info), RVVM_MEMFS_OK, "stat the new parent");
        ck(info.nlink == 3, "the new parent gained one");
        /* And removing the subdirectory takes it back. */
        ck_rc(rvvm_memfs_unlink(fs, "/other/s", true), RVVM_MEMFS_OK, "rmdir the moved subdir");
        ck_rc(rvvm_memfs_stat(fs, "/other", true, &info), RVVM_MEMFS_OK, "stat the new parent again");
        ck(info.nlink == 2, "rmdir took the link back");
    }

    stage("rename");
    /* --- rename, including the subtree reindex --- */
    ck_rc(rvvm_memfs_mkdir(fs, "/src", 0755), RVVM_MEMFS_OK, "mkdir /src");
    ck_rc(rvvm_memfs_mkdir(fs, "/src/inner", 0755), RVVM_MEMFS_OK, "mkdir /src/inner");
    ck_rc(rvvm_memfs_create_file(fs, "/src/inner/deep", 0644), RVVM_MEMFS_OK, "deep file");
    ck_rc(rvvm_memfs_write(fs, "/src/inner/deep", 0, "D", 1, &done), RVVM_MEMFS_OK, "write deep");
    ck_rc(rvvm_memfs_rename(fs, "/src", "/dst"), RVVM_MEMFS_OK, "rename a directory");
    ck_rc(rvvm_memfs_stat(fs, "/src", true, &info), RVVM_MEMFS_ENOENT, "old name gone");
    /* The whole point of the reindex: the child is now findable at its NEW key,
     * and NOT at the old one. A rename that only moved the root would leave the
     * subtree filed under paths that no longer lead anywhere. */
    ck_rc(rvvm_memfs_stat(fs, "/dst/inner/deep", true, &info), RVVM_MEMFS_OK, "deep under new name");
    ck(info.size == 1, "deep file kept its contents");
    ck_rc(rvvm_memfs_stat(fs, "/src/inner/deep", true, &info), RVVM_MEMFS_ENOENT,
          "old subtree key gone");
    ck_rc(rvvm_memfs_mkdir(fs, "/dst/inner/2", 0755), RVVM_MEMFS_OK, "write into moved tree");
    ck_rc(rvvm_memfs_rename(fs, "/dst/inner/2", "/dst/inner/3"), RVVM_MEMFS_OK, "rename leaf");
    ck_rc(rvvm_memfs_stat(fs, "/dst/inner/3", true, &info), RVVM_MEMFS_OK, "renamed leaf found");
    ck_rc(rvvm_memfs_stat(fs, "/dst/inner/2", true, &info), RVVM_MEMFS_ENOENT, "renamed leaf gone");
    /* A directory cannot be moved inside itself. */
    ck_rc(rvvm_memfs_rename(fs, "/dst", "/dst/inner/self"), RVVM_MEMFS_EINVAL, "no self-move");
    /* Rename over an existing empty directory replaces it. */
    ck_rc(rvvm_memfs_mkdir(fs, "/victim", 0755), RVVM_MEMFS_OK, "mkdir /victim");
    ck_rc(rvvm_memfs_mkdir(fs, "/swap", 0755), RVVM_MEMFS_OK, "mkdir /swap");
    ck_rc(rvvm_memfs_rename(fs, "/swap", "/victim"), RVVM_MEMFS_OK, "rename over empty dir");
    ck_rc(rvvm_memfs_stat(fs, "/swap", true, &info), RVVM_MEMFS_ENOENT, "source gone after replace");
    ck_rc(rvvm_memfs_stat(fs, "/victim", true, &info), RVVM_MEMFS_OK, "target is the new dir");
    ck_rc(rvvm_memfs_mkdir(fs, "/full", 0755), RVVM_MEMFS_OK, "mkdir /full");
    ck_rc(rvvm_memfs_create_file(fs, "/full/kid", 0644), RVVM_MEMFS_OK, "kid in /full");
    ck_rc(rvvm_memfs_rename(fs, "/victim", "/full"), RVVM_MEMFS_ENOTEMPTY, "rename over full dir");

    stage("unlink");
    /* --- unlink, and that the map forgets --- */
    ck_rc(rvvm_memfs_stat(fs, "/a/f", true, &info), RVVM_MEMFS_OK, "/a/f is still there before the rmdir");
    ck_rc(rvvm_memfs_unlink(fs, "/victim", false), RVVM_MEMFS_EISDIR, "unlink a dir as file");
    ck_rc(rvvm_memfs_unlink(fs, "/a/f", true), RVVM_MEMFS_ENOTDIR, "rmdir a file");
    ck_rc(rvvm_memfs_mkdir(fs, "/empty", 0755), RVVM_MEMFS_OK, "mkdir /empty");
    ck_rc(rvvm_memfs_mkdir(fs, "/empty/kid", 0755), RVVM_MEMFS_OK, "kid in /empty");
    ck_rc(rvvm_memfs_unlink(fs, "/empty", true), RVVM_MEMFS_ENOTEMPTY, "rmdir non-empty");
    ck_rc(rvvm_memfs_unlink(fs, "/empty/kid", true), RVVM_MEMFS_OK, "rmdir the kid");
    ck_rc(rvvm_memfs_unlink(fs, "/empty", true), RVVM_MEMFS_OK, "rmdir now empty");
    ck_rc(rvvm_memfs_stat(fs, "/empty", true, &info), RVVM_MEMFS_ENOENT, "removed dir gone");

    /* Remove a node whose key hashed into a cluster, then confirm the entry
     * beyond it is still findable. This is the map-remove hole: an entry hidden
     * behind a deleted one is invisible to every lookup that walks the chain. */
    {
        int i;
        char p[64];
        ck_rc(rvvm_memfs_mkdir(fs, "/clu", 0755), RVVM_MEMFS_OK, "mkdir /clu");
        for (i = 0; i < 40; i++) {
            snprintf(p, sizeof(p), "/clu/%02d", i);
            if (rvvm_memfs_create_file(fs, p, 0644) != RVVM_MEMFS_OK) {
                ck(0, p);
            } else if (rvvm_memfs_stat(fs, p, true, &info) != RVVM_MEMFS_OK) {
                /* Findable the instant it was made: if this fires, the insert is
                 * losing entries and every later failure is a consequence. */
                ck(0, "just-created file is not findable");
                break;
            }
        }
        for (i = 0; i < 40; i += 2) {
            snprintf(p, sizeof(p), "/clu/%02d", i);
            ck_rc(rvvm_memfs_unlink(fs, p, false), RVVM_MEMFS_OK, "cluster unlink");
        }
        {
            char why[256];
            if (rvvm_memfs_selftest_consistency(fs, why, sizeof(why))) {
                ck(0, why);
            } else {
                ck(1, "map and nodes agree after 20 removals");
            }
        }
        for (i = 1; i < 40; i += 2) {
            snprintf(p, sizeof(p), "/clu/%02d", i);
            ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "survivor still findable");
        }
        /* And the name is reusable, which is what "the map forgot it" means. */
        snprintf(p, sizeof(p), "/clu/%02d", 0);
        ck_rc(rvvm_memfs_create_file(fs, p, 0644), RVVM_MEMFS_OK, "recreate a removed name");
        ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "recreated is findable");
        /* Reusing a hole must not put two nodes on one name, and must not lose
         * the slot's old identity: the fresh node's inode has to be its own. */
        {
            uint64_t first = 0;
            snprintf(p, sizeof(p), "/clu/%02d", 3);
            ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "/clu/03 present");
            first = info.ino;
            ck_rc(rvvm_memfs_unlink(fs, p, false), RVVM_MEMFS_OK, "remove /clu/03");
            ck_rc(rvvm_memfs_create_file(fs, p, 0644), RVVM_MEMFS_OK, "recreate /clu/03");
            ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "recreated /clu/03");
            ck(info.ino != first, "a reused slot gets a fresh inode");
            /* The neighbours of the reused slot are the real test: a hole in the
             * map that was closed wrongly hides whatever probed past it. */
            snprintf(p, sizeof(p), "/clu/%02d", 5);
            ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "/clu/05 still there");
            snprintf(p, sizeof(p), "/clu/%02d", 1);
            ck_rc(rvvm_memfs_stat(fs, p, true, &info), RVVM_MEMFS_OK, "/clu/01 still there");
        }
    }

    stage("sizebudget");
    /* --- truncate and the size budget --- */
    {
        rvvm_memfs_t* q = rvvm_memfs_create(false, 100);
        ck(q != NULL, "create with a limit");
        ck_rc(rvvm_memfs_create_file(q, "/f", 0644), RVVM_MEMFS_OK, "file in limited fs");
        ck_rc(rvvm_memfs_write(q, "/f", 0, "0123456789", 10, &done), RVVM_MEMFS_OK, "write 10");
        ck(rvvm_memfs_used(q) == 10, "used counts 10");
        /* A write at offset 90 makes the file 91 bytes, which a 100-byte tmpfs
         * can hold. The boundary is what matters, so this asks for the byte that
         * does not fit rather than one that nearly does. */
        ck_rc(rvvm_memfs_write(q, "/f", 100, "x", 1, &done), RVVM_MEMFS_ENOSPC, "over the limit");
        ck_rc(rvvm_memfs_read(q, "/f", 0, buf, sizeof(buf), &done), RVVM_MEMFS_OK, "read after ENOSPC");
        ck(done == 10, "a refused write changed nothing");
        /* And the boundary from the other side: exactly the limit is allowed. */
        ck_rc(rvvm_memfs_write(q, "/f", 99, "x", 1, &done), RVVM_MEMFS_OK, "exactly the limit fits");
        ck(rvvm_memfs_used(q) == 100, "used is at the limit");
        /* Truncating has to give the budget back, or a guest that cuts a file to
         * fit and rewrites it is stuck on a filesystem that is now empty. */
        ck_rc(rvvm_memfs_truncate(q, "/f", 4), RVVM_MEMFS_OK, "truncate to 4");
        ck(rvvm_memfs_used(q) == 4, "truncate released bytes");
        ck_rc(rvvm_memfs_write(q, "/f", 0, "abcdefghij", 10, &done), RVVM_MEMFS_OK, "rewrite to 10");
        ck(rvvm_memfs_used(q) == 10, "back to 10");
        ck_rc(rvvm_memfs_truncate(q, "/f", 0), RVVM_MEMFS_OK, "truncate to 0");
        ck(rvvm_memfs_used(q) == 0, "truncate to 0 releases everything");
        /* O_CREAT on an existing file truncates it and releases its bytes. */
        ck_rc(rvvm_memfs_write(q, "/f", 0, "0123456789", 10, &done), RVVM_MEMFS_OK, "refill");
        ck_rc(rvvm_memfs_create_file(q, "/f", 0644), RVVM_MEMFS_OK, "O_CREAT truncates");
        ck(rvvm_memfs_used(q) == 0, "O_CREAT released the bytes");
        /* The budget is the filesystem's, not a file's: two files share it. */
        ck_rc(rvvm_memfs_create_file(q, "/g", 0644), RVVM_MEMFS_OK, "second file");
        ck_rc(rvvm_memfs_write(q, "/f", 0, "0123456789", 10, &done), RVVM_MEMFS_OK, "f at 10");
        ck_rc(rvvm_memfs_write(q, "/g", 0, "0123456789", 10, &done), RVVM_MEMFS_OK, "g takes the other 10");
        ck(rvvm_memfs_used(q) == 20, "used is the sum of both files");
        ck_rc(rvvm_memfs_write(q, "/g", 0, "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890", 100, &done),
              RVVM_MEMFS_ENOSPC, "a third file sees both of the others");
        /* And removing one gives them back. */
        ck_rc(rvvm_memfs_unlink(q, "/f", false), RVVM_MEMFS_OK, "unlink f");
        ck(rvvm_memfs_used(q) == 10, "unlink released f's bytes");
        ck_rc(rvvm_memfs_write(q, "/g", 0, "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890", 90, &done),
              RVVM_MEMFS_OK, "g now has the whole budget");
        /* A subtree removal releases the whole subtree's bytes. */
        ck_rc(rvvm_memfs_mkdir(q, "/d", 0755), RVVM_MEMFS_OK, "mkdir /d");
        ck_rc(rvvm_memfs_create_file(q, "/d/x", 0644), RVVM_MEMFS_OK, "file in the subtree");
        ck_rc(rvvm_memfs_write(q, "/d/x", 0, "0123456789", 10, &done), RVVM_MEMFS_OK, "fill it");
        ck_rc(rvvm_memfs_unlink(q, "/d/x", false), RVVM_MEMFS_OK, "unlink the file");
        ck(rvvm_memfs_used(q) == 90, "the subtree's bytes came back too");
        rvvm_memfs_free(q);
    }

    stage("readonly");
    /* --- read-only, which is the whole reason this is a filesystem --- */
    {
        rvvm_memfs_t* ro = rvvm_memfs_create(true, 0);
        ck(ro != NULL, "create read-only");
        ck(rvvm_memfs_read_only(ro), "reports read-only");
        ck_rc(rvvm_memfs_mkdir(ro, "/d", 0755), RVVM_MEMFS_EROFS, "mkdir on ro");
        ck_rc(rvvm_memfs_create_file(ro, "/f", 0644), RVVM_MEMFS_EROFS, "create on ro");
        ck_rc(rvvm_memfs_write(ro, "/f", 0, "x", 1, &done), RVVM_MEMFS_EROFS, "write on ro");
        ck_rc(rvvm_memfs_unlink(ro, "/f", false), RVVM_MEMFS_EROFS, "unlink on ro");
        ck_rc(rvvm_memfs_rename(ro, "/f", "/g"), RVVM_MEMFS_EROFS, "rename on ro");
        ck_rc(rvvm_memfs_truncate(ro, "/f", 0), RVVM_MEMFS_EROFS, "truncate on ro");
        ck_rc(rvvm_memfs_chmod(ro, "/", 0755), RVVM_MEMFS_EROFS, "chmod on ro");
        /* A read-only mount still has a readable root and an empty listing -
         * refusing the writes must not cost it the ability to be listed. */
        pos = 0;
        ck_rc(rvvm_memfs_getdents(ro, "/", &pos, name, sizeof(name), &kind, &ino),
              RVVM_MEMFS_EOF, "read-only root lists empty");
        ck_rc(rvvm_memfs_stat(ro, "/", true, &info), RVVM_MEMFS_OK, "read-only root stats");
        rvvm_memfs_free(ro);
    }

    stage("nodebound");
    /* --- the node bound, so ENOSPC is reachable --- */
    {
        rvvm_memfs_t* tiny = rvvm_memfs_create(false, 0);
        int i;
        rvvm_memfs_result_t last = RVVM_MEMFS_OK;
        for (i = 0; i < RVVM_MEMFS_MAX_NODES + 4; i++) {
            char p[32];
            snprintf(p, sizeof(p), "/n%d", i);
            last = rvvm_memfs_create_file(tiny, p, 0644);
            if (last != RVVM_MEMFS_OK) {
                break;
            }
        }
        ck(last == RVVM_MEMFS_ENOSPC, "node bound is reachable and is ENOSPC");
        ck((int)rvvm_memfs_count(tiny) == RVVM_MEMFS_MAX_NODES, "count stopped at the bound");
        rvvm_memfs_free(tiny);
    }

    stage("longname");
    /* --- a long name, and a name that is too long --- */
    {
        char toolong[RVVM_MEMFS_NAME_MAX + 8];
        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = '\0';
        ck_rc(rvvm_memfs_create_file(fs, toolong, 0644), RVVM_MEMFS_EINVAL, "name too long");
        {
            char p[RVVM_MEMFS_NAME_MAX + 16];
            snprintf(p, sizeof(p), "/%s", toolong);
            ck_rc(rvvm_memfs_create_file(fs, p, 0644), RVVM_MEMFS_EINVAL,
                  "a component that is too long is refused");
        }
    }

    stage("free");
    /* --- free every path still reachable, and the tree is gone --- */
    ck(rvvm_memfs_count(fs) > 0, "still has nodes before free");
    rvvm_memfs_free(fs);

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
