#include "ui_spiffs_font.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"

#include "../GUI/src/font/lv_font_fmt_txt.h"
#include "../GUI/src/misc/lv_fs.h"
#include "../GUI/src/misc/lv_mem.h"

#define UI_SPIFFS_FONT_CACHE_SLOTS 32U

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
    uint8_t valid;
    uint32_t unicode_letter;
    uint32_t glyph_id;
    uint32_t bmp_size;
    uint32_t use_count;
    lv_font_fmt_txt_glyph_dsc_t gdsc;
    uint8_t * bitmap;
} ui_spiffs_glyph_cache_entry_t;

typedef struct {
    lv_fs_file_t file;
    ui_spiffs_font_header_t header;
    lv_font_fmt_txt_cmap_t * cmaps;
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
    lv_fs_file_t * fp;
    int8_t bit_pos;
    uint8_t byte_value;
} ui_bit_iterator_t;

static const char * TAG = "ui_font_spiffs";

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

static ui_bit_iterator_t ui_bit_iterator_init(lv_fs_file_t * fp)
{
    ui_bit_iterator_t it;
    it.fp = fp;
    it.bit_pos = -1;
    it.byte_value = 0;
    return it;
}

static unsigned int ui_read_bits(ui_bit_iterator_t * it, int n_bits, lv_fs_res_t * res)
{
    unsigned int value = 0;

    while(n_bits--) {
        it->byte_value <<= 1;
        it->bit_pos--;

        if(it->bit_pos < 0) {
            it->bit_pos = 7;
            *res = lv_fs_read(it->fp, &(it->byte_value), 1, NULL);
            if(*res != LV_FS_RES_OK) {
                return 0;
            }
        }

        value |= ((it->byte_value & 0x80U) ? 1U : 0U) << n_bits;
    }

    *res = LV_FS_RES_OK;
    return value;
}

static int ui_read_bits_signed(ui_bit_iterator_t * it, int n_bits, lv_fs_res_t * res)
{
    unsigned int value = ui_read_bits(it, n_bits, res);

    if(value & (1U << (n_bits - 1))) {
        value |= ~0U << n_bits;
    }

    return (int)value;
}

