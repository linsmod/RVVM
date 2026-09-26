/*
vp_app.c - the per-app package (see vp_app.h)
*/

#include "virtpass/vp_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define app_mkdir_one(path) _mkdir(path)
#else
#include <sys/types.h>
#define app_mkdir_one(path) mkdir((path), 0755)
#endif

#include "virtpass/vp_rootfs.h" /* VP_GUEST_APP_DIR */
#include "virtpass/vp_zip.h"

#define APP_PATH_MAX 4096

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

/* Pull "key": "value" out of the flat manifest. Written by our own packer, so
 * this is a scan, not a parser: an unexpected shape is simply "not found". */
static bool app_json_string(const char* json, const char* key, char* out, size_t size)
{
    char        pattern[64];
    const char* at;
    size_t      used = 0;
    int         wrote = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if (wrote < 0 || (size_t)wrote >= sizeof(pattern) || size == 0) {
        return false;
    }
    at = strstr(json, pattern);
    if (!at) {
        return false;
    }
    at += (size_t)wrote;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' || *at == ':') {
        at++;
    }
    if (*at != '"') {
        return false;
    }
    at++;
    for (; *at && *at != '"'; at++, used++) {
        if (used + 1 >= size) {
            out[0] = 0;
            return false;
        }
        out[used] = *at;
    }
    if (*at != '"') {
        out[0] = 0;
        return false;
    }
    out[used] = 0;
    return true;
}

static bool app_json_uint(const char* json, const char* key, uint32_t* out)
{
    char        pattern[64];
    const char* at;
    uint32_t    value = 0;
    int         wrote = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if (wrote < 0 || (size_t)wrote >= sizeof(pattern)) {
        return false;
    }
    at = strstr(json, pattern);
    if (!at) {
        return false;
    }
    at += (size_t)wrote;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r' || *at == ':') {
        at++;
    }
    if (*at < '0' || *at > '9') {
        return false;
    }
    while (*at >= '0' && *at <= '9') {
        value = value * 10 + (uint32_t)(*at - '0');
        at++;
    }
    *out = value;
    return true;
}

/* An app id is one path component and nothing that resolves away: what keeps an
 * id from naming a guest path of its own. */
