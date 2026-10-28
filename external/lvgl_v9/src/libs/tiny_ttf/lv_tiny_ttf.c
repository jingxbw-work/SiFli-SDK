/**
* @file lv_tiny_ttf.c
*
*/

/*********************
 *      INCLUDES
 *********************/
#include "../../lvgl.h"
#include <limits.h>

#if LV_USE_TINY_TTF != 0
#include "../../core/lv_global.h"

#define font_draw_buf_handlers &(LV_GLOBAL_DEFAULT()->font_draw_buf_handlers)

/*********************
 *      DEFINES
 *********************/

#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_GLYF_ONLY
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#define STBTT_malloc(x, u) ttf_stb_alloc(x, u)
#define STBTT_free(x, u) ((void)(u), lv_free(x))
static void * ttf_stb_alloc(size_t size, void * userdata);

#if LV_TINY_TTF_FILE_SUPPORT != 0
/* for stream support */
#define STBTT_STREAM_TYPE ttf_cb_stream_t *
#define STBTT_STREAM_SEEK(s, x) ttf_cb_stream_seek(s, x);
#define STBTT_STREAM_READ(s, x, y) ttf_cb_stream_read(s, x, y);
#define STBTT_STREAM_ERROR(s) ((s)->error != LV_TINY_TTF_ERROR_NONE)
#define STBTT_STREAM_FAIL(s) ttf_cb_stream_fail(s, LV_TINY_TTF_ERROR_INVALID_DATA)
#define STBTT_USERDATA_ERROR(u) ((u) && ((ttf_cb_stream_t *)(u))->error != LV_TINY_TTF_ERROR_NONE)

/* a hydra stream that can be in memory or from a file*/
typedef struct ttf_cb_stream {
    lv_fs_file_t * file;
    const void * data;
    size_t size;
    size_t position;
    uint32_t operations;
    lv_tiny_ttf_error_t error;
} ttf_cb_stream_t;

static void ttf_cb_stream_read(ttf_cb_stream_t * stream, void * data, size_t to_read);
static void ttf_cb_stream_seek(ttf_cb_stream_t * stream, size_t position);
static void ttf_cb_stream_fail(ttf_cb_stream_t * stream, lv_tiny_ttf_error_t error);
#endif

#include "stb_rect_pack.h"
#include "stb_truetype_htcw.h"

/**********************
 *      TYPEDEFS
 **********************/

typedef struct ttf_font_desc {
    lv_cache_t * glyph_cache;
    lv_cache_t * draw_data_cache;
    lv_cache_t * kerning_cache;
    stbtt_fontinfo info;
    lv_fs_file_t file;
#if LV_TINY_TTF_FILE_SUPPORT != 0
    ttf_cb_stream_t stream;
#else
    const uint8_t * stream;
#endif
    float scale;
    int ascent;
    int descent;
    int cache_size;
    lv_font_kerning_t kerning;
} ttf_font_desc_t;

typedef struct _tiny_ttf_glyph_cache_data_t {
    lv_font_glyph_dsc_t glyph_dsc;
    uint32_t unicode;
    int adv_w;
} tiny_ttf_glyph_cache_data_t;

typedef struct  {
    int glyph1_idx;
    int glyph2_idx;
    uint16_t adv_w16;
} tiny_ttf_kerning_cache_data_t;

typedef struct {
    const ttf_font_desc_t * dsc;
    int adv_w;
} tiny_ttf_kerning_cache_create_data_t;

typedef struct _lv_tiny_ttf_cache_data_t {
    lv_draw_buf_t * draw_buf;
    uint32_t glyph_index;
    uint32_t size;
} tiny_ttf_cache_data_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool ttf_get_glyph_dsc_cb(const lv_font_t * font, lv_font_glyph_dsc_t * dsc_out, uint32_t unicode_letter,
                                 uint32_t unicode_letter_next);
static const void * ttf_get_glyph_bitmap_cb(lv_font_glyph_dsc_t * g_dsc, lv_draw_buf_t * draw_buf);
static void ttf_release_glyph_cb(const lv_font_t * font, lv_font_glyph_dsc_t * g_dsc);
static lv_font_t * lv_tiny_ttf_create(const char * path, const void * data, size_t data_size,
                                      int32_t font_size, lv_font_kerning_t kerning,
                                      size_t cache_size);

static bool tiny_ttf_glyph_cache_create_cb(tiny_ttf_glyph_cache_data_t * node, void * user_data);
static void tiny_ttf_glyph_cache_free_cb(tiny_ttf_glyph_cache_data_t * node, void * user_data);
static lv_cache_compare_res_t tiny_ttf_glyph_cache_compare_cb(const tiny_ttf_glyph_cache_data_t * lhs,
                                                              const tiny_ttf_glyph_cache_data_t * rhs);

