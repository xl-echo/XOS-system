/* ============================================================================
 * XOS 字体渲染（第 23 册：字体渲染）
 * 完全自研：位图字体、TrueType 解析、OpenType 解析、字形轮廓光栅化、
 * 抗锯齿、亚像素 ClearType、字体度量行高、字符映射 Unicode、字距连字、
 * 字体回退替换、字体缓存、字体子集化、粗体斜体合成、文本排版换行、
 * 双向文本 BiDi、复杂文字整形、中日韩字体处理、字体嵌入许可、
 * 渲染性能、测试。
 * ========================================================================== */
#ifndef XOS_FONT_H
#define XOS_FONT_H

#include "types.h"

#define FONT_MAX_CACHE   16u
#define FONT_MAX_FACES   8u
#define FONT_MAX_FALLBACK 4u
#define FONT_MAX_GLYPH   32u
#define FONT_NAME_LEN    16u

/* 字体类型 */
#define FONT_BITMAP  0u
#define FONT_TTF     1u
#define FONT_OTF     2u

typedef struct {
    u32 id;
    u32 type;                    /* FONT_BITMAP / FONT_TTF / FONT_OTF */
    u32 size;                    /* 像素高度 */
    u32 flags;                   /* 位 0: 伪粗体 位 1: 伪斜体 */
    char name[FONT_NAME_LEN];
    u32 used;
} font_face_t;

typedef struct {
    u32 code;                    /* Unicode 码点 */
    u32 glyph;                   /* 字形索引 */
    u32 advance;                 /* 前进宽度 */
    u32 used;
} font_cmap_t;

typedef struct {
    u32 face_id;
    u32 glyph;
    u32 last_used;
    u32 hit;
    u32 used;
} font_cache_t;

/* ---------------- API ---------------- */
/* 1. 位图字体 */
int font_init(void);
int font_bitmap_register(const char *name, u32 size, u32 *id);
int font_bitmap_glyph(u32 face_id, u32 code, u8 *bits);

/* 2. TrueType 字体解析 */
int font_ttf_parse(const u8 *data, u32 len, u32 *sfnt, u32 *tables);

/* 3. OpenType 字体解析 */
int font_otf_parse(const u8 *data, u32 len, u32 *sfnt, u32 *cff);

/* 4. 字形轮廓光栅化 */
int font_glyph_raster(u32 face_id, u32 glyph, u8 *bits, u32 w, u32 h);
int font_glyph_metrics(u32 face_id, u32 glyph, u32 *w, u32 *h, u32 *adv);

/* 5. 抗锯齿渲染 */
int font_aa_enable(u32 on);
u32 font_aa_state(void);

/* 6. 亚像素渲染 ClearType */
int font_subpixel_enable(u32 on);
u32 font_subpixel_state(void);

/* 7. 字体度量与行高 */
int font_metrics(u32 face_id, u32 *ascent, u32 *descent, u32 *height);
int font_line_height(u32 face_id, u32 *h);

/* 8. 字符映射与 Unicode */
int font_cmap_add(u32 face_id, u32 code, u32 glyph, u32 advance);
int font_cmap_lookup(u32 face_id, u32 code, u32 *glyph);

/* 9. 字距调整与连字 */
int font_kerning_add(u32 face_id, u32 a, u32 b, int delta);
int font_kerning_get(u32 face_id, u32 a, u32 b, int *delta);
int font_ligature_enable(u32 on);

/* 10. 字体回退与替换 */
int font_fallback_add(u32 face_id, u32 fallback_id);
int font_fallback_get(u32 face_id, u32 missing, u32 *out_face);

/* 11. 字体缓存 */
int font_cache_get(u32 face_id, u32 glyph, u32 *idx);
int font_cache_hits(u32 *hits, u32 *misses);
int font_cache_clear(void);

/* 12. 字体子集化 */
int font_subset_begin(u32 face_id, u32 *token);
int font_subset_add(u32 token, u32 code);
u32 font_subset_count(u32 token);
int font_subset_end(u32 token);

/* 13. 粗体与斜体合成 */
int font_synthetic(u32 face_id, u32 bold, u32 italic);

/* 14. 文本排版与换行 */
int font_wrap(const char *text, u32 max_width, u32 *lines);

/* 15. 双向文本 BiDi */
int font_bidi_set(u32 dir);
u32 font_bidi_dir(void);

/* 16. 复杂文字整形 */
int font_shape_enable(u32 on);
u32 font_shape_state(void);

/* 17. 中日韩字体处理 */
int font_cjk_probe(const char *face_name, u32 *ok);

/* 18. 字体嵌入与许可 */
int font_license_set(u32 face_id, u32 perm);
u32 font_license_get(u32 face_id);

/* 19. 字体渲染性能 */
u32 font_render_cost(void);
void font_dump(void);

/* 20. 字体渲染测试 */
int font_selftest(void);

#endif /* XOS_FONT_H */
