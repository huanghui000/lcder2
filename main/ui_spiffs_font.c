#include "ui_spiffs_font.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"

#include "../GUI/src/font/lv_font_fmt_txt.h"
#include "../GUI/src/misc/lv_mem.h"

#define UI_SPIFFS_FONT_CACHE_SLOTS 64U

typedef struct {
    uint32_t version;
    uint16_t tables_count;
    uint16_t font_size;
    uint16_t ascent;
    int16_t descent;
    uint16_t typo_ascent;
    int16_t typo_descent;
    uint16_t typo_line_gap;
    int16_t min_y;
    int16_t max_y;
    uint16_t default_advance_width;
    uint16_t kerning_scale;
    uint8_t index_to_loc_format;
    uint8_t glyph_id_format;
    uint8_t advance_width_format;
    uint8_t bits_per_pixel;
    uint8_t xy_bits;
    uint8_t wh_bits;
    uint8_t advance_width_bits;
    uint8_t compression_id;
    uint8_t subpixels_mode;
    uint8_t padding;
    int16_t underline_position;
    uint16_t underline_thickness;
} ui_spiffs_font_header_t;

typedef struct {
    uint32_t data_offset;
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t data_entries_count;
    uint8_t format_type;
    uint8_t padding;
} ui_spiffs_cmap_table_t;

typedef struct {
    ui_spiffs_cmap_table_t table;
    uint32_t data_start;
} ui_spiffs_cmap_runtime_t;

typedef struct {
    uint8_t valid;
    uint32_t unicode_letter;
    uint32_t glyph_id;
    uint32_t bmp_size;
    uint32_t use_count;
    lv_font_fmt_txt_glyph_dsc_t gdsc;
    uint8_t * bitmap;
} ui_spiffs_glyph_cache_entry_t;

typedef struct {
    FILE * file;
    ui_spiffs_font_header_t header;
    ui_spiffs_cmap_runtime_t * cmap_records;
    uint32_t cmap_count;
    uint32_t table_base_offset;
    uint32_t glyph_count;
    uint32_t loca_start;
    uint32_t glyf_start;
    uint32_t glyf_length;
    uint32_t last_letter;
    uint32_t last_glyph_id;
    uint32_t last_bitmap_glyph_id;
    uint32_t bitmap_cache_size;
    uint8_t * bitmap_cache;
    uint32_t cache_use_counter;
    ui_spiffs_glyph_cache_entry_t glyph_cache[UI_SPIFFS_FONT_CACHE_SLOTS];
} ui_spiffs_font_dsc_t;

typedef struct {
    FILE * fp;
    int8_t bit_pos;
    uint8_t byte_value;
} ui_bit_iterator_t;

static const char * TAG = "ui_font_spiffs";

typedef int ui_fs_res_t;

#define UI_FS_RES_OK 0
#define UI_FS_RES_ERR (-1)

static void ui_log_font_heap(const char * stage, size_t base_heap)
{
    size_t heap_now = esp_get_free_heap_size();

    if(base_heap == 0U) {
        ESP_LOGI(TAG, "heap at %s: %u", stage, (unsigned)heap_now);
    } else {
        ESP_LOGI(TAG, "heap at %s: %u (delta=%d)", stage, (unsigned)heap_now,
            (int)heap_now - (int)base_heap);
    }
}

static const char * ui_font_resolve_path(const char * path)
{
    if(path == NULL) {
        return NULL;
    }

    if(path[0] != '\0' && path[1] == ':') {
        return path + 2;
    }

    return path;
}

static ui_fs_res_t ui_fs_open(FILE ** fp, const char * path)
{
    const char * resolved = ui_font_resolve_path(path);
    FILE * file;

    if(resolved == NULL || resolved[0] == '\0') {
        return UI_FS_RES_ERR;
    }

    file = fopen(resolved, "rb");
    if(file == NULL) {
        return UI_FS_RES_ERR;
    }

    *fp = file;
    return UI_FS_RES_OK;
}

static void ui_fs_close(FILE * fp)
{
    if(fp != NULL) {
        fclose(fp);
    }
}

static ui_fs_res_t ui_fs_seek(FILE * fp, uint32_t pos)
{
    return (fp != NULL && fseek(fp, (long)pos, SEEK_SET) == 0) ? UI_FS_RES_OK : UI_FS_RES_ERR;
}