static bool tiny_ttf_draw_data_cache_create_cb(tiny_ttf_cache_data_t * node, void * user_data);
static void tiny_ttf_draw_data_cache_free_cb(tiny_ttf_cache_data_t * node, void * user_data);
static lv_cache_compare_res_t tiny_ttf_draw_data_cache_compare_cb(const tiny_ttf_cache_data_t * lhs,
                                                                  const tiny_ttf_cache_data_t * rhs);

static bool tiny_ttf_kerning_cache_create_cb(tiny_ttf_kerning_cache_data_t * node, void * user_data);
static void tiny_ttf_kerning_cache_free_cb(tiny_ttf_kerning_cache_data_t * node, void * user_data);
static lv_cache_compare_res_t tiny_ttf_kerning_cache_compare_cb(const tiny_ttf_kerning_cache_data_t * lhs,
                                                                const tiny_ttf_kerning_cache_data_t * rhs);

static bool lv_tiny_ttf_cache_create(ttf_font_desc_t * dsc);

static lv_font_t * tiny_ttf_font_create_cb(const lv_font_info_t * info, const void * src);
static void tiny_ttf_font_delete_cb(lv_font_t * font);
static void * tiny_ttf_font_dup_src_cb(const void * src);
static void tiny_ttf_font_free_src_cb(void * src);

/**********************
 *  GLOBAL VARIABLES
 **********************/

const lv_font_class_t lv_tiny_ttf_font_class = {
    .create_cb = tiny_ttf_font_create_cb,
    .delete_cb = tiny_ttf_font_delete_cb,
    .dup_src_cb = tiny_ttf_font_dup_src_cb,
    .free_src_cb = tiny_ttf_font_free_src_cb,
};

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_tiny_ttf_error_t lv_tiny_ttf_get_error(const lv_font_t * font)
{
    if(!font || !font->dsc || font->get_glyph_dsc != ttf_get_glyph_dsc_cb)
        return LV_TINY_TTF_ERROR_INVALID_DATA;
#if LV_TINY_TTF_FILE_SUPPORT != 0
    return ((const ttf_font_desc_t *)font->dsc)->stream.error;
#else
    return LV_TINY_TTF_ERROR_NONE;
#endif
}

static bool ttf_operation_begin(ttf_font_desc_t * dsc, bool resizing)
{
#if LV_TINY_TTF_FILE_SUPPORT != 0
    if(dsc->stream.error == LV_TINY_TTF_ERROR_MEMORY) {
        /* Only public-operation entry may recover an allocation failure. */
        dsc->stream.error = LV_TINY_TTF_ERROR_NONE;
        if(!resizing && !lv_tiny_ttf_cache_create(dsc)) {
            dsc->stream.error = LV_TINY_TTF_ERROR_MEMORY;
            return false;
        }
    }
    if(dsc->stream.error) return false;
    dsc->stream.operations = 0;
#else
    LV_UNUSED(dsc);
    LV_UNUSED(resizing);
#endif
    return true;
}

static bool ttf_operation_failed(const ttf_font_desc_t * dsc)
{
#if LV_TINY_TTF_FILE_SUPPORT != 0
    return dsc->stream.error != LV_TINY_TTF_ERROR_NONE;
#else
    LV_UNUSED(dsc);
    return false;
#endif
}

void lv_tiny_ttf_set_size(lv_font_t * font, int32_t font_size)
{
    if(font_size <= 0 || font_size > 4096) {
        LV_LOG_ERROR("invalid font size: %"LV_PRIx32, font_size);
        return;
    }
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(!ttf_operation_begin(dsc, true)) return;
    dsc->scale = stbtt_ScaleForMappingEmToPixels(&dsc->info, (float)font_size);
    int line_gap = 0;
    stbtt_GetFontVMetrics(&dsc->info, &dsc->ascent, &dsc->descent, &line_gap);
    if(ttf_operation_failed(dsc)) return;
    font->line_height = (int32_t)(dsc->scale * (dsc->ascent - dsc->descent + line_gap));
    font->base_line = (int32_t)(dsc->scale * (line_gap - dsc->descent));

    /* size change means cache needs to be invalidated. */

    if(dsc->glyph_cache) {
        lv_cache_destroy(dsc->glyph_cache, NULL);
        dsc->glyph_cache = NULL;
    }

    if(dsc->draw_data_cache) {
        lv_cache_destroy(dsc->draw_data_cache, NULL);
        dsc->draw_data_cache = NULL;
    }
    if(dsc->kerning_cache) {
        lv_cache_destroy(dsc->kerning_cache, NULL);
        dsc->kerning_cache = NULL;
    }

    if(!lv_tiny_ttf_cache_create(dsc)) {
#if LV_TINY_TTF_FILE_SUPPORT != 0
        ttf_cb_stream_fail(&dsc->stream, LV_TINY_TTF_ERROR_MEMORY);
#endif
    }
}