static bool app_valid_id(const char* id)
{
    if (!id || !*id || !strcmp(id, ".") || !strcmp(id, "..")) {
        return false;
    }
    for (const char* at = id; *at; at++) {
        char c = *at;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* An entry is relative to the app's own directory: no anchor, no climb out. */
static bool app_valid_rel(const char* rel)
{
    if (!rel || !*rel || rel[0] == '/' || rel[0] == '\\' || strstr(rel, "..")) {
        return false;
    }
    return strchr(rel, '\\') == NULL;
}

static bool app_parse_manifest(const char* json, vp_app_t* out, const char** error)
{
    uint32_t version = 0;

    memset(out, 0, sizeof(*out));
    if (!app_json_string(json, "id", out->id, sizeof(out->id)) || !app_valid_id(out->id)) {
        *error = "the manifest has no usable \"id\"";
        return false;
    }
    if (!app_json_string(json, "entry", out->entry, sizeof(out->entry)) ||
        !app_valid_rel(out->entry)) {
        *error = "the manifest has no usable \"entry\"";
        return false;
    }
    if (!app_json_string(json, "args", out->args, sizeof(out->args))) {
        out->args[0] = 0;
    }
    if (app_json_uint(json, "version", &version)) {
        out->version = version;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void app_mkdir_parents(char* path)
{
    for (char* pos = path + 1; *pos; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            char saved = *pos;
            *pos = 0;
            app_mkdir_one(path);
            *pos = saved;
        }
    }
}

static void app_mkdir_p(const char* path)
{
    char work[APP_PATH_MAX];
    snprintf(work, sizeof(work), "%s", path);
    app_mkdir_parents(work);
    app_mkdir_one(work);
}

/* Read one entry into a freshly allocated, NUL-terminated buffer. A directory
 * (no bytes) yields an empty string. */
static char* app_read_entry(const vp_zip_t* zip, uint32_t idx)
{
    const vp_zip_entry_t* entry = vp_zip_entry(zip, idx);
    char* buf;

    if (!entry) {
        return NULL;
    }
    buf = malloc((size_t)entry->size + 1);
    if (!buf) {
        return NULL;
    }
    if (!vp_zip_read(zip, idx, buf, (size_t)entry->size)) {
        free(buf);
        return NULL;
    }
    buf[entry->size] = 0;
    return buf;
}

static bool app_write_file(const char* dest, const char* guest_rel, const char* data, size_t size)
{
    char path[APP_PATH_MAX];
    FILE* f;

    if (snprintf(path, sizeof(path), "%s/%s", dest, guest_rel) >= (int)sizeof(path)) {
        return false;
    }
    app_mkdir_parents(path);
    f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

/* Record the id once in the packages list a `pm` would own. One id per line,
 * simplest thing a listing can be; a package manager can grow the line later. */
static void app_record_package(const char* dest, const char* id)
{
    char  dir[APP_PATH_MAX];
    char  path[APP_PATH_MAX];
    FILE* f;

    if (snprintf(dir, sizeof(dir), "%s/data/system", dest) >= (int)sizeof(dir)) {
        return;
    }
    app_mkdir_p(dir);
    if (snprintf(path, sizeof(path), "%s/packages.list", dir) >= (int)sizeof(path)) {
        return;
    }
    /* Already listed? Read it back: the file is tiny. */
    f = fopen(path, "rb");
    if (f) {
        char  line[VP_APP_ID_MAX + 2];
        bool  present = false;
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = 0;
            }
            if (!strcmp(line, id)) {
                present = true;
                break;
            }
        }
        fclose(f);
        if (present) {
            return;
        }
    }
    f = fopen(path, "ab");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n", id);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Public
//
// One package is installed by one primitive, app_install_zip(); the paths that
// reach it differ by *who* is installing. The bundle's pre-deployment form is a
// tar of `.vapp`s (apps.tar.gz -> apps/<id>.vapp), so the host reads a member's
// bytes and installs from memory; a future `pm install <id>.vapp` names a loose
// file and installs from the path. Same install, two ways in.
// ---------------------------------------------------------------------------

static bool app_read_manifest(vp_zip_t* zip, vp_app_t* out, const char** error)
{
    uint32_t idx = vp_zip_find(zip, VP_VAPP_MANIFEST);
    char*    manifest;
    bool     ok;

    if (idx == VP_ZIP_NONE) {
        *error = "the package has no meta.json";
        return false;
    }
    manifest = app_read_entry(zip, idx);
    if (!manifest) {
        *error = "the manifest could not be read";
        return false;
    }
    ok = app_parse_manifest(manifest, out, error);
    free(manifest);
    return ok;
}

static bool app_install_zip(vp_zip_t* zip, const char* dest, const char** error)
{
    vp_app_t app;
    char     app_dir[APP_PATH_MAX];
    char     data_dir[APP_PATH_MAX];
    uint32_t manifest_idx = vp_zip_find(zip, VP_VAPP_MANIFEST);
    size_t   prefix_len = strlen(VP_VAPP_FILES);
    bool     ok = true;

    if (manifest_idx == VP_ZIP_NONE) {
        *error = "the package has no meta.json";
        return false;
    }
    {
        char* manifest = app_read_entry(zip, manifest_idx);
        if (!manifest || !app_parse_manifest(manifest, &app, error)) {
            free(manifest);
            return false;
        }
        free(manifest);
    }
    if (snprintf(app_dir, sizeof(app_dir), "%s%s/%s", dest, VP_GUEST_APP_DIR, app.id) >=
        (int)sizeof(app_dir)) {
        *error = "the app id is too long for the host destination";
        return false;
    }

    /* The manifest beside the installed payload: a launch reads it there, so the
     * running app is defined by what was installed, not by the .vapp on disk. */
    {
        char* manifest = app_read_entry(zip, manifest_idx);
        if (!manifest || !app_write_file(app_dir, VP_VAPP_MANIFEST, manifest, strlen(manifest))) {
            *error = "the manifest could not be installed";
            free(manifest);
            return false;
        }
        free(manifest);
    }

    for (uint32_t i = 0; i < vp_zip_count(zip) && ok; i++) {
        const vp_zip_entry_t* entry = vp_zip_entry(zip, i);
        const char* rel;

        if (!entry || strcmp(entry->name, VP_VAPP_MANIFEST) == 0) {
            continue;
        }
        if (strncmp(entry->name, VP_VAPP_FILES, prefix_len) != 0) {
            continue;   /* anything outside files/ is not the app's payload */
        }
        rel = entry->name + prefix_len;
        if (!*rel || !app_valid_rel(rel)) {
            continue;
        }
        if (entry->is_dir) {
            char dir[APP_PATH_MAX];
            if (snprintf(dir, sizeof(dir), "%s/%s", app_dir, rel) < (int)sizeof(dir)) {
                app_mkdir_p(dir);
            }
            continue;
        }
        {
            char* data = app_read_entry(zip, i);
            if (!data) {
                *error = "a payload entry could not be read";
                ok = false;
            } else if (!app_write_file(app_dir, rel, data, (size_t)entry->size)) {
                *error = "a payload entry could not be written";
                ok = false;
            }
            free(data);
        }
    }

    if (ok) {
        if (snprintf(data_dir, sizeof(data_dir), "%s%s/%s", dest, VP_GUEST_DATA_DIR, app.id) >=
            (int)sizeof(data_dir)) {
            *error = "the app id is too long for the host destination";
            ok = false;
        } else {
            app_mkdir_p(data_dir);
            app_record_package(dest, app.id);
        }
    }
    return ok;
}

bool vp_app_read(const char* vapp_path, vp_app_t* out, const char** error)
{
    const char* local_error = NULL;
    vp_zip_t*   zip;
    bool        ok;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!vapp_path || !out) {
        *error = "no package was named";
        return false;
    }
    zip = vp_zip_open(vapp_path, error);
    if (!zip) {
        return false;
    }
    ok = app_read_manifest(zip, out, error);
    vp_zip_close(zip);
    return ok;
}

bool vp_app_read_memory(const void* data, size_t size, vp_app_t* out, const char** error)
{
    const char* local_error = NULL;
    vp_zip_t*   zip;
    bool        ok;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!out) {
        *error = "no manifest destination was named";
        return false;
    }
    zip = vp_zip_open_memory(data, size, error);
    if (!zip) {
        return false;
    }
    ok = app_read_manifest(zip, out, error);
    vp_zip_close(zip);
    return ok;
}

bool vp_app_install(const char* vapp_path, const char* dest, const char** error)
{
    const char* local_error = NULL;
    vp_zip_t*   zip;
    bool        ok;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!vapp_path || !dest) {
        *error = "no package or destination was named";
        return false;
    }
    zip = vp_zip_open(vapp_path, error);
    if (!zip) {
        return false;
    }
    ok = app_install_zip(zip, dest, error);
    vp_zip_close(zip);
    return ok;
}