static ui_fs_res_t ui_fs_read(FILE * fp, void * buf, size_t len, uint32_t * out_len)
{
    size_t n;

    if(fp == NULL) {
        return UI_FS_RES_ERR;
    }

    n = fread(buf, 1U, len, fp);
    if(out_len != NULL) {
        *out_len = (uint32_t)n;
    }

    return (n == len) ? UI_FS_RES_OK : UI_FS_RES_ERR;
}

static ui_bit_iterator_t ui_bit_iterator_init(FILE * fp)
{
    ui_bit_iterator_t it;
    it.fp = fp;
    it.bit_pos = -1;
    it.byte_value = 0;
    return it;
}

static unsigned int ui_read_bits(ui_bit_iterator_t * it, int n_bits, ui_fs_res_t * res)
{
    unsigned int value = 0;

    while(n_bits--) {
        it->byte_value <<= 1;
        it->bit_pos--;

        if(it->bit_pos < 0) {
            it->bit_pos = 7;
            *res = ui_fs_read(it->fp, &(it->byte_value), 1, NULL);
            if(*res != UI_FS_RES_OK) {
                return 0;
            }
        }

        value |= ((it->byte_value & 0x80U) ? 1U : 0U) << n_bits;
    }

    *res = UI_FS_RES_OK;
    return value;
}

static int ui_read_bits_signed(ui_bit_iterator_t * it, int n_bits, ui_fs_res_t * res)
{
    unsigned int value = ui_read_bits(it, n_bits, res);

    if(value & (1U << (n_bits - 1))) {
        value |= ~0U << n_bits;
    }

    return (int)value;
}

static int32_t ui_read_label_at(FILE * fp, uint32_t absolute_offset, const char * label)
{
    uint32_t length;
    char buf[4];

    if(ui_fs_seek(fp, absolute_offset) != UI_FS_RES_OK) {
        return -1;
    }

    if(ui_fs_read(fp, &length, sizeof(length), NULL) != UI_FS_RES_OK ||
       ui_fs_read(fp, buf, sizeof(buf), NULL) != UI_FS_RES_OK ||
       memcmp(label, buf, sizeof(buf)) != 0) {
        return -1;
    }

    return (int32_t)length;
}

static int32_t ui_read_label(ui_spiffs_font_dsc_t * dsc, uint32_t start, const char * label)
{
    return ui_read_label_at(dsc->file, dsc->table_base_offset + start, label);
}

static bool ui_fs_read_at(FILE * fp, uint32_t offset, void * buf, size_t len)
{
    return ui_fs_seek(fp, offset) == UI_FS_RES_OK && ui_fs_read(fp, buf, len, NULL) == UI_FS_RES_OK;
}

static bool ui_fs_read_u8_at(FILE * fp, uint32_t offset, uint8_t * value)
{
    return ui_fs_read_at(fp, offset, value, sizeof(*value));
}

static bool ui_fs_read_u16_at(FILE * fp, uint32_t offset, uint16_t * value)
{
    return ui_fs_read_at(fp, offset, value, sizeof(*value));
}

static int ui_find_unicode_in_file(FILE * fp, uint32_t list_offset, uint16_t len, uint16_t key)
{
    int left = 0;
    int right = (int)len - 1;

    while(left <= right) {
        int mid = left + ((right - left) / 2);
        uint16_t value = 0;

        if(!ui_fs_read_u16_at(fp, list_offset + (uint32_t)mid * (uint32_t)sizeof(uint16_t), &value)) {
            return -1;
        }

        if(value == key) {
            return mid;
        }

        if(value < key) left = mid + 1;
        else right = mid - 1;
    }

    return -1;
}