void lv_tiny_ttf_destroy(lv_font_t * font)
{
    LV_ASSERT_NULL(font);

    if(font->dsc != NULL) {
        ttf_font_desc_t * ttf = (ttf_font_desc_t *)font->dsc;
#if LV_TINY_TTF_FILE_SUPPORT != 0
        if(ttf->stream.file != NULL) {
            lv_fs_close(&ttf->file);
        }
#endif
        if(ttf->glyph_cache) lv_cache_destroy(ttf->glyph_cache, NULL);
        if(ttf->draw_data_cache) lv_cache_destroy(ttf->draw_data_cache, NULL);
        if(ttf->kerning_cache) lv_cache_destroy(ttf->kerning_cache, NULL);
        lv_free(ttf);
        font->dsc = NULL;
    }

    lv_free(font);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void * ttf_stb_alloc(size_t size, void * userdata)
{
#if LV_TINY_TTF_FILE_SUPPORT != 0
    ttf_cb_stream_t * stream = userdata;
    if(stream && stream->error) return NULL;
#else
    LV_UNUSED(userdata);
#endif
    void * memory = lv_malloc(size);
#if LV_TINY_TTF_FILE_SUPPORT != 0
    if(!memory && stream) ttf_cb_stream_fail(stream, LV_TINY_TTF_ERROR_MEMORY);
#endif
    return memory;
}

#if LV_TINY_TTF_FILE_SUPPORT != 0
static void ttf_cb_stream_fail(ttf_cb_stream_t * stream, lv_tiny_ttf_error_t error)
{
    if(!stream->error) stream->error = error;
}

static void ttf_cb_stream_read(ttf_cb_stream_t * stream, void * data, size_t to_read)
{
    if(stream->error) { memset(data, 0, to_read); return; }
    if(stream->position > stream->size || to_read > stream->size - stream->position ||
       to_read > UINT32_MAX || ++stream->operations > 1048576u) {
        ttf_cb_stream_fail(stream, LV_TINY_TTF_ERROR_INVALID_DATA);
        memset(data, 0, to_read);
        return;
    }
    if(stream->file != NULL) {
        uint32_t br = 0;
        if(lv_fs_read(stream->file, data, (uint32_t)to_read, &br) != LV_FS_RES_OK || br != to_read) {
            ttf_cb_stream_fail(stream, LV_TINY_TTF_ERROR_IO);
            memset(data, 0, to_read);
            return;
        }
    }
    else {
        memcpy(data, ((const unsigned char *)stream->data + stream->position), to_read);
    }
    stream->position += to_read;
}
static void ttf_cb_stream_seek(ttf_cb_stream_t * stream, size_t position)
{
    if(stream->error) return;
    if(position > stream->size || position > INT_MAX) {
        ttf_cb_stream_fail(stream, LV_TINY_TTF_ERROR_INVALID_DATA);
        return;
    }
    if(stream->file != NULL) {
        if(lv_fs_seek(stream->file, (uint32_t)position, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
            ttf_cb_stream_fail(stream, LV_TINY_TTF_ERROR_IO);
            return;
        }
    }
    stream->position = position;
}
#endif

static inline uint16_t ttf_calculate_kerning_width(float scale, uint16_t adv_w, int k)
{

    /*Horizontal space required by the glyph in [px]*/;
    return (uint16_t)(scale * (adv_w + k) + 0.5f);
}

static uint16_t ttf_get_glyph_pair_kerning_width(const ttf_font_desc_t * dsc, uint32_t g1, uint32_t g2, int adv_w)
{
    tiny_ttf_kerning_cache_data_t kerning_cache_search_key = {
        .glyph1_idx = g1,
        .glyph2_idx = g2,
    };

    tiny_ttf_kerning_cache_create_data_t kerning_cache_create_data = {
        .adv_w = adv_w,
        .dsc = dsc,
    };

    if(dsc->kerning_cache->max_size == 0) {
        /* No cache, call the create function directly */
        bool ret = tiny_ttf_kerning_cache_create_cb(&kerning_cache_search_key, (void *)&kerning_cache_create_data);
        if(!ret) return 0;
        return kerning_cache_search_key.adv_w16;
    }

    LV_ASSERT_NULL(dsc->kerning_cache);
    lv_cache_entry_t * kerning_entry = lv_cache_acquire_or_create(dsc->kerning_cache, &kerning_cache_search_key,
                                                                  (void *)&kerning_cache_create_data);
    if(!kerning_entry) return 0;
    tiny_ttf_kerning_cache_data_t * data = lv_cache_entry_get_data(kerning_entry);
    LV_ASSERT_NULL(data);

    uint16_t width = data->adv_w16;
    lv_cache_release(dsc->kerning_cache, kerning_entry, NULL);
    return width;
}

static bool ttf_get_glyph_dsc_cb(const lv_font_t * font, lv_font_glyph_dsc_t * dsc_out, uint32_t unicode_letter,
                                 uint32_t unicode_letter_next)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(!ttf_operation_begin(dsc, false) || !dsc->glyph_cache || !dsc->kerning_cache) return false;
    if(unicode_letter < 0x20 ||
       unicode_letter == 0xf8ff || /*LV_SYMBOL_DUMMY*/
       unicode_letter == 0x200c) { /*ZERO WIDTH NON-JOINER*/
        dsc_out->box_w = 0;
        dsc_out->adv_w = 0;
        dsc_out->box_h = 0; /*height of the bitmap in [px]*/
        dsc_out->ofs_x = 0; /*X offset of the bitmap in [pf]*/
        dsc_out->ofs_y = 0; /*Y offset of the bitmap in [pf]*/
        dsc_out->format = LV_FONT_GLYPH_FORMAT_NONE;
        dsc_out->is_placeholder = false;
        return true;
    }

    tiny_ttf_glyph_cache_data_t search_key = {
        .unicode = unicode_letter,
    };


    int adv_w;
    lv_cache_entry_t * entry = lv_cache_acquire_or_create(dsc->glyph_cache, &search_key, (void *)dsc);

    if(entry == NULL) {
        if(!dsc->cache_size) {  /* no cache, do everything directly */
            int g1 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter);
            if(!tiny_ttf_glyph_cache_create_cb(&search_key, dsc)) return false;
            *dsc_out = search_key.glyph_dsc;
            adv_w = search_key.adv_w;

            /*Kerning correction*/
            if(font->kerning == LV_FONT_KERNING_NORMAL &&
               unicode_letter_next != 0) {
                int g2 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter_next); /* not using cache, only do glyph id lookup */
                if(g2) {
                    dsc_out->adv_w = ttf_get_glyph_pair_kerning_width(dsc, g1, g2, adv_w);
                }
            }

            dsc_out->entry = NULL;
            return !ttf_operation_failed(dsc);
        }
        LV_LOG_ERROR("cache not allocated");
        return false;
    }

    tiny_ttf_glyph_cache_data_t * data = lv_cache_entry_get_data(entry);
    *dsc_out = data->glyph_dsc;
    adv_w = data->adv_w;
    lv_cache_release(dsc->glyph_cache, entry, NULL);

    /*Kerning correction*/
    if(font->kerning == LV_FONT_KERNING_NORMAL &&
       unicode_letter_next != 0) { /* check if we need to do any kerning calculations */
        uint32_t g1 = dsc_out->gid.index;

        int g2 = 0;
        search_key.unicode = unicode_letter_next; /* reuse search key */
        lv_cache_entry_t * entry_next = lv_cache_acquire_or_create(dsc->glyph_cache, &search_key, (void *)dsc);

        if(entry_next == NULL) {
            g2 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter_next);
        }
        else {
            tiny_ttf_glyph_cache_data_t * data_next = lv_cache_entry_get_data(entry_next);
            g2 = data_next->glyph_dsc.gid.index;
            lv_cache_release(dsc->glyph_cache, entry_next, NULL);
        }
        if(g2) {
            dsc_out->adv_w = ttf_get_glyph_pair_kerning_width(dsc, g1, g2, adv_w);
        }
    }

    dsc_out->entry = NULL;
    return !ttf_operation_failed(dsc);
}

