/*
vp_bundle.c - the host half of a release bundle (see vp_bundle.h)
*/

#include "virtpass/vp_bundle.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h> /* _mkdir() */
#include <io.h>     /* _setmode() */
#else
#include <unistd.h> /* unlink() / rmdir() / mkdir() */
#endif

#ifdef _WIN32
/* MAX_PATH is the CRTs' limit, not this buffer's: keeping the buffer bigger
 * costs nothing and avoids pulling <windows.h> in here for one constant. */
#define BUNDLE_PATH_MAX 2048
#define bundle_mkdir(p) _mkdir(p)
/* The guest's tree is the guest's; a host path that is a reparse point (a
 * junction, i.e. what "symlink" means on Windows) is removed by rmdir() the
 * way the shim does it, so unlink() first and rmdir() after, like the core. */
#define bundle_rmdir(p) rmdir(p)
#define bundle_unlink(p) unlink(p)
#else
#define BUNDLE_PATH_MAX 4096
#define bundle_mkdir(p) mkdir(p, 0755)
#define bundle_rmdir(p) rmdir(p)
#define bundle_unlink(p) unlink(p)
#endif

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static bool bundle_join(char* out, size_t size, const char* directory, const char* name)
{
    int wrote = snprintf(out, size, "%s/%s", directory, name);
    return wrote >= 0 && (size_t)wrote < size;
}

/* Windows compares paths without case; Android does not. The prefix a guest
 * names is always spelled the same way, so this only matters for a typed-in
 * command line. */