static uint32_t ui_get_glyph_id(ui_spiffs_font_dsc_t * dsc, uint32_t letter)
{
    uint16_t i;

    if(letter == '\0') {
        return 0;
    }

    if(letter == dsc->last_letter) {
        return dsc->last_glyph_id;
    }

    for(i = 0; i < dsc->cmap_count; i++) {
        const ui_spiffs_cmap_runtime_t * cmap = &dsc->cmap_records[i];
        uint32_t rcp;
        uint32_t glyph_id = 0;
        uint16_t ofs16 = 0;
        uint8_t ofs8 = 0;

        if(letter < cmap->table.range_start) {
            continue;
        }

        rcp = letter - cmap->table.range_start;
        if(rcp > cmap->table.range_length) {
            continue;
        }

        switch(cmap->table.format_type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                glyph_id = cmap->table.glyph_id_start + rcp;
                break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
                if(ui_fs_read_u8_at(dsc->file, cmap->data_start + rcp, &ofs8)) {
                    glyph_id = cmap->table.glyph_id_start + ofs8;
                }
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
                int found = ui_find_unicode_in_file(dsc->file, cmap->data_start, cmap->table.data_entries_count, (uint16_t)rcp);
                if(found >= 0) {
                    glyph_id = cmap->table.glyph_id_start + (uint32_t)found;
                }
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
                int found = ui_find_unicode_in_file(dsc->file, cmap->data_start, cmap->table.data_entries_count, (uint16_t)rcp);
                uint32_t glyph_ofs_base = cmap->data_start + (uint32_t)cmap->table.data_entries_count * (uint32_t)sizeof(uint16_t);
                if(found >= 0 && ui_fs_read_u16_at(dsc->file,
                                                   glyph_ofs_base + (uint32_t)found * (uint32_t)sizeof(uint16_t),
                                                   &ofs16)) {
                    glyph_id = cmap->table.glyph_id_start + ofs16;
                }
                break;
            }
            default:
                break;
        }

        dsc->last_letter = letter;
        dsc->last_glyph_id = glyph_id;
        return glyph_id;
    }

    dsc->last_letter = letter;
    dsc->last_glyph_id = 0;
    return 0;
}

static ui_spiffs_glyph_cache_entry_t * ui_glyph_cache_find(ui_spiffs_font_dsc_t * dsc, uint32_t unicode_letter)
{
    uint32_t i;

    for(i = 0; i < UI_SPIFFS_FONT_CACHE_SLOTS; i++) {
        ui_spiffs_glyph_cache_entry_t * entry = &dsc->glyph_cache[i];
        if(entry->valid && entry->unicode_letter == unicode_letter) {
            entry->use_count = ++dsc->cache_use_counter;
            return entry;
        }
    }

    return NULL;
}

static ui_spiffs_glyph_cache_entry_t * ui_glyph_cache_get_slot(ui_spiffs_font_dsc_t * dsc)
{
    uint32_t i;
    ui_spiffs_glyph_cache_entry_t * oldest = &dsc->glyph_cache[0];

    for(i = 0; i < UI_SPIFFS_FONT_CACHE_SLOTS; i++) {
        ui_spiffs_glyph_cache_entry_t * entry = &dsc->glyph_cache[i];
        if(!entry->valid) {
            return entry;
        }
        if(entry->use_count < oldest->use_count) {
            oldest = entry;
        }
    }

    if(oldest->bitmap != NULL) {
        lv_mem_free(oldest->bitmap);
        oldest->bitmap = NULL;
    }
    memset(&oldest->gdsc, 0, sizeof(oldest->gdsc));
    oldest->bmp_size = 0;
    oldest->valid = 0;

    return oldest;
}