static const void * ttf_get_glyph_bitmap_cb(lv_font_glyph_dsc_t * g_dsc, lv_draw_buf_t * draw_buf)
{
    LV_UNUSED(draw_buf);
    uint32_t glyph_index = g_dsc->gid.index;
    const lv_font_t * font = g_dsc->resolved_font;
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(!ttf_operation_begin(dsc, false) || !dsc->draw_data_cache) return NULL;
    tiny_ttf_cache_data_t search_key = {
        .glyph_index = glyph_index,
        .size = font->line_height,
    };

    lv_cache_entry_t * entry = lv_cache_acquire_or_create(dsc->draw_data_cache, &search_key, (void *)font->dsc);
    if(entry == NULL) {
        if(!dsc->cache_size) {  /* no cache, do everything directly */
            if(tiny_ttf_draw_data_cache_create_cb(&search_key, (void *)font->dsc)) {
                /* use the cache entry to store the buffer if no cache specified */
                g_dsc->entry = (lv_cache_entry_t *)search_key.draw_buf;
                return g_dsc->entry;
            }
            else {
                return NULL;
            }
        }
        LV_LOG_ERROR("cache not allocated");
        return NULL;
    }

    g_dsc->entry = entry;
    tiny_ttf_cache_data_t * cached_data = lv_cache_entry_get_data(entry);
    return cached_data->draw_buf;
}