static int32_t ui_read_label_at(lv_fs_file_t * fp, uint32_t absolute_offset, const char * label)
{
    uint32_t length;
    char buf[4];

    if(lv_fs_seek(fp, absolute_offset, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
        return -1;
    }

    if(lv_fs_read(fp, &length, sizeof(length), NULL) != LV_FS_RES_OK ||
       lv_fs_read(fp, buf, sizeof(buf), NULL) != LV_FS_RES_OK ||
       memcmp(label, buf, sizeof(buf)) != 0) {
        return -1;
    }

    return (int32_t)length;
}

static int32_t ui_read_label(ui_spiffs_font_dsc_t * dsc, uint32_t start, const char * label)
{
    return ui_read_label_at(&dsc->file, dsc->table_base_offset + start, label);
}

static int ui_find_unicode(const uint16_t * list, uint16_t len, uint16_t key)
{
    int left = 0;
    int right = (int)len - 1;

    while(left <= right) {
        int mid = left + ((right - left) / 2);
        uint16_t value = list[mid];

        if(value == key) {
            return mid;
        }

        if(value < key) {
            left = mid + 1;
        } else {
            right = mid - 1;
        }
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
        const lv_font_fmt_txt_cmap_t * cmap = &dsc->cmaps[i];
        uint32_t rcp;
        uint32_t glyph_id = 0;

        if(letter < cmap->range_start) {
            continue;
        }

        rcp = letter - cmap->range_start;
        if(rcp > cmap->range_length) {
            continue;
        }

        switch(cmap->type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                glyph_id = cmap->glyph_id_start + rcp;
                break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
                glyph_id = cmap->glyph_id_start + ((const uint8_t *)cmap->glyph_id_ofs_list)[rcp];
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
                int found = ui_find_unicode(cmap->unicode_list, cmap->list_length, (uint16_t)rcp);
                if(found >= 0) {
                    glyph_id = cmap->glyph_id_start + (uint32_t)found;
                }
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
                int found = ui_find_unicode(cmap->unicode_list, cmap->list_length, (uint16_t)rcp);
                if(found >= 0) {
                    glyph_id = cmap->glyph_id_start + ((const uint16_t *)cmap->glyph_id_ofs_list)[found];
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
    lv_fs_res_t res;
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

        res = lv_fs_seek(&dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset16), LV_FS_SEEK_SET);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        res = lv_fs_read(&dsc->file, &offset16, sizeof(offset16), NULL);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        offset = offset16;
        next_offset = dsc->glyf_length;
        if(glyph_id < (dsc->glyph_count - 1)) {
            res = lv_fs_read(&dsc->file, &next_offset16, sizeof(next_offset16), NULL);
            if(res != LV_FS_RES_OK) {
                return false;
            }
            next_offset = next_offset16;
        }
    } else if(dsc->header.index_to_loc_format == 1) {
        res = lv_fs_seek(&dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset), LV_FS_SEEK_SET);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        res = lv_fs_read(&dsc->file, &offset, sizeof(offset), NULL);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        next_offset = dsc->glyf_length;
        if(glyph_id < (dsc->glyph_count - 1)) {
            res = lv_fs_read(&dsc->file, &next_offset, sizeof(next_offset), NULL);
            if(res != LV_FS_RES_OK) {
                return false;
            }
        }
    } else {
        return false;
    }

    res = lv_fs_seek(&dsc->file, dsc->glyf_start + offset, LV_FS_SEEK_SET);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    bit_it = ui_bit_iterator_init(&dsc->file);

    if(dsc->header.advance_width_bits == 0) {
        gdsc_out->adv_w = dsc->header.default_advance_width;
    } else {
        gdsc_out->adv_w = ui_read_bits(&bit_it, dsc->header.advance_width_bits, &res);
        if(res != LV_FS_RES_OK) {
            return false;
        }
    }

    if(dsc->header.advance_width_format == 0) {
        gdsc_out->adv_w *= 16;
    }

    gdsc_out->ofs_x = (int8_t)ui_read_bits_signed(&bit_it, dsc->header.xy_bits, &res);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    gdsc_out->ofs_y = (int8_t)ui_read_bits_signed(&bit_it, dsc->header.xy_bits, &res);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    gdsc_out->box_w = (uint8_t)ui_read_bits(&bit_it, dsc->header.wh_bits, &res);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    gdsc_out->box_h = (uint8_t)ui_read_bits(&bit_it, dsc->header.wh_bits, &res);
    if(res != LV_FS_RES_OK) {
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
    lv_fs_res_t res;
    ui_bit_iterator_t bit_it;
    uint32_t bit_count;
    uint32_t glyph_offset;
    uint32_t i;

    if(bmp_size == 0U || bitmap_out == NULL) {
        return true;
    }

    if(dsc->header.index_to_loc_format == 0) {
        uint16_t offset16 = 0;

        res = lv_fs_seek(&dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(offset16), LV_FS_SEEK_SET);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        res = lv_fs_read(&dsc->file, &offset16, sizeof(offset16), NULL);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        glyph_offset = offset16;
    } else if(dsc->header.index_to_loc_format == 1) {
        res = lv_fs_seek(&dsc->file, dsc->loca_start + glyph_id * (uint32_t)sizeof(glyph_offset), LV_FS_SEEK_SET);
        if(res != LV_FS_RES_OK) {
            return false;
        }

        res = lv_fs_read(&dsc->file, &glyph_offset, sizeof(glyph_offset), NULL);
        if(res != LV_FS_RES_OK) {
            return false;
        }
    } else {
        return false;
    }

    res = lv_fs_seek(&dsc->file, dsc->glyf_start + glyph_offset, LV_FS_SEEK_SET);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    bit_it = ui_bit_iterator_init(&dsc->file);
    bit_count = dsc->header.advance_width_bits + 2U * dsc->header.xy_bits + 2U * dsc->header.wh_bits;

    ui_read_bits(&bit_it, (int)bit_count, &res);
    if(res != LV_FS_RES_OK) {
        return false;
    }

    if((bit_count % 8U) == 0U) {
        if(lv_fs_read(&dsc->file, bitmap_out, bmp_size, NULL) != LV_FS_RES_OK) {
            return false;
        }
    } else {
        for(i = 0; i + 1U < bmp_size; i++) {
            bitmap_out[i] = (uint8_t)ui_read_bits(&bit_it, 8, &res);
            if(res != LV_FS_RES_OK) {
                return false;
            }
        }

        bitmap_out[bmp_size - 1U] = (uint8_t)ui_read_bits(&bit_it, 8 - (int)(bit_count % 8U), &res);
        if(res != LV_FS_RES_OK) {
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
    uint32_t i;

    if(dsc->cmaps == NULL) {
        return;
    }

    for(i = 0; i < dsc->cmap_count; i++) {
        if(dsc->cmaps[i].glyph_id_ofs_list != NULL) {
            lv_mem_free((void *)dsc->cmaps[i].glyph_id_ofs_list);
        }

        if(dsc->cmaps[i].unicode_list != NULL) {
            lv_mem_free((void *)dsc->cmaps[i].unicode_list);
        }
    }

    lv_mem_free(dsc->cmaps);
    dsc->cmaps = NULL;
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

    if(lv_fs_open(&dsc->file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "Open font file failed: %s", path);
        goto fail;
    }

    ui_log_font_heap("open file", heap_start);

    {
        uint8_t probe[12];
        uint32_t probe_len = 0;

        if(lv_fs_read(&dsc->file, probe, sizeof(probe), &probe_len) != LV_FS_RES_OK) {
            ESP_LOGW(TAG, "Read font probe failed: %s", path);
            goto fail;
        }
        if(probe_len != sizeof(probe)) {
            ESP_LOGW(TAG, "Short font probe read: %s", path);
            goto fail;
        }

        if(lv_fs_seek(&dsc->file, 0, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
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

    if(lv_fs_read(&dsc->file, &dsc->header, sizeof(dsc->header), NULL) != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "Read head body failed: %s", path);
        goto fail;
    }

    cmaps_length = ui_read_label(dsc, (uint32_t)header_length, "cmap");
    if(cmaps_length < 0) {
        ESP_LOGW(TAG, "Read cmap label failed: %s header_len=%d", path, (int)header_length);
        goto fail;
    }

    if(lv_fs_read(&dsc->file, &cmaps_count, sizeof(cmaps_count), NULL) != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "Read cmap count failed: %s", path);
        goto fail;
    }

    dsc->cmaps = lv_mem_alloc(sizeof(lv_font_fmt_txt_cmap_t) * cmaps_count);
    tables = lv_mem_alloc(sizeof(ui_spiffs_cmap_table_t) * cmaps_count);
    if(dsc->cmaps == NULL || tables == NULL) {
        goto fail;
    }

    memset(dsc->cmaps, 0, sizeof(lv_font_fmt_txt_cmap_t) * cmaps_count);
    dsc->cmap_count = cmaps_count;

    ui_log_font_heap("alloc cmap headers", heap_start);

    if(lv_fs_read(&dsc->file, tables, sizeof(ui_spiffs_cmap_table_t) * cmaps_count, NULL) != LV_FS_RES_OK) {
        ESP_LOGW(TAG, "Read cmap tables failed: %s count=%u", path, (unsigned)cmaps_count);
        goto fail;
    }

    for(i = 0; i < cmaps_count; i++) {
        lv_font_fmt_txt_cmap_t * cmap = &dsc->cmaps[i];

        cmap->range_start = tables[i].range_start;
        cmap->range_length = tables[i].range_length;
        cmap->glyph_id_start = tables[i].glyph_id_start;
        cmap->type = tables[i].format_type;

        if(lv_fs_seek(&dsc->file, dsc->table_base_offset + (uint32_t)header_length + tables[i].data_offset,
                      LV_FS_SEEK_SET) != LV_FS_RES_OK) {
            ESP_LOGW(TAG, "Seek cmap data failed: %s idx=%u ofs=%u", path, (unsigned)i, (unsigned)tables[i].data_offset);
            goto fail;
        }

        switch(tables[i].format_type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
                uint8_t * glyph_id_ofs = lv_mem_alloc(tables[i].data_entries_count);
                if(glyph_id_ofs == NULL) {
                    goto fail;
                }
                cmap->glyph_id_ofs_list = glyph_id_ofs;
                cmap->list_length = tables[i].range_length;
                if(lv_fs_read(&dsc->file, glyph_id_ofs, tables[i].data_entries_count, NULL) != LV_FS_RES_OK) {
                    ESP_LOGW(TAG, "Read cmap full data failed: %s idx=%u", path, (unsigned)i);
                    goto fail;
                }
                break;
            }
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
                uint32_t list_bytes = (uint32_t)tables[i].data_entries_count * sizeof(uint16_t);
                uint16_t * unicode_list = lv_mem_alloc(list_bytes);
                if(unicode_list == NULL) {
                    goto fail;
                }
                cmap->unicode_list = unicode_list;
                cmap->list_length = tables[i].data_entries_count;
                if(lv_fs_read(&dsc->file, unicode_list, list_bytes, NULL) != LV_FS_RES_OK) {
                    ESP_LOGW(TAG, "Read cmap unicode list failed: %s idx=%u", path, (unsigned)i);
                    goto fail;
                }

                if(tables[i].format_type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
                    uint16_t * glyph_id_ofs = lv_mem_alloc(list_bytes);
                    if(glyph_id_ofs == NULL) {
                        goto fail;
                    }
                    cmap->glyph_id_ofs_list = glyph_id_ofs;
                    if(lv_fs_read(&dsc->file, glyph_id_ofs, list_bytes, NULL) != LV_FS_RES_OK) {
                        ESP_LOGW(TAG, "Read cmap sparse glyph ids failed: %s idx=%u", path, (unsigned)i);
                        goto fail;
                    }
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "Unsupported cmap type: %u", (unsigned)tables[i].format_type);
                goto fail;
        }
    }

    lv_mem_free(tables);
    tables = NULL;
    ui_log_font_heap("load cmap data", heap_start);

    loca_length = ui_read_label(dsc, (uint32_t)header_length + (uint32_t)cmaps_length, "loca");
    if(loca_length < 0) {
        ESP_LOGW(TAG, "Read loca label failed: %s", path);
        goto fail;
    }

    if(lv_fs_read(&dsc->file, &dsc->glyph_count, sizeof(dsc->glyph_count), NULL) != LV_FS_RES_OK) {
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
    glyf_length = ui_read_label_at(&dsc->file, dsc->glyf_start, "glyf");
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
        if(dsc->file.drv != NULL) {
            lv_fs_close(&dsc->file);
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
        if(dsc->file.drv != NULL) {
            lv_fs_close(&dsc->file);
        }
        lv_mem_free(dsc);
    }

    lv_mem_free(font);
}