static bool ui_read_glyph_dsc(ui_spiffs_font_dsc_t * dsc, uint32_t glyph_id, lv_font_fmt_txt_glyph_dsc_t * gdsc_out,
                              uint32_t * bmp_size_out)
{
    ui_fs_res_t res;
    ui_bit_iterator_t bit_it;
    uint32_t offset;
    uint32_t next_offset;
    uint32_t bit_count;
    uint32_t bmp_size;

    if(glyph_id >= dsc->glyph_count) {
        return false;
    }

    if(glyph_id == 0) {
        memset(gdsc_out, 0, sizeof(*gdsc_out));
        if(bmp_size_out != NULL) {
            *bmp_size_out = 0;
        }
        return true;
    }

    if(dsc->header.index_to_loc_format == 0) {
        uint16_t offset16 = 0;
        uint16_t next_offset16 = 0;

        res = ui_fs_seek(dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset16));
        if(res != UI_FS_RES_OK) {
            return false;
        }

        res = ui_fs_read(dsc->file, &offset16, sizeof(offset16), NULL);
        if(res != UI_FS_RES_OK) {
            return false;
        }

        offset = offset16;
        next_offset = dsc->glyf_length;
        if(glyph_id < (dsc->glyph_count - 1)) {
            res = ui_fs_read(dsc->file, &next_offset16, sizeof(next_offset16), NULL);
            if(res != UI_FS_RES_OK) {
                return false;
            }
            next_offset = next_offset16;
        }
    } else if(dsc->header.index_to_loc_format == 1) {
        res = ui_fs_seek(dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset));
        if(res != UI_FS_RES_OK) {
            return false;
        }

        res = ui_fs_read(dsc->file, &offset, sizeof(offset), NULL);
        if(res != UI_FS_RES_OK) {
            return false;
        }

        next_offset = dsc->glyf_length;
        if(glyph_id < (dsc->glyph_count - 1)) {
            res = ui_fs_read(dsc->file, &next_offset, sizeof(next_offset), NULL);
            if(res != UI_FS_RES_OK) {
                return false;
            }
        }
    } else {
        return false;
    }

    res = ui_fs_seek(dsc->file, dsc->glyf_start + offset);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    bit_it = ui_bit_iterator_init(dsc->file);

    if(dsc->header.advance_width_bits == 0) {
        gdsc_out->adv_w = dsc->header.default_advance_width;
    } else {
        gdsc_out->adv_w = ui_read_bits(&bit_it, dsc->header.advance_width_bits, &res);
        if(res != UI_FS_RES_OK) {
            return false;
        }
    }

    if(dsc->header.advance_width_format == 0) {
        gdsc_out->adv_w *= 16;
    }

    gdsc_out->ofs_x = (int8_t)ui_read_bits_signed(&bit_it, dsc->header.xy_bits, &res);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    gdsc_out->ofs_y = (int8_t)ui_read_bits_signed(&bit_it, dsc->header.xy_bits, &res);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    gdsc_out->box_w = (uint8_t)ui_read_bits(&bit_it, dsc->header.wh_bits, &res);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    gdsc_out->box_h = (uint8_t)ui_read_bits(&bit_it, dsc->header.wh_bits, &res);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    gdsc_out->bitmap_index = 0;

    bit_count = dsc->header.advance_width_bits + 2U * dsc->header.xy_bits + 2U * dsc->header.wh_bits;
    bmp_size = next_offset - offset - (bit_count / 8U);

    if(bmp_size_out != NULL) {
        *bmp_size_out = bmp_size;
    }

    return true;
}

static bool ui_read_glyph_bitmap_data(ui_spiffs_font_dsc_t * dsc, uint32_t glyph_id, uint32_t bmp_size, uint8_t * bitmap_out)
{
    ui_fs_res_t res;
    ui_bit_iterator_t bit_it;
    uint32_t bit_count;
    uint32_t glyph_offset;
    uint32_t i;

    if(bmp_size == 0U || bitmap_out == NULL) {
        return true;
    }

    if(dsc->header.index_to_loc_format == 0) {
        uint16_t offset16 = 0;

        res = ui_fs_seek(dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset16));
        if(res != UI_FS_RES_OK) {
            return false;
        }

        res = ui_fs_read(dsc->file, &offset16, sizeof(offset16), NULL);
        if(res != UI_FS_RES_OK) {
            return false;
        }

        glyph_offset = offset16;
    } else if(dsc->header.index_to_loc_format == 1) {
        res = ui_fs_seek(dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(glyph_offset));
        if(res != UI_FS_RES_OK) {
            return false;
        }

        res = ui_fs_read(dsc->file, &glyph_offset, sizeof(glyph_offset), NULL);
        if(res != UI_FS_RES_OK) {
            return false;
        }
    } else {
        return false;
    }

    res = ui_fs_seek(dsc->file, dsc->glyf_start + glyph_offset);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    bit_it = ui_bit_iterator_init(dsc->file);
    bit_count = dsc->header.advance_width_bits + 2U * dsc->header.xy_bits + 2U * dsc->header.wh_bits;

    ui_read_bits(&bit_it, (int)bit_count, &res);
    if(res != UI_FS_RES_OK) {
        return false;
    }

    if((bit_count % 8U) == 0U) {
        if(ui_fs_read(dsc->file, bitmap_out, bmp_size, NULL) != UI_FS_RES_OK) {
            return false;
        }
    } else {
        for(i = 0; i + 1U < bmp_size; i++) {
            bitmap_out[i] = (uint8_t)ui_read_bits(&bit_it, 8, &res);
            if(res != UI_FS_RES_OK) {
                return false;
            }
        }

        bitmap_out[bmp_size - 1U] = (uint8_t)ui_read_bits(&bit_it, 8 - (int)(bit_count % 8U), &res);
        if(res != UI_FS_RES_OK) {
            return false;
        }

        bitmap_out[bmp_size - 1U] <<= (bit_count % 8U);
    }

    return true;
}