static void ttf_release_glyph_cb(const lv_font_t * font, lv_font_glyph_dsc_t * g_dsc)
{
    LV_ASSERT_NULL(font);

    ttf_font_desc_t * dsc = (ttf_font_desc_t *)font->dsc;
    if(!dsc->cache_size) {  /* no cache, do everything directly */
        lv_draw_buf_destroy((lv_draw_buf_t *)g_dsc->entry);
    }
    else {
        if(g_dsc->entry == NULL) {
            return;
        }
        lv_cache_release(dsc->draw_data_cache, g_dsc->entry, NULL);
    }
    g_dsc->entry = NULL;
}

static bool lv_tiny_ttf_cache_create(ttf_font_desc_t * dsc)
{
    /*Init cache*/
    if(!dsc->glyph_cache) dsc->glyph_cache = lv_cache_create(&lv_cache_class_lru_rb_count, sizeof(tiny_ttf_glyph_cache_data_t), dsc->cache_size,
    (lv_cache_ops_t) {
        .compare_cb = (lv_cache_compare_cb_t)tiny_ttf_glyph_cache_compare_cb,
        .create_cb = (lv_cache_create_cb_t)tiny_ttf_glyph_cache_create_cb,
        .free_cb = (lv_cache_free_cb_t)tiny_ttf_glyph_cache_free_cb
    });
    if(!dsc->glyph_cache) return false;
    lv_cache_set_name(dsc->glyph_cache, "TINY_TTF_GLYPH");

    if(!dsc->draw_data_cache) dsc->draw_data_cache = lv_cache_create(&lv_cache_class_lru_rb_count, sizeof(tiny_ttf_cache_data_t), dsc->cache_size,
    (lv_cache_ops_t) {
        .compare_cb = (lv_cache_compare_cb_t)tiny_ttf_draw_data_cache_compare_cb,
        .create_cb = (lv_cache_create_cb_t)tiny_ttf_draw_data_cache_create_cb,
        .free_cb = (lv_cache_free_cb_t)tiny_ttf_draw_data_cache_free_cb,
    });
    if(!dsc->draw_data_cache) return false;
    lv_cache_set_name(dsc->draw_data_cache, "TINY_TTF_DRAW_DATA");

    if(!dsc->kerning_cache) dsc->kerning_cache = lv_cache_create(&lv_cache_class_lru_rb_count, sizeof(tiny_ttf_kerning_cache_data_t),
                                         LV_TINY_TTF_CACHE_KERNING_CNT,
    (lv_cache_ops_t) {
        .compare_cb = (lv_cache_compare_cb_t)tiny_ttf_kerning_cache_compare_cb,
        .create_cb = (lv_cache_create_cb_t)tiny_ttf_kerning_cache_create_cb,
        .free_cb = (lv_cache_free_cb_t)tiny_ttf_kerning_cache_free_cb,
    });
    if(!dsc->kerning_cache) return false;
    lv_cache_set_name(dsc->kerning_cache, "TINY_TTF_KERNING_DATA");
    return true;
}