static bool bundle_prefix_is(const char* text, const char* prefix)
{
#ifdef _WIN32
    return _strnicmp(text, prefix, strlen(prefix)) == 0;
#else
    return strncmp(text, prefix, strlen(prefix)) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Files the archive does not ship
// ---------------------------------------------------------------------------

/* Write one host file into the materialized rootfs. It is a *host* file, and a
 * host file is what the guest sees: the shadow index only supplies what the host
 * cannot express (symlinks), so a name we write wins over the archive. */
static bool bundle_write_file(const char* dest, const char* guest_path, const char* contents)
{
    char path[BUNDLE_PATH_MAX];
    char* at;
    size_t len = strlen(contents);
    FILE* file;

    if (strlen(dest) + strlen(guest_path) + 1 > sizeof(path)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s%s", dest, guest_path);
    for (at = path + strlen(dest); *at; ++at) {
        if (*at == '/' || *at == '\\') {
            char saved = *at;
            *at = 0;
            bundle_mkdir(path);
            *at = saved;
        }
    }

    file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    bool ok = fwrite(contents, 1, len, file) == len;
    fclose(file);
    return ok;
}

/* inittab: Alpine's own starts /sbin/openrc, which the minirootfs does not ship,
 * so a session started from it never reaches a shell. Ours runs one on the run's
 * console - the session the host attached, which the core answers as both
 * /dev/console and /dev/tty1.
 *
 * /proc/mounts: the archive has /proc as an empty directory and /etc/mtab is a
 * symlink to ../proc/mounts. A generated listing is what mount(8) and anything
 * that reads mtab expect; the mount table will generate it properly once there
 * is one, and until then these are the mounts the guest is really given. */
static const char* bundle_inittab =
    "# Written by the VirtPass host at install time (handover.md, Step 2).\n"
    "#\n"
    "# The archive ships Alpine's inittab, whose sysinit is /sbin/openrc - a\n"
    "# package the minirootfs does not contain - so it never reaches a shell.\n"
    "# This one runs a shell on the run's own console instead: a userland guest\n"
    "# has exactly one terminal, which the core answers as /dev/console and as\n"
    "# every /dev/ttyN (so it works whatever tty an inittab names).\n"
    "tty1::respawn:/bin/sh\n";

static bool bundle_write_session_files(const char* dest);

static bool bundle_path_exists(const char* dest, const char* guest_path)
{
    char path[BUNDLE_PATH_MAX];
    FILE* f;

    if (strlen(dest) + strlen(guest_path) + 1 > sizeof(path)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s%s", dest, guest_path);
    f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

/* /etc/inittab is only written when the archive's own is still there: a guest
 * that edited it must find its version on the next run, which is what
 * persistence means for a file the host also owns.
 *
 * /proc/mounts used to be written here too, from a string constant in this file.
 * It is generated now, by the core, from the mount table that actually decides
 * which paths skip the hostfs prefix - see userland_proc_gen_mounts() in
 * rvvm_user.c. A copy written here was a transcription kept somewhere else from
 * the namespace it described, and it did drift: it listed a devpts mount on
 * /dev/pts that nothing implements, and mount(8), df(1) and every library
 * reading /proc/mounts believed it. Generated, it cannot. */
static bool bundle_write_session_files(const char* dest)
{
    if (!bundle_path_exists(dest, "/etc/inittab")) {
        return bundle_write_file(dest, "/etc/inittab", bundle_inittab);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* The install stamp                                                   */
/*                                                                    */
/* `dest` is not a scratch copy: it is the writable layer the guest    */
/* keeps its state in (handover.md §1.7). So the archive is unpacked    */
/* once, and a later run of the *same* bundle must not walk over what   */
/* the guest changed. A stamp under `<dest>/.vp/` ties the tree to the  */
/* archive's size and mtime, and an updated archive re-extracts. A dot  */
/* directory keeps it out of a plain `ls /`; the shadow does not know   */
/* host files, so it is not hidden from `ls -a` - a known, harmless     */
/* deviation (§6).                                                     */
/* ------------------------------------------------------------------ */

static bool bundle_stamp_path(const char* dest, const char* stamp, char* out, size_t size)
{
    char dir[BUNDLE_PATH_MAX];

    if (!bundle_join(dir, sizeof(dir), dest, ".vp")) {
        return false;
    }
    bundle_mkdir(dir);
    return bundle_join(out, size, dir, stamp);
}

static bool bundle_stamp_ok(const char* dest, const char* stamp, const char* archive)
{
    struct stat ast;
    char        path[BUNDLE_PATH_MAX];
    char        want[128];
    char        got[128];
    size_t      n;
    FILE*       f;

    if (stat(archive, &ast) != 0 || !bundle_stamp_path(dest, stamp, path, sizeof(path))) {
        return false;   /* no archive to fingerprint: extract and find out */
    }
    f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    got[n] = 0;
    snprintf(want, sizeof(want), "size=%lld\nmtime=%lld\n",
             (long long)ast.st_size, (long long)ast.st_mtime);
    return strcmp(want, got) == 0;
}

static void bundle_stamp_write(const char* dest, const char* stamp, const char* archive)
{
    struct stat ast;
    char        path[BUNDLE_PATH_MAX];
    FILE*       f;

    if (stat(archive, &ast) != 0 || !bundle_stamp_path(dest, stamp, path, sizeof(path))) {
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "size=%lld\nmtime=%lld\n", (long long)ast.st_size, (long long)ast.st_mtime);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Mounting a run
// ---------------------------------------------------------------------------

static vp_rootfs_t* g_mounted = NULL;

static void bundle_remove_tree(const char* path);
static bool bundle_provision_apps(const char* apps_tar_gz, const char* dest,
                                  size_t* installed, const char** error);

void vp_bundle_unmount(void)
{
    if (g_mounted) {
        vp_rootfs_close(g_mounted);
        g_mounted = NULL;
    }
}

/* Extract one optional bundle layer into @dest the first time (or after the
 * archive changes), stamped under <dest>/.vp/<stamp>. Regular files and
 * directories only: the shadow index is the base archive's business, and a
 * system/apps layer is plain payload. A NULL/empty @tar_gz is a host without
 * that layer, which is not an error. On a fresh install *files reports what was
 * written; a repeat of an unchanged archive writes nothing and reports 0. */
static bool bundle_install_layer(const char* tar_gz, const char* dest, const char* stamp,
                                 size_t* files, const char** error)
{
    const char* local_error = NULL;
    vp_rootfs_t* layer;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (files) {
        *files = 0;
    }
    if (!tar_gz || !*tar_gz) {
        return true;
    }

    layer = vp_rootfs_open(tar_gz, error);
    if (!layer) {
        return false;
    }
    if (!bundle_stamp_ok(dest, stamp, tar_gz)) {
        size_t written = vp_rootfs_extract(layer, dest, error);
        if (!written) {
            vp_rootfs_close(layer);
            return false;
        }
        bundle_stamp_write(dest, stamp, tar_gz);
        if (files) {
            *files = written;
        }
    }
    vp_rootfs_release_data(layer);
    vp_rootfs_close(layer);
    return true;
}

/* The install for a run whose root is memory: the bundle is released into that
 * storage instead of onto the host (see rvvm_user_root_memfs()).
 *
 * What is missing here is as deliberate as what is here. No shadow, no stamp, no
 * hidden store, no mode file: each of those exists to describe a tree that
 * outlives the run - an index for the symlinks the host cannot make, a stamp so an
 * unchanged archive is not re-extracted, a file so a deletion is remembered - and
 * this tree dies with the run, so every one of them has nothing to describe.
 *
 * No apps either, and not for a good reason: vp_app installs a package into a host
 * tree at <guest>/data/app/<id>, and there is no host tree here. An app
 * provisioned for a memory root is the next piece rather than a permanent gap. */
static bool bundle_mount_memory(rvvm_machine_t* machine, vp_rootfs_t* rootfs,
                                const char* system_tar_gz, const char* apps_tar_gz,
                                vp_bundle_stats_t* stats, const char** error)
{
    rvvm_memfs_t* memfs = rvvm_user_root_memfs(machine);
    size_t        written;

    (void)apps_tar_gz;
    if (!memfs) {
        *error = "the run has no memory root to install into";
        return false;
    }
    written = vp_rootfs_install(rootfs, memfs, error);
    if (!written) {
        rvvm_memfs_free(memfs);
        return false;
    }
    if (stats) {
        stats->files = written;
    }

    /* The system layer over it, flattened the same way: the run boots the session
     * server out of its own tree (sbin/vpsessiond), so a run with no rootfs is a
     * run with no host directory behind it, not a run with nothing at all. */
    if (system_tar_gz) {
        size_t                layer_files = 0;
        vp_rootfs_t*          layer = vp_rootfs_open(system_tar_gz, error);
        bool                  ok;

        if (!layer) {
            rvvm_memfs_free(memfs);
            return false;
        }
        layer_files = vp_rootfs_install(layer, memfs, error);
        ok = (*error == NULL);
        vp_rootfs_close(layer);
        if (!ok) {
            rvvm_memfs_free(memfs);
            return false;
        }
        if (stats) {
            stats->system_files = layer_files;
        }
    }
    rvvm_memfs_free(memfs);

    /* The bytes were only needed to write the entries; the storage is the tree. */
    vp_rootfs_release_data(rootfs);
    g_mounted = rootfs;
    if (stats) {
        stats->entries = vp_rootfs_count(rootfs);
    }
    return true;
}

bool vp_bundle_mount(rvvm_machine_t* machine, const char* rootfs_tar_gz,
                     const char* system_tar_gz, const char* apps_tar_gz,
                     const char* dest, vp_bundle_stats_t* stats, const char** error)
{
    const char* local_error = NULL;
    vp_rootfs_t* rootfs;
    char pts[BUNDLE_PATH_MAX];

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (stats) {
        stats->entries = 0;
        stats->files = 0;
        stats->system_files = 0;
        stats->apps = 0;
    }
    /* @dest is the host directory the rootfs is materialized into, and it is
     * optional: a host that names none gets a run whose / is a memory filesystem
     * (see rvvm_user_set_prefix), and the bundle goes into that. */
    if (!machine || !rootfs_tar_gz) {
        *error = "no bundle was named";
        return false;
    }

    /* A previous run's index dies with its machine. */
    vp_bundle_unmount();

    rootfs = vp_rootfs_open(rootfs_tar_gz, error);
    if (!rootfs) {
        return false;
    }

    if (!dest) {
        return bundle_mount_memory(machine, rootfs, system_tar_gz, apps_tar_gz, stats, error);
    }
    /* Installed once: a run of the same archive leaves the tree as the guest
     * left it (that tree is the writable layer). An updated archive - a
     * different size or mtime - re-extracts. */
    size_t written = 0;
    if (!bundle_stamp_ok(dest, "install", rootfs_tar_gz)) {
        written = vp_rootfs_extract(rootfs, dest, error);
        if (!written) {
            vp_rootfs_close(rootfs);
            return false;
        }
        bundle_stamp_write(dest, "install", rootfs_tar_gz);
    }
    /* The tree is materialized on disk and the shadow is what the guest is
     * answered from; the inflated archive was only needed to write the files,
     * which is done. Drop it so ~7 MB does not stay resident per run. */
    vp_rootfs_release_data(rootfs);
    g_mounted = rootfs;
    if (stats) {
        stats->entries = vp_rootfs_count(rootfs);
        stats->files = written;
    }

    /* Not fatal: a guest with no inittab is still a guest, and it says so. */
    (void)bundle_write_session_files(dest);
    /* The devices themselves are synthesized by the core, but the directory they
     * live in is the host's: the archive ships /dev empty, with no pts under it,
     * and a guest that mounts devpts needs somewhere to mount it. */
    if (bundle_join(pts, sizeof(pts), dest, "dev/pts")) {
        bundle_mkdir(pts);
    }

    rvvm_user_set_prefix(machine, dest);
    rvvm_user_set_shadow(machine, vp_rootfs_shadow(rootfs));

    /* Archive-only entries the guest deletes are recorded in the shadow; keep
     * that set on disk so a deletion survives the run (see vp_shadow.h). */
    {
        char vp_dir[BUNDLE_PATH_MAX];
        char hidden[BUNDLE_PATH_MAX];
        if (bundle_join(vp_dir, sizeof(vp_dir), dest, ".vp")) {
            bundle_mkdir(vp_dir);
            if (bundle_join(hidden, sizeof(hidden), vp_dir, "hidden")) {
                vp_shadow_set_hidden_store(vp_rootfs_shadow(rootfs), hidden);
            }
        }
    }

    /* The system layer: host-provided system programs laid out at their guest
     * paths (sbin/vpsessiond, ...), flattened over the base. Like the base, the
     * host owns these - an updated archive overwrites them. */
    if (!bundle_install_layer(system_tar_gz, dest, "system",
                              stats ? &stats->system_files : NULL, error)) {
        return false;
    }

    /* The apps: every `<id>.vapp` package in the directory, installed once and
     * persistently under /data/app (the system image holds its apps). Rebuilt
     * only when the packages change, so one the bundle dropped does not linger. */
    if (!bundle_provision_apps(apps_tar_gz, dest, stats ? &stats->apps : NULL, error)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Apps
// ---------------------------------------------------------------------------

static void bundle_remove_tree(const char* path)
{
    DIR* dir = opendir(path);
    struct dirent* de;

    if (dir) {
        while ((de = readdir(dir)) != NULL) {
            char child[BUNDLE_PATH_MAX];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
                continue;
            }
            if (!bundle_join(child, sizeof(child), path, de->d_name)) {
                continue;
            }
            if (opendir(child)) {
                bundle_remove_tree(child);
            } else {
                bundle_unlink(child);
            }
        }
        closedir(dir);
    }
    bundle_rmdir(path);
}

bool vp_bundle_app_id_from_guest_path(const char* guest_path, char* out, size_t size)
{
    static const char prefix[] = "data/app/";
    const char* at = guest_path;
    const char* end;
    size_t len;

    if (!at) {
        return false;
    }
    while (*at == '/' || *at == '\\') {
        at++;
    }
    if (!bundle_prefix_is(at, prefix)) {
        return false;
    }
    at += sizeof(prefix) - 1;
    end = at;
    while (*end && *end != '/' && *end != '\\') {
        end++;
    }
    len = (size_t)(end - at);
    /* "/data/app" alone, or an id with no entry point after it, is not a launch
     * target: the entry is named by the manifest, and the host is not going to
     * guess it. */
    if (!len || *end == 0 || len + 1 > size) {
        return false;
    }
    memcpy(out, at, len);
    out[len] = 0;
    return strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
}

/* ---- the apps archive: `apps/<id>.vapp` members ----------------------- */

/* A member of the apps archive that is an app package: "/apps/<id>.vapp".
 * Fills @id (one path component) and returns true. */
static bool bundle_vapp_member(const char* path, char* id, size_t id_size)
{
    static const char prefix[] = "/" VP_APPS_MEMBER_DIR "/";
    size_t plen = sizeof(prefix) - 1;
    size_t len = strlen(path);
    size_t ext = strlen(VP_VAPP_EXT);
    size_t id_len;

    if (strncmp(path, prefix, plen) != 0 || len <= plen + ext) {
        return false;
    }
    if (strcmp(path + len - ext, VP_VAPP_EXT) != 0) {
        return false;
    }
    id_len = len - plen - ext;
    if (!id_len || id_len >= id_size || memchr(path + plen, '/', id_len)) {
        return false;
    }
    memcpy(id, path + plen, id_len);
    id[id_len] = 0;
    return true;
}

/* Read one `.vapp` member's bytes: a malloc'd buffer (caller frees) and its
 * size, or NULL. The archive's bytes must still be held (before release_data). */
static char* bundle_read_member(const vp_rootfs_t* apps, uint32_t idx, size_t* out_size)
{
    const vp_shadow_entry_t* entry = vp_shadow_entry(vp_rootfs_shadow(apps), idx);
    char* buf;

    if (!entry || entry->kind != VP_SHADOW_FILE || entry->hidden || !entry->size) {
        return NULL;
    }
    buf = malloc((size_t)entry->size);
    if (!buf) {
        return NULL;
    }
    if (vp_rootfs_read_entry(apps, idx, buf, (size_t)entry->size) == (size_t)-1) {
        free(buf);
        return NULL;
    }
    *out_size = (size_t)entry->size;
    return buf;
}

/* Install every `.vapp` the apps archive holds into <dest>/data/app/<id>. This
 * is the *pre-deployment* form (apps.tar.gz -> apps/<id>.vapp); a later
 * `pm install <id>.vapp` reaches the same vp_app_install() primitive with a
 * loose file. A run boots one of the apps; the rest stay installed, the way a
 * preinstalled image holds its apps - nothing is emptied between runs. The
 * archive is authoritative, so when it changes /data/app is rebuilt (an app it
 * dropped does not linger). A NULL/empty archive is a host without apps, which
 * is not an error. */
static bool bundle_provision_apps(const char* apps_tar_gz, const char* dest,
                                  size_t* installed, const char** error)
{
    const char*  local_error = NULL;
    char         app_tree[BUNDLE_PATH_MAX];
    vp_rootfs_t* apps;
    size_t       count, i, n = 0;
    bool         ok = true;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (installed) {
        *installed = 0;
    }
    if (!apps_tar_gz || !*apps_tar_gz) {
        return true;
    }
    if (bundle_stamp_ok(dest, "apps", apps_tar_gz)) {
        return true;   /* provisioned by an earlier run of the same archive */
    }

    apps = vp_rootfs_open(apps_tar_gz, error);
    if (!apps) {
        return false;
    }

    /* The archive is authoritative: drop what a previous archive installed, so a
     * removed app is really gone (its packages.list entry too). */
    if (bundle_join(app_tree, sizeof(app_tree), dest, "data/app")) {
        bundle_remove_tree(app_tree);
    }
    {
        char list_path[BUNDLE_PATH_MAX];
        if (bundle_join(list_path, sizeof(list_path), dest, "data/system/packages.list")) {
            remove(list_path);
        }
    }

    count = vp_rootfs_count(apps);
    for (i = 0; i < count && ok; i++) {
        const vp_shadow_entry_t* entry = vp_shadow_entry(vp_rootfs_shadow(apps), i);
        char   id[VP_APP_ID_MAX];
        char*  bytes;
        size_t size = 0;

        if (!entry || !bundle_vapp_member(entry->path, id, sizeof(id))) {
            continue;
        }
        bytes = bundle_read_member(apps, (uint32_t)i, &size);
        if (!bytes) {
            *error = "an app package could not be read from the archive";
            ok = false;
            break;
        }
        ok = vp_app_install_memory(bytes, size, dest, error);
        free(bytes);
        if (ok) {
            n++;
        }
    }

    vp_rootfs_close(apps);
    if (ok) {
        bundle_stamp_write(dest, "apps", apps_tar_gz);
        if (installed) {
            *installed = n;
        }
    }
    return ok;
}

bool vp_bundle_app_assets_path(const char* dest, const char* app_id, char* out, size_t size)
{
    char app_dir[BUNDLE_PATH_MAX];
    char assets[BUNDLE_PATH_MAX];
    struct stat st;

    if (!dest || !app_id || !*app_id || !out || !size) {
        return false;
    }
    if (!bundle_join(app_dir, sizeof(app_dir), dest, "data/app")) {
        return false;
    }
    if (snprintf(assets, sizeof(assets), "%s/%s/assets", app_dir, app_id) >= (int)sizeof(assets) ||
        stat(assets, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    snprintf(out, size, "%s", assets);
    return true;
}

size_t vp_bundle_list_apps(const char* apps_tar_gz, vp_bundle_app_t* out, size_t max)
{
    const char*  error = NULL;
    vp_rootfs_t* apps;
    size_t       found = 0, count, i;

    if (!apps_tar_gz || !out || !max) {
        return 0;
    }
    apps = vp_rootfs_open(apps_tar_gz, &error);
    if (!apps) {
        return 0;   /* no bundle is not an error: a host may run without one */
    }
    count = vp_rootfs_count(apps);
    for (i = 0; i < count; i++) {
        const vp_shadow_entry_t* entry = vp_shadow_entry(vp_rootfs_shadow(apps), i);
        char     id[VP_APP_ID_MAX];
        char*    bytes;
        size_t   size = 0;
        vp_app_t app;

        if (!entry || !bundle_vapp_member(entry->path, id, sizeof(id))) {
            continue;
        }
        bytes = bundle_read_member(apps, (uint32_t)i, &size);
        if (!bytes) {
            continue;
        }
        if (vp_app_read_memory(bytes, size, &app, &error)) {
            if (found < max) {
                snprintf(out[found].id, sizeof(out[found].id), "%s", app.id);
                snprintf(out[found].guest_path, sizeof(out[found].guest_path), "%s/%s/%s",
                         VP_GUEST_APP_DIR, app.id, app.entry);
                snprintf(out[found].args, sizeof(out[found].args), "%s", app.args);
            }
            found++;
        }
        free(bytes);
    }
    vp_rootfs_close(apps);
    return found;
}

bool vp_bundle_read_app(const char* apps_tar_gz, const char* id, vp_app_t* out)
{
    const char*  error = NULL;
    vp_rootfs_t* apps;
    char         path[BUNDLE_PATH_MAX];
    char*        bytes;
    size_t       size = 0;
    uint32_t     idx;
    bool         ok;

    if (!apps_tar_gz || !id || !*id || !out) {
        return false;
    }
    apps = vp_rootfs_open(apps_tar_gz, &error);
    if (!apps) {
        return false;
    }
    if (snprintf(path, sizeof(path), "/%s/%s%s", VP_APPS_MEMBER_DIR, id, VP_VAPP_EXT) >=
        (int)sizeof(path)) {
        vp_rootfs_close(apps);
        return false;
    }
    idx = vp_shadow_index(vp_rootfs_shadow(apps), path);
    if (idx == VP_SHADOW_NONE) {
        vp_rootfs_close(apps);
        return false;
    }
    bytes = bundle_read_member(apps, idx, &size);
    if (!bytes) {
        vp_rootfs_close(apps);
        return false;
    }
    ok = vp_app_read_memory(bytes, size, out, &error);
    free(bytes);
    vp_rootfs_close(apps);
    return ok;
}

// ---------------------------------------------------------------------------
// The /assets mount
// ---------------------------------------------------------------------------

static char g_assets_root[BUNDLE_PATH_MAX] = ".";

void vp_bundle_set_assets_root(const char* dir)
{
    if (dir && *dir) {
        snprintf(g_assets_root, sizeof(g_assets_root), "%s", dir);
    }
}

/* Which tree a call is about. A host that can have several runs at once (the
 * Android host boots one guest per card) passes its run's own root as the mount
 * userdata, because a process-global root would let one run read another's
 * resources. A host with a single run at a time (the WinHost) leaves it NULL and
 * uses the root set above. */
static const char* asset_root_of(void* user)
{
    return (user && *(const char*)user) ? (const char*)user : g_assets_root;
}

/* Resolve an asset name under the root, refusing anything that climbs out of it:
 * the tree belongs to the guest and must not turn into a way to read the host's
 * disk. Returns false when @name is not a plain relative path. */
static bool asset_host_path(const char* root, char* out, size_t size, const char* name)
{
    if (!name || !*name || name[0] == '/' || name[0] == '\\' || strstr(name, "..")) {
        return false;
    }
    /* APK asset names use '/', and so does the app payload this serves, so a
     * nested name like "fonts/x.ttf" resolves as-is. */
    return bundle_join(out, size, root, name);
}

static int asset_open_fd(void* user, const char* name)
{
    char path[BUNDLE_PATH_MAX];
    int fd;

    if (!asset_host_path(asset_root_of(user), path, sizeof(path), name)) {
        return -EINVAL;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -ENOENT;
    }
#ifdef _WIN32
    /* The guest reads raw bytes: no CRLF translation on the way through. */
    _setmode(fd, _O_BINARY);
#endif
    return fd;
}

static int64_t asset_size(void* user, const char* name)
{
    char path[BUNDLE_PATH_MAX];
    struct stat st;

    if (!asset_host_path(asset_root_of(user), path, sizeof(path), name)) {
        return -EINVAL;
    }
    if (stat(path, &st) != 0) {
        return -ENOENT;
    }
    /* A directory says so by refusing to answer a size - the core then asks
     * open_dir() and reports S_IFDIR. */
    if (S_ISDIR(st.st_mode)) {
        return -EISDIR;
    }
    return (int64_t)st.st_size;
}

static void* asset_open_dir(void* user, const char* name)
{
    char path[BUNDLE_PATH_MAX];
    const char* root = asset_root_of(user);

    if (!name || !*name) {
        /* The mount root is the asset directory itself. */
        snprintf(path, sizeof(path), "%s", root);
    } else if (!asset_host_path(root, path, sizeof(path), name)) {
        return NULL;
    }
    return opendir(path);
}

static const char* asset_dir_next(void* user, void* dir)
{
    struct dirent* de;

    (void)user;
    de = readdir(dir);
    return de ? de->d_name : NULL; /* "." and ".." are the core's to report */
}

static void asset_dir_close(void* user, void* dir)
{
    (void)user;
    closedir(dir);
}

static const rvvm_asset_ops_t bundle_asset_ops = {
    .open_fd = asset_open_fd,
    .size = asset_size,
    .open_dir = asset_open_dir,
    .dir_next = asset_dir_next,
    .dir_close = asset_dir_close,
};

const rvvm_asset_ops_t* vp_bundle_assets(void)
{
    return &bundle_asset_ops;
}