static ui_spiffs_glyph_cache_entry_t * ui_glyph_cache_load(ui_spiffs_font_dsc_t * dsc, uint32_t unicode_letter)
{
    ui_spiffs_glyph_cache_entry_t * entry;
    uint32_t glyph_id;
    uint32_t bmp_size = 0;

    entry = ui_glyph_cache_find(dsc, unicode_letter);
    if(entry != NULL) {
        return entry;
    }

    glyph_id = ui_get_glyph_id(dsc, unicode_letter);
    if(glyph_id == 0) {
        return NULL;
    }

    entry = ui_glyph_cache_get_slot(dsc);
    if(entry == NULL) {
        return NULL;
    }

    if(!ui_read_glyph_dsc(dsc, glyph_id, &entry->gdsc, &bmp_size)) {
        return NULL;
    }

    entry->glyph_id = glyph_id;
    entry->unicode_letter = unicode_letter;
    entry->bmp_size = bmp_size;
    entry->use_count = ++dsc->cache_use_counter;

    if((uint32_t)entry->gdsc.box_w * (uint32_t)entry->gdsc.box_h != 0U && bmp_size != 0U) {
        uint8_t * new_bitmap = lv_mem_realloc(entry->bitmap, bmp_size);
        if(new_bitmap == NULL) {
            entry->bmp_size = 0;
            return NULL;
        }
        entry->bitmap = new_bitmap;
        if(!ui_read_glyph_bitmap_data(dsc, glyph_id, bmp_size, entry->bitmap)) {
            return NULL;
        }
    } else if(entry->bitmap != NULL) {
        lv_mem_free(entry->bitmap);
        entry->bitmap = NULL;
    }

    entry->valid = 1;
    return entry;
}

static const uint8_t * ui_spiffs_font_get_bitmap(const lv_font_t * font, uint32_t unicode_letter)
{
    ui_spiffs_font_dsc_t * dsc = (ui_spiffs_font_dsc_t *)font->dsc;
    ui_spiffs_glyph_cache_entry_t * entry;

    if(unicode_letter == '\t') {
        unicode_letter = ' ';
    }

    entry = ui_glyph_cache_load(dsc, unicode_letter);
    if(entry == NULL || entry->bmp_size == 0U) {
        return NULL;
    }

    dsc->last_bitmap_glyph_id = entry->glyph_id;
    return entry->bitmap;
}

static bool ui_spiffs_font_get_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * dsc_out,
                                         uint32_t unicode_letter, uint32_t unicode_letter_next)
{
    ui_spiffs_font_dsc_t * dsc = (ui_spiffs_font_dsc_t *)font->dsc;
    ui_spiffs_glyph_cache_entry_t * entry;
    bool is_tab = false;

    LV_UNUSED(unicode_letter_next);

    if(unicode_letter == '\t') {
        unicode_letter = ' ';
        is_tab = true;
    }

    entry = ui_glyph_cache_load(dsc, unicode_letter);
    if(entry == NULL) {
        return false;
    }

    dsc_out->adv_w = (entry->gdsc.adv_w + 8U) >> 4;
    dsc_out->box_h = entry->gdsc.box_h;
    dsc_out->box_w = entry->gdsc.box_w;
    dsc_out->ofs_x = entry->gdsc.ofs_x;
    dsc_out->ofs_y = entry->gdsc.ofs_y;
    dsc_out->bpp = dsc->header.bits_per_pixel;
    dsc_out->is_placeholder = false;

    if(is_tab) {
        dsc_out->adv_w *= 2;
        dsc_out->box_w *= 2;
    }

    return true;
}