static lv_font_t * lv_tiny_ttf_create(const char * path, const void * data, size_t data_size, int32_t font_size,
                                      lv_font_kerning_t kerning, size_t cache_size)
{
    LV_UNUSED(data_size);
    if((path == NULL && data == NULL) || 0 >= font_size || font_size > 4096 || cache_size > INT_MAX) {
        LV_LOG_ERROR("tiny_ttf: invalid argument");
        return NULL;
    }
    ttf_font_desc_t * dsc = lv_malloc_zeroed(sizeof(ttf_font_desc_t));
    if(dsc == NULL) {
        LV_LOG_ERROR("tiny_ttf: out of memory");
        return NULL;
    }
#if LV_TINY_TTF_FILE_SUPPORT != 0
    dsc->info.userdata = &dsc->stream;
    if(path != NULL) {
        if(LV_FS_RES_OK != lv_fs_open(&dsc->file, path, LV_FS_MODE_RD)) {
            lv_free(dsc);
            LV_LOG_ERROR("tiny_ttf: unable to open %s\n", path);
            return NULL;
        }
        dsc->stream.file = &dsc->file;
        uint32_t size = 0;
        if(lv_fs_seek(&dsc->file, 0, LV_FS_SEEK_END) != LV_FS_RES_OK ||
           lv_fs_tell(&dsc->file, &size) != LV_FS_RES_OK || size < 12 || size > INT_MAX ||
           lv_fs_seek(&dsc->file, 0, LV_FS_SEEK_SET) != LV_FS_RES_OK) {
            lv_fs_close(&dsc->file);
            lv_free(dsc);
            return NULL;
        }
        dsc->stream.size = size;
    }
    else {
        dsc->stream.data = (const uint8_t *)data;
        dsc->stream.size = data_size;
    }
    int offset = stbtt_GetFontOffsetForIndex(&dsc->stream, 0);
    if(offset < 0 || dsc->stream.error ||
       0 == stbtt_InitFont(&dsc->info, &dsc->stream, offset) || dsc->stream.error) {
        if(dsc->stream.file) lv_fs_close(&dsc->file);
        lv_free(dsc);
        LV_LOG_ERROR("tiny_ttf: init failed");
        return NULL;
    }

#else
    dsc->stream = (const uint8_t *)data;
    if(0 == stbtt_InitFont(&dsc->info, dsc->stream, stbtt_GetFontOffsetForIndex(dsc->stream, 0))) {
        lv_free(dsc);
        LV_LOG_ERROR("tiny_ttf: init failed");
        return NULL;
    }
#endif

    dsc->cache_size = (int)cache_size;

    lv_font_t * out_font = lv_malloc_zeroed(sizeof(lv_font_t));
    if(out_font == NULL) {
#if LV_TINY_TTF_FILE_SUPPORT != 0
        if(dsc->stream.file) lv_fs_close(&dsc->file);
#endif
        lv_free(dsc);
        LV_LOG_ERROR("tiny_ttf: out of memory");
        return NULL;
    }

    /* check if font  has kerning tables to use, else disable kerning automatically. */
    if(kerning != LV_FONT_KERNING_NONE && stbtt_KernTableCheck(&dsc->info) == 0) {
        /* disable kerning if font has no tables. */
        LV_LOG_INFO("Disabling kerning as font doesn't support it.");
        kerning = LV_FONT_KERNING_NONE;
    }

    dsc->kerning = kerning;
    out_font->kerning = kerning;

    out_font->get_glyph_dsc = ttf_get_glyph_dsc_cb;
    out_font->get_glyph_bitmap = ttf_get_glyph_bitmap_cb;
    out_font->release_glyph = ttf_release_glyph_cb;
    out_font->dsc = dsc;
    if(ttf_operation_failed(dsc)) {
        lv_tiny_ttf_destroy(out_font);
        return NULL;
    }
    lv_tiny_ttf_set_size(out_font, font_size);
    if(ttf_operation_failed(dsc) || !dsc->glyph_cache || !dsc->draw_data_cache || !dsc->kerning_cache) {
        lv_tiny_ttf_destroy(out_font);
        return NULL;
    }
    return out_font;
}
#if LV_TINY_TTF_FILE_SUPPORT != 0
lv_font_t * lv_tiny_ttf_create_file_ex(const char * path, int32_t font_size, lv_font_kerning_t kerning,
                                       size_t cache_size)
{
    return lv_tiny_ttf_create(path, NULL, 0, font_size, kerning, cache_size);
}
lv_font_t * lv_tiny_ttf_create_file(const char * path, int32_t font_size)
{
    return lv_tiny_ttf_create(path, NULL, 0, font_size, LV_FONT_KERNING_NORMAL, LV_TINY_TTF_CACHE_GLYPH_CNT);
}
#endif

lv_font_t * lv_tiny_ttf_create_data_ex(const void * data, size_t data_size, int32_t font_size,
                                       lv_font_kerning_t kerning, size_t cache_size)
{
    return lv_tiny_ttf_create(NULL, data, data_size, font_size, kerning, cache_size);
}
lv_font_t * lv_tiny_ttf_create_data(const void * data, size_t data_size, int32_t font_size)
{
    return lv_tiny_ttf_create(NULL, data, data_size, font_size, LV_FONT_KERNING_NORMAL, LV_TINY_TTF_CACHE_GLYPH_CNT);
}