bool vp_app_install_memory(const void* data, size_t size, const char* dest, const char** error)
{
    const char* local_error = NULL;
    vp_zip_t*   zip;
    bool        ok;

    if (!error) {
        error = &local_error;
    }
    *error = NULL;
    if (!dest) {
        *error = "no destination was named";
        return false;
    }
    zip = vp_zip_open_memory(data, size, error);
    if (!zip) {
        return false;
    }
    ok = app_install_zip(zip, dest, error);
    vp_zip_close(zip);
    return ok;
}

bool vp_app_installed(const char* dest, const char* id, vp_app_t* out)
{
    const char* error = NULL;
    char        path[APP_PATH_MAX];
    char*       json;
    FILE*       f;
    long        len;
    bool        ok;

    if (!dest || !id || !*id || !out) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s%s/%s/%s", dest, VP_GUEST_APP_DIR, id,
                 VP_VAPP_MANIFEST) >= (int)sizeof(path)) {
        return false;
    }
    f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    json = malloc((size_t)len + 1);
    if (!json) {
        fclose(f);
        return false;
    }
    if (len && fread(json, 1, (size_t)len, f) != (size_t)len) {
        free(json);
        fclose(f);
        return false;
    }
    fclose(f);
    json[len] = 0;
    ok = app_parse_manifest(json, out, &error);
    free(json);
    return ok;
}

bool vp_app_guest_entry(const char* dest, const char* id, char* out, size_t size)
{
    vp_app_t app;

    if (!out || !size || !vp_app_installed(dest, id, &app)) {
        return false;
    }
    if (snprintf(out, size, "%s/%s/%s", VP_GUEST_APP_DIR, app.id, app.entry) >= (int)size) {
        out[0] = 0;
        return false;
    }
    return true;
}