static void ui_spiffs_font_release_cmaps(ui_spiffs_font_dsc_t * dsc)
{
    if(dsc->cmap_records == NULL) {
        return;
    }

    lv_mem_free(dsc->cmap_records);
    dsc->cmap_records = NULL;
}

static void ui_spiffs_font_release_glyph_cache(ui_spiffs_font_dsc_t * dsc)
{
    uint32_t i;

    for(i = 0; i < UI_SPIFFS_FONT_CACHE_SLOTS; i++) {
        if(dsc->glyph_cache[i].bitmap != NULL) {
            lv_mem_free(dsc->glyph_cache[i].bitmap);
            dsc->glyph_cache[i].bitmap = NULL;
        }
        dsc->glyph_cache[i].valid = 0;
    }
}

lv_font_t * ui_spiffs_font_load(const char * path)
{
    lv_font_t * font = NULL;
    ui_spiffs_font_dsc_t * dsc = NULL;
    ui_spiffs_cmap_table_t * tables = NULL;
    size_t heap_start = esp_get_free_heap_size();
    uint32_t cmaps_count = 0;
    uint32_t i;
    int32_t header_length;
    int32_t cmaps_length;
    int32_t loca_length;
    int32_t glyf_length;

    font = lv_mem_alloc(sizeof(lv_font_t));
    dsc = lv_mem_alloc(sizeof(ui_spiffs_font_dsc_t));
    if(font == NULL || dsc == NULL) {
        goto fail;
    }

    ui_log_font_heap("alloc font structs", heap_start);

    memset(font, 0, sizeof(*font));
    memset(dsc, 0, sizeof(*dsc));

    if(ui_fs_open(&dsc->file, path) != UI_FS_RES_OK) {
        ESP_LOGW(TAG, "Open font file failed: %s", path);
        goto fail;
    }

    ui_log_font_heap("open file", heap_start);

    {
        uint8_t probe[12];
        uint32_t probe_len = 0;

        if(ui_fs_read(dsc->file, probe, sizeof(probe), &probe_len) != UI_FS_RES_OK) {
            ESP_LOGW(TAG, "Read font probe failed: %s", path);
            goto fail;
        }
        if(probe_len != sizeof(probe)) {
            ESP_LOGW(TAG, "Short font probe read: %s", path);
            goto fail;
        }

        if(ui_fs_seek(dsc->file, 0) != UI_FS_RES_OK) {
            ESP_LOGW(TAG, "Seek font probe reset failed: %s", path);
            goto fail;
        }

        if(memcmp(&probe[4], "head", 4) == 0) {
            dsc->table_base_offset = 0;
        } else if(memcmp(&probe[8], "head", 4) == 0) {
            dsc->table_base_offset = 4;
        } else {
            ESP_LOGW(TAG, "Invalid font header: %s", path);
            goto fail;
        }
    }

    header_length = ui_read_label(dsc, 0, "head");
    if(header_length < 0) {
        ESP_LOGW(TAG, "Read head label failed: %s base=%u", path, (unsigned)dsc->table_base_offset);
        goto fail;
    }

    if(ui_fs_read(dsc->file, &dsc->header, sizeof(dsc->header), NULL) != UI_FS_RES_OK) {
        ESP_LOGW(TAG, "Read head body failed: %s", path);
        goto fail;
    }

    cmaps_length = ui_read_label(dsc, (uint32_t)header_length, "cmap");
    if(cmaps_length < 0) {
        ESP_LOGW(TAG, "Read cmap label failed: %s header_len=%d", path, (int)header_length);
        goto fail;
    }

    if(ui_fs_read(dsc->file, &cmaps_count, sizeof(cmaps_count), NULL) != UI_FS_RES_OK) {
        ESP_LOGW(TAG, "Read cmap count failed: %s", path);
        goto fail;
    }

    dsc->cmap_records = lv_mem_alloc(sizeof(ui_spiffs_cmap_runtime_t) * cmaps_count);
    tables = lv_mem_alloc(sizeof(ui_spiffs_cmap_table_t) * cmaps_count);
    if(dsc->cmap_records == NULL || tables == NULL) {
        goto fail;
    }

    memset(dsc->cmap_records, 0, sizeof(ui_spiffs_cmap_runtime_t) * cmaps_count);
    dsc->cmap_count = cmaps_count;

    ui_log_font_heap("alloc cmap headers", heap_start);

    if(ui_fs_read(dsc->file, tables, sizeof(ui_spiffs_cmap_table_t) * cmaps_count, NULL) != UI_FS_RES_OK) {
        ESP_LOGW(TAG, "Read cmap tables failed: %s count=%u", path, (unsigned)cmaps_count);
        goto fail;
    }

    for(i = 0; i < cmaps_count; i++) {
        dsc->cmap_records[i].table = tables[i];
        dsc->cmap_records[i].data_start = dsc->table_base_offset + (uint32_t)header_length + tables[i].data_offset;
    }

    lv_mem_free(tables);
    tables = NULL;
    ui_log_font_heap("load cmap data", heap_start);

    loca_length = ui_read_label(dsc, (uint32_t)header_length + (uint32_t)cmaps_length, "loca");
    if(loca_length < 0) {
        ESP_LOGW(TAG, "Read loca label failed: %s", path);
        goto fail;
    }

    if(ui_fs_read(dsc->file, &dsc->glyph_count, sizeof(dsc->glyph_count), NULL) != UI_FS_RES_OK) {
        ESP_LOGW(TAG, "Read loca count failed: %s", path);
        goto fail;
    }

    /* `loca` stores: length(4) + tag(4) + glyph_count(4) + offsets... */
    dsc->loca_start = dsc->table_base_offset + (uint32_t)header_length + (uint32_t)cmaps_length + 8U + sizeof(dsc->glyph_count);

    if(dsc->header.index_to_loc_format != 0 && dsc->header.index_to_loc_format != 1) {
        ESP_LOGW(TAG, "Unsupported loca format: %u", (unsigned)dsc->header.index_to_loc_format);
        goto fail;
    }

    ui_log_font_heap("loca on-demand", heap_start);

    dsc->glyf_start = dsc->table_base_offset + (uint32_t)header_length + (uint32_t)cmaps_length + (uint32_t)loca_length;
    glyf_length = ui_read_label_at(dsc->file, dsc->glyf_start, "glyf");
    if(glyf_length < 0) {
        ESP_LOGW(TAG, "Read glyf label failed: %s start=%u", path, (unsigned)dsc->glyf_start);
        goto fail;
    }
    dsc->glyf_length = (uint32_t)glyf_length;

    font->dsc = dsc;
    font->get_glyph_dsc = ui_spiffs_font_get_glyph_dsc;
    font->get_glyph_bitmap = ui_spiffs_font_get_bitmap;
    font->line_height = dsc->header.ascent - dsc->header.descent;
    font->base_line = -dsc->header.descent;
    font->subpx = dsc->header.subpixels_mode;
    font->underline_position = dsc->header.underline_position;
    font->underline_thickness = dsc->header.underline_thickness;

    ui_log_font_heap("font ready", heap_start);
    ESP_LOGI(TAG, "SPIFFS font loaded with lazy glyph reads: %s", path);
    return font;

fail:
    if(tables != NULL) {
        lv_mem_free(tables);
    }
    if(dsc != NULL) {
        ui_spiffs_font_release_cmaps(dsc);
        ui_spiffs_font_release_glyph_cache(dsc);
        if(dsc->bitmap_cache != NULL) {
            lv_mem_free(dsc->bitmap_cache);
        }
        if(dsc->file != NULL) {
            ui_fs_close(dsc->file);
        }
        lv_mem_free(dsc);
    }
    if(font != NULL) {
        lv_mem_free(font);
    }
    return NULL;
}

void ui_spiffs_font_free(lv_font_t * font)
{
    ui_spiffs_font_dsc_t * dsc;

    if(font == NULL) {
        return;
    }

    dsc = (ui_spiffs_font_dsc_t *)font->dsc;
    if(dsc != NULL) {
        ui_spiffs_font_release_cmaps(dsc);
        ui_spiffs_font_release_glyph_cache(dsc);
        if(dsc->bitmap_cache != NULL) {
            lv_mem_free(dsc->bitmap_cache);
        }
        if(dsc->file != NULL) {
            ui_fs_close(dsc->file);
        }
        lv_mem_free(dsc);
    }

    lv_mem_free(font);
}