/*-----------------
 * Cache Callbacks
 *----------------*/

static bool tiny_ttf_glyph_cache_create_cb(tiny_ttf_glyph_cache_data_t * node, void * user_data)
{
    ttf_font_desc_t * dsc = (ttf_font_desc_t *)user_data;
    lv_font_glyph_dsc_t * dsc_out = &node->glyph_dsc;

    uint32_t unicode_letter = node->unicode;

    int g1 = stbtt_FindGlyphIndex(&dsc->info, (int)unicode_letter);
    if(g1 <= 0 || g1 >= dsc->info.numGlyphs || ttf_operation_failed(dsc)) {
        /* Glyph not found */
        return false;
    }
    int x1, y1, x2, y2;

    stbtt_GetGlyphBitmapBox(&dsc->info, g1, dsc->scale, dsc->scale, &x1, &y1, &x2, &y2);
    if(ttf_operation_failed(dsc)) return false;
    if(x2 < x1 || y2 < y1 || x2 - x1 >= 4096 || y2 - y1 >= 4096 ||
       (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1) > 4u * 1024u * 1024u) {
#if LV_TINY_TTF_FILE_SUPPORT != 0
        ttf_cb_stream_fail(&dsc->stream, LV_TINY_TTF_ERROR_INVALID_DATA);
#endif
        return false;
    }

    int advw;
    int lsb;
    stbtt_GetGlyphHMetrics(&dsc->info, g1, &advw, &lsb);
    if(ttf_operation_failed(dsc)) return false;
    if(dsc->kerning != LV_FONT_KERNING_NORMAL) { /* calculate default advance */
        dsc_out->adv_w = ttf_calculate_kerning_width(dsc->scale, advw, 0);
    }
    else {
        dsc_out->adv_w = ttf_calculate_kerning_width(dsc->scale, advw, 0);
    }
    /* precalculate no kerning value */
    node->adv_w = advw;
    dsc_out->box_w = (x2 - x1 + 1);         /*width of the bitmap in [px]*/
    dsc_out->box_h = (y2 - y1 + 1);         /*height of the bitmap in [px]*/
    dsc_out->ofs_x = x1;                    /*X offset of the bitmap in [pf]*/
    dsc_out->ofs_y = -y2;                   /*Y offset of the bitmap measured from the as line*/
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc_out->is_placeholder = false;
    dsc_out->gid.index = (uint32_t)g1;

    return true;
}

static void tiny_ttf_glyph_cache_free_cb(tiny_ttf_glyph_cache_data_t * node, void * user_data)
{
    LV_UNUSED(node);
    LV_UNUSED(user_data);
}

static lv_cache_compare_res_t tiny_ttf_glyph_cache_compare_cb(const tiny_ttf_glyph_cache_data_t * lhs,
                                                              const tiny_ttf_glyph_cache_data_t * rhs)
{
    if(lhs->unicode != rhs->unicode) {
        return lhs->unicode > rhs->unicode ? 1 : -1;
    }

    return 0;
}

