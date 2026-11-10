#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "rmem.h"
#include "zlib.h"

static const char *TAG = "zip";

#define ZIP_EOCD_SIG   0x06054b50u
#define ZIP_CENTRAL_SIG 0x02014b50u
#define ZIP_LOCAL_SIG  0x04034b50u
#define ZIP_MAX_ENTRIES 4096
#define ZIP_MAX_CD      (8u * 1024u * 1024u)

struct zip {
    FILE *fp;
    char path[256];
    zip_entry_t *entries;
    int count;
    uint32_t cd_offset;
    uint32_t cd_size;
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t zip_open(const char *path, zip_t **out)
{
    *out = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    long fsize = ftell(fp);
    if (fsize < 22) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t tail_len = (size_t)fsize < 66000 ? (size_t)fsize : 66000;
    uint8_t *tail = malloc(tail_len);
    if (!tail) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, fsize - (long)tail_len, SEEK_SET) != 0 ||
        fread(tail, 1, tail_len, fp) != tail_len) {
        free(tail);
        fclose(fp);
        return ESP_FAIL;
    }

    long eocd = -1;
    for (long i = (long)tail_len - 22; i >= 0; i--) {
        if (rd32(tail + i) == ZIP_EOCD_SIG) {
            uint16_t comment_len = rd16(tail + i + 20);
            if ((size_t)i + 22 + comment_len <= tail_len) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd < 0) {
        ESP_LOGE(TAG, "%s: end of central directory not found", path);
        free(tail);
        fclose(fp);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t entries = rd16(tail + eocd + 10);
    uint32_t cd_size = rd32(tail + eocd + 12);
    uint32_t cd_offset = rd32(tail + eocd + 16);
    free(tail);

    if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        ESP_LOGE(TAG, "%s: ZIP64 is not supported", path);
        fclose(fp);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (entries > ZIP_MAX_ENTRIES || cd_size > ZIP_MAX_CD) {
        ESP_LOGE(TAG, "%s: archive too large for reader (%u entries, %u cd bytes)", path,
                 entries, cd_size);
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    if (fseek(fp, (long)cd_offset, SEEK_SET) != 0 || fread(cd, 1, cd_size, fp) != cd_size) {
        free(cd);
        fclose(fp);
        return ESP_FAIL;
    }

    zip_t *zip = calloc(1, sizeof(*zip));
    if (!zip) {
        free(cd);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    zip->fp = fp;
    snprintf(zip->path, sizeof(zip->path), "%s", path);
    zip->cd_offset = cd_offset;
    zip->cd_size = cd_size;
    zip->entries = calloc(entries ? entries : 1, sizeof(zip_entry_t));
    if (!zip->entries) {
        free(cd);
        free(zip);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t p = 0;
    int n = 0;
    while (p + 46 <= cd_size && n < entries) {
        if (rd32(cd + p) != ZIP_CENTRAL_SIG) {
            break;
        }
        uint16_t flags = rd16(cd + p + 8);
        uint16_t method = rd16(cd + p + 10);
        uint32_t comp = rd32(cd + p + 20);
        uint32_t uncomp = rd32(cd + p + 24);
        uint16_t name_len = rd16(cd + p + 28);
        uint16_t extra_len = rd16(cd + p + 30);
        uint16_t comment_len = rd16(cd + p + 32);
        uint32_t local = rd32(cd + p + 42);
        if (p + 46 + name_len > cd_size) {
            break;
        }
        size_t copy = name_len < sizeof(zip->entries[n].name) - 1
                          ? name_len
                          : sizeof(zip->entries[n].name) - 1;
        memcpy(zip->entries[n].name, cd + p + 46, copy);
        zip->entries[n].name[copy] = '\0';
        zip->entries[n].flags = flags;
        zip->entries[n].method = method;
        zip->entries[n].comp_size = comp;
        zip->entries[n].uncomp_size = uncomp;
        zip->entries[n].local_offset = local;
        n++;
        p += 46 + name_len + extra_len + comment_len;
    }
    free(cd);
    zip->count = n;
    ESP_LOGI(TAG, "%s: %d entries", path, n);
    *out = zip;
    return ESP_OK;
}

void zip_close(zip_t *zip)
{
    if (!zip) {
        return;
    }
    if (zip->fp) {
        fclose(zip->fp);
    }
    free(zip->entries);
    free(zip);
}

int zip_count(const zip_t *zip)
{
    return zip ? zip->count : 0;
}

const zip_entry_t *zip_entry_at(const zip_t *zip, int index)
{
    if (!zip || index < 0 || index >= zip->count) {
        return NULL;
    }
    return &zip->entries[index];
}

int zip_find(const zip_t *zip, const char *name)
{
    if (!zip || !name) {
        return -1;
    }
    for (int i = 0; i < zip->count; i++) {
        if (strcmp(zip->entries[i].name, name) == 0) {
            return i;
        }
    }
    /* Some EPUBs use backslashes or a leading slash. */
    const char *alt = name;
    while (*alt == '/') {
        alt++;
    }
    for (int i = 0; i < zip->count; i++) {
        const char *e = zip->entries[i].name;
        while (*e == '/') {
            e++;
        }
        if (strcmp(e, alt) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t inflate_entry(const uint8_t *comp, size_t comp_len, size_t uncomp_len,
                               uint8_t **out, size_t *out_len)
{
    uint8_t *buf = rmalloc(uncomp_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    if (uncomp_len == 0) {
        buf[0] = '\0';
        *out = buf;
        *out_len = 0;
        return ESP_OK;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        rfree(buf);
        return ESP_FAIL;
    }
    zs.next_in = (Bytef *)comp;
    zs.avail_in = (uInt)comp_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)uncomp_len;
    int ret = inflate(&zs, Z_FINISH);
    size_t produced = uncomp_len - zs.avail_out;
    inflateEnd(&zs);
    if (ret != Z_STREAM_END && !(ret == Z_OK && produced == uncomp_len)) {
        ESP_LOGE(TAG, "inflate failed: %d", ret);
        rfree(buf);
        return ESP_FAIL;
    }
    buf[produced] = '\0';
    *out = buf;
    *out_len = produced;
    return ESP_OK;
}

esp_err_t zip_read_index(const zip_t *zip, int index, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    const zip_entry_t *e = zip_entry_at(zip, index);
    if (!e) {
        return ESP_ERR_INVALID_ARG;
    }
    if (e->flags & 1) {
        ESP_LOGE(TAG, "%s: encrypted entry", e->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (e->comp_size == 0xFFFFFFFFu || e->uncomp_size == 0xFFFFFFFFu) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t hdr[30];
    if (fseek(zip->fp, (long)e->local_offset, SEEK_SET) != 0 ||
        fread(hdr, 1, sizeof(hdr), zip->fp) != sizeof(hdr)) {
        return ESP_FAIL;
    }
    if (rd32(hdr) != ZIP_LOCAL_SIG) {
        ESP_LOGE(TAG, "%s: bad local header", e->name);
        return ESP_FAIL;
    }
    uint16_t name_len = rd16(hdr + 26);
    uint16_t extra_len = rd16(hdr + 28);
    long data_off = (long)e->local_offset + 30 + name_len + extra_len;

    uint8_t *comp = malloc(e->comp_size ? e->comp_size : 1);
    if (!comp) {
        return ESP_ERR_NO_MEM;
    }
    if (fseek(zip->fp, data_off, SEEK_SET) != 0 ||
        fread(comp, 1, e->comp_size, zip->fp) != e->comp_size) {
        free(comp);
        return ESP_FAIL;
    }

    esp_err_t err;
    if (e->method == 0) {
        uint8_t *buf = rmalloc(e->uncomp_size + 1);
        if (!buf) {
            free(comp);
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf, comp, e->uncomp_size);
        buf[e->uncomp_size] = '\0';
        *data = buf;
        *size = e->uncomp_size;
        err = ESP_OK;
    } else if (e->method == 8) {
        err = inflate_entry(comp, e->comp_size, e->uncomp_size, data, size);
    } else {
        ESP_LOGE(TAG, "%s: unsupported compression method %u", e->name, e->method);
        err = ESP_ERR_NOT_SUPPORTED;
    }
    free(comp);
    return err;
}

esp_err_t zip_read(const zip_t *zip, const char *name, uint8_t **data, size_t *size)
{
    int idx = zip_find(zip, name);
    if (idx < 0) {
        ESP_LOGW(TAG, "entry not found: %s", name);
        return ESP_ERR_NOT_FOUND;
    }
    return zip_read_index(zip, idx, data, size);
}