static bool tiny_ttf_draw_data_cache_create_cb(tiny_ttf_cache_data_t * node, void * user_data)
{
    int g1 = (int)node->glyph_index;
    if(g1 == 0) {
        /* Glyph not found */
        return false;
    }

    ttf_font_desc_t * dsc = (ttf_font_desc_t *)user_data;

    const stbtt_fontinfo * info = (const stbtt_fontinfo *)&dsc->info;
    int x1, y1, x2, y2;
    stbtt_GetGlyphBitmapBox(info, g1, dsc->scale, dsc->scale, &x1, &y1, &x2, &y2);
    if(ttf_operation_failed(dsc)) return false;
    int w, h;
    w = x2 - x1 + 1;
    h = y2 - y1 + 1;
    if(w <= 0 || h <= 0 || w > 4096 || h > 4096 || (uint32_t)w * h > 4u * 1024u * 1024u) {
#if LV_TINY_TTF_FILE_SUPPORT != 0
        ttf_cb_stream_fail(&dsc->stream, LV_TINY_TTF_ERROR_INVALID_DATA);
#endif
        return false;
    }

    lv_draw_buf_t * draw_buf = lv_draw_buf_create_ex(font_draw_buf_handlers, w, h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if(NULL == draw_buf) {
#if LV_TINY_TTF_FILE_SUPPORT != 0
        ttf_cb_stream_fail(&dsc->stream, LV_TINY_TTF_ERROR_MEMORY);
#endif
        LV_LOG_ERROR("tiny_ttf: out of memory");
        return false;
    }

    lv_draw_buf_clear(draw_buf, NULL);

    uint32_t stride = draw_buf->header.stride;
    stbtt_MakeGlyphBitmap(info, draw_buf->data, w, h, stride, dsc->scale, dsc->scale, g1);
    if(ttf_operation_failed(dsc)) {
        lv_draw_buf_destroy(draw_buf);
        return false;
    }

    lv_draw_buf_flush_cache(draw_buf, NULL);
    node->draw_buf = draw_buf;
    return true;
}

static void tiny_ttf_draw_data_cache_free_cb(tiny_ttf_cache_data_t * node, void * user_data)
{
    LV_UNUSED(user_data);

    lv_draw_buf_destroy(node->draw_buf);
}

static lv_cache_compare_res_t tiny_ttf_draw_data_cache_compare_cb(const tiny_ttf_cache_data_t * lhs,
                                                                  const tiny_ttf_cache_data_t * rhs)
{
    if(lhs->glyph_index != rhs->glyph_index) {
        return lhs->glyph_index > rhs->glyph_index ? 1 : -1;
    }

    if(lhs->size != rhs->size) {
        return lhs->size > rhs->size ? 1 : -1;
    }

    return 0;
}

static bool tiny_ttf_kerning_cache_create_cb(tiny_ttf_kerning_cache_data_t * node, void * user_data)
{
    tiny_ttf_kerning_cache_create_data_t * create_data = (tiny_ttf_kerning_cache_create_data_t *)user_data;
    const ttf_font_desc_t * dsc = create_data->dsc;
    const int adv_w = create_data->adv_w;
    const int k = stbtt_GetGlyphKernAdvance(&dsc->info, node->glyph1_idx, node->glyph2_idx);
    if(ttf_operation_failed(dsc)) return false;
    node->adv_w16 = ttf_calculate_kerning_width(dsc->scale, adv_w, k);
    return true;
}

static void tiny_ttf_kerning_cache_free_cb(tiny_ttf_kerning_cache_data_t * node, void * user_data)
{
    LV_UNUSED(node);
    LV_UNUSED(user_data);
}

static lv_cache_compare_res_t tiny_ttf_kerning_cache_compare_cb(const tiny_ttf_kerning_cache_data_t * lhs,
                                                                const tiny_ttf_kerning_cache_data_t * rhs)
{
    lv_cache_compare_res_t ret = lhs->glyph1_idx - rhs->glyph1_idx;
    if(ret == 0) {
        return lhs->glyph2_idx - rhs->glyph2_idx;
    }
    return ret;
}

static lv_font_t * tiny_ttf_font_create_cb(const lv_font_info_t * info, const void * src)
{
    const lv_tiny_ttf_font_src_t * font_src = src;

    if(font_src->path) {
#if LV_TINY_TTF_FILE_SUPPORT
        if(font_src->cache_size) {
            return lv_tiny_ttf_create_file_ex(font_src->path, info->size, info->kerning, font_src->cache_size);
        }

        return lv_tiny_ttf_create_file(font_src->path, info->size);
#else
        LV_LOG_WARN("LV_TINY_TTF_FILE_SUPPORT not enabled");
        return NULL;
#endif
    }

    if(font_src->cache_size) {
        return lv_tiny_ttf_create_data_ex(font_src->data, font_src->data_size, info->size, info->kerning, font_src->cache_size);
    }

    return lv_tiny_ttf_create_data(font_src->data, font_src->data_size, info->size);
}

static void tiny_ttf_font_delete_cb(lv_font_t * font)
{
    lv_tiny_ttf_destroy(font);
}

static void * tiny_ttf_font_dup_src_cb(const void * src)
{
    const lv_tiny_ttf_font_src_t * font_src = src;

    lv_tiny_ttf_font_src_t * new_src = lv_malloc_zeroed(sizeof(lv_tiny_ttf_font_src_t));
    LV_ASSERT_MALLOC(new_src);
    *new_src = *font_src;

    if(font_src->path) {
        new_src->path = lv_strdup(font_src->path);
    }

    return new_src;
}

static void tiny_ttf_font_free_src_cb(void * src)
{
    lv_tiny_ttf_font_src_t * font_src = src;
    if(font_src->path) {
        lv_free((char *)font_src->path);
        font_src->path = NULL;
    }

    lv_free(font_src);
}

#endif
