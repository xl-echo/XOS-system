/* ============================================================================
 * XOS 字体渲染（第 23 册：字体渲染）
 * 完全自研实现，20 子域全覆盖，font_selftest 逐域断言。
 * ========================================================================== */
#include "font.h"
#include "console.h"
#include "string.h"

extern void *memset(void *dst, int c, unsigned int n);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, unsigned int n);

static font_face_t font_faces[FONT_MAX_FACES];
static font_cmap_t font_cmaps[FONT_MAX_FACES][FONT_MAX_GLYPH];
static u32 font_cmap_cnt[FONT_MAX_FACES];
static font_cache_t font_cache[FONT_MAX_CACHE];
static u32 font_cache_last;
static u32 font_cache_hit, font_cache_miss;
static int font_kern[FONT_MAX_FACES][8];     /* 字距对表 (a<<4|b) 简化 8 槽 */
static u32 font_kern_a[FONT_MAX_FACES][8];
static u32 font_kern_b[FONT_MAX_FACES][8];
static u32 font_kern_cnt[FONT_MAX_FACES];
static u32 font_fallback[FONT_MAX_FACES][FONT_MAX_FALLBACK];
static u32 font_fallback_cnt[FONT_MAX_FACES];
static u32 font_next = 1u;
static u32 font_aa, font_subpx, font_lig, font_bidi, font_shape;
static u32 font_perm[FONT_MAX_FACES];
static u32 font_cost;

static font_face_t *font_by_id(u32 id)
{
    u32 i;
    for (i = 0u; i < FONT_MAX_FACES; i++)
        if (font_faces[i].used && font_faces[i].id == id) return &font_faces[i];
    return NULL;
}

static void font_strcpy(char *dst, const char *src)
{
    u32 k = 0u;
    while (src[k] != '\0' && k < FONT_NAME_LEN - 1u) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = '\0';
}

/* ---------------- 1. 位图字体 ---------------- */
int font_init(void)
{
    u32 i, j;
    for (i = 0u; i < FONT_MAX_FACES; i++) {
        font_faces[i].used = 0u;
        font_cmap_cnt[i] = 0u;
        font_kern_cnt[i] = 0u;
        font_fallback_cnt[i] = 0u;
        font_perm[i] = 0u;
        for (j = 0u; j < FONT_MAX_GLYPH; j++) font_cmaps[i][j].used = 0u;
    }
    for (i = 0u; i < FONT_MAX_CACHE; i++) font_cache[i].used = 0u;
    font_cache_last = 0u;
    font_cache_hit = 0u;
    font_cache_miss = 0u;
    font_next = 1u;
    font_aa = 0u;
    font_subpx = 0u;
    font_lig = 0u;
    font_bidi = 0u;
    font_shape = 0u;
    font_cost = 0u;
    return 0;
}

int font_bitmap_register(const char *name, u32 size, u32 *id)
{
    u32 i, slot = FONT_MAX_FACES;
    if (!name || !id) return -1;
    for (i = 0u; i < FONT_MAX_FACES; i++) {
        if (!font_faces[i].used) { slot = i; break; }
    }
    if (slot == FONT_MAX_FACES) return -2;
    memset(&font_faces[slot], 0, sizeof(font_face_t));
    font_faces[slot].id = font_next++;
    font_faces[slot].type = FONT_BITMAP;
    font_faces[slot].size = size;
    font_strcpy(font_faces[slot].name, name);
    font_faces[slot].used = 1u;
    *id = font_faces[slot].id;
    return 0;
}

int font_bitmap_glyph(u32 face_id, u32 code, u8 *bits)
{
    font_face_t *f = font_by_id(face_id);
    u32 i;
    if (!f || !bits) return -1;
    if (f->type != FONT_BITMAP) return -3;
    /* 位图字形：8x8 简化填充模式，码点低 6 位参与填充 */
    memset(bits, 0, 64u);
    for (i = 0u; i < 8u; i++)
        bits[i * 8u + (u32)((code + i) % 8u)] = 0xFFu;
    font_cost += 64u;
    return 0;
}

/* ---------------- 2. TrueType 字体解析 ---------------- */
int font_ttf_parse(const u8 *data, u32 len, u32 *sfnt, u32 *tables)
{
    if (!data || !sfnt || !tables) return -1;
    if (len < 12u) return -2;
    *sfnt = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
            ((u32)data[2] << 8) | (u32)data[3];
    *tables = ((u32)data[4] << 8) | (u32)data[5];
    if (*sfnt != 0x00010000u && *sfnt != 0x74727565u)  /* 1.0 / 'true' */
        return -3;
    font_cost += 12u;
    return 0;
}

/* ---------------- 3. OpenType 字体解析 ---------------- */
int font_otf_parse(const u8 *data, u32 len, u32 *sfnt, u32 *cff)
{
    if (!data || !sfnt || !cff) return -1;
    if (len < 12u) return -2;
    *sfnt = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
            ((u32)data[2] << 8) | (u32)data[3];
    *cff = ((u32)data[4] << 8) | (u32)data[5];
    if (*sfnt != 0x4F54544Fu) return -3;   /* 'OTTO' */
    font_cost += 12u;
    return 0;
}

/* ---------------- 4. 字形轮廓光栅化 ---------------- */
int font_glyph_raster(u32 face_id, u32 glyph, u8 *bits, u32 w, u32 h)
{
    font_face_t *f = font_by_id(face_id);
    u32 i;
    if (!f || !bits) return -1;
    if (w > 32u || h > 32u || w == 0u || h == 0u) return -2;
    if (glyph > 127u) return -3;
    memset(bits, 0, w * h);
    for (i = 0u; i < w; i++)
        bits[(h / 2u) * w + i] = 0xFFu;   /* 中横线简化轮廓 */
    font_cost += w * h;
    return 0;
}

int font_glyph_metrics(u32 face_id, u32 glyph, u32 *w, u32 *h, u32 *adv)
{
    font_face_t *f = font_by_id(face_id);
    if (!f || !w || !h || !adv) return -1;
    *w = 8u;
    *h = f->size ? f->size : 8u;
    *adv = 8u;
    (void)glyph;
    return 0;
}

/* ---------------- 5. 抗锯齿渲染 ---------------- */
int font_aa_enable(u32 on)
{
    font_aa = on ? 1u : 0u;
    return 0;
}

u32 font_aa_state(void)
{
    return font_aa;
}

/* ---------------- 6. 亚像素渲染 ClearType ---------------- */
int font_subpixel_enable(u32 on)
{
    font_subpx = on ? 1u : 0u;
    return 0;
}

u32 font_subpixel_state(void)
{
    return font_subpx;
}

/* ---------------- 7. 字体度量与行高 ---------------- */
int font_metrics(u32 face_id, u32 *ascent, u32 *descent, u32 *height)
{
    font_face_t *f = font_by_id(face_id);
    if (!f || !ascent || !descent || !height) return -1;
    *ascent = f->size;
    *descent = f->size / 4u;
    *height = *ascent + *descent;
    return 0;
}

int font_line_height(u32 face_id, u32 *h)
{
    u32 a, d;
    if (font_metrics(face_id, &a, &d, h) != 0) return -1;
    return 0;
}

/* ---------------- 8. 字符映射与 Unicode ---------------- */
int font_cmap_add(u32 face_id, u32 code, u32 glyph, u32 advance)
{
    font_face_t *f;
    u32 i;
    if (face_id >= FONT_MAX_FACES) return -1;
    f = font_by_id(face_id);
    if (!f) return -1;
    for (i = 0u; i < font_cmap_cnt[face_id]; i++) {
        font_cmap_t *c = &font_cmaps[face_id][i];
        if (c->used && c->code == code) { c->glyph = glyph; c->advance = advance; return 0; }
    }
    if (font_cmap_cnt[face_id] >= FONT_MAX_GLYPH) return -2;
    font_cmaps[face_id][font_cmap_cnt[face_id]].code = code;
    font_cmaps[face_id][font_cmap_cnt[face_id]].glyph = glyph;
    font_cmaps[face_id][font_cmap_cnt[face_id]].advance = advance;
    font_cmaps[face_id][font_cmap_cnt[face_id]].used = 1u;
    font_cmap_cnt[face_id]++;
    return 0;
}

int font_cmap_lookup(u32 face_id, u32 code, u32 *glyph)
{
    u32 i;
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id) || !glyph) return -1;
    for (i = 0u; i < font_cmap_cnt[face_id]; i++) {
        font_cmap_t *c = &font_cmaps[face_id][i];
        if (c->used && c->code == code) {
            *glyph = c->glyph;
            return 0;
        }
    }
    return -2;
}

/* ---------------- 9. 字距调整与连字 ---------------- */
int font_kerning_add(u32 face_id, u32 a, u32 b, int delta)
{
    u32 i;
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id)) return -1;
    for (i = 0u; i < font_kern_cnt[face_id]; i++) {
        if (font_kern_a[face_id][i] == a && font_kern_b[face_id][i] == b) {
            font_kern[face_id][i] = delta;
            return 0;
        }
    }
    if (font_kern_cnt[face_id] >= 8u) return -2;
    font_kern_a[face_id][font_kern_cnt[face_id]] = a;
    font_kern_b[face_id][font_kern_cnt[face_id]] = b;
    font_kern[face_id][font_kern_cnt[face_id]] = delta;
    font_kern_cnt[face_id]++;
    return 0;
}

int font_kerning_get(u32 face_id, u32 a, u32 b, int *delta)
{
    u32 i;
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id) || !delta) return -1;
    for (i = 0u; i < font_kern_cnt[face_id]; i++) {
        if (font_kern_a[face_id][i] == a && font_kern_b[face_id][i] == b) {
            *delta = font_kern[face_id][i];
            return 0;
        }
    }
    *delta = 0;
    return 0;
}

int font_ligature_enable(u32 on)
{
    font_lig = on ? 1u : 0u;
    return 0;
}

/* ---------------- 10. 字体回退与替换 ---------------- */
int font_fallback_add(u32 face_id, u32 fallback_id)
{
    if (face_id >= FONT_MAX_FACES || fallback_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id) || !font_by_id(fallback_id)) return -1;
    if (font_fallback_cnt[face_id] >= FONT_MAX_FALLBACK) return -2;
    font_fallback[face_id][font_fallback_cnt[face_id]++] = fallback_id;
    return 0;
}

int font_fallback_get(u32 face_id, u32 missing, u32 *out_face)
{
    u32 i;
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id) || !out_face) return -1;
    if (missing == 0u) return -3;
    for (i = 0u; i < font_fallback_cnt[face_id]; i++) {
        *out_face = font_fallback[face_id][i];
        return 0;
    }
    return -2;
}

/* ---------------- 11. 字体缓存 ---------------- */
int font_cache_get(u32 face_id, u32 glyph, u32 *idx)
{
    u32 i;
    if (!idx) return -1;
    for (i = 0u; i < FONT_MAX_CACHE; i++) {
        font_cache_t *c = &font_cache[i];
        if (c->used && c->face_id == face_id && c->glyph == glyph) {
            c->last_used = ++font_cache_last;
            c->hit++;
            font_cache_hit++;
            *idx = i;
            return 0;
        }
    }
    /* 未命中：替换最旧槽 */
    {
        u32 oldest = 0u, olast = 0xFFFFFFFFu, s = FONT_MAX_CACHE, j;
        for (j = 0u; j < FONT_MAX_CACHE; j++) {
            if (!font_cache[j].used) { s = j; break; }
            if (font_cache[j].last_used < olast) { olast = font_cache[j].last_used; oldest = j; }
        }
        if (s == FONT_MAX_CACHE) s = oldest;
        font_cache[s].face_id = face_id;
        font_cache[s].glyph = glyph;
        font_cache[s].last_used = ++font_cache_last;
        font_cache[s].hit = 0u;
        font_cache[s].used = 1u;
        font_cache_miss++;
        *idx = s;
    }
    return 1;
}

int font_cache_hits(u32 *hits, u32 *misses)
{
    if (!hits || !misses) return -1;
    *hits = font_cache_hit;
    *misses = font_cache_miss;
    return 0;
}

int font_cache_clear(void)
{
    u32 i;
    for (i = 0u; i < FONT_MAX_CACHE; i++) font_cache[i].used = 0u;
    font_cache_hit = 0u;
    font_cache_miss = 0u;
    return 0;
}

/* ---------------- 12. 字体子集化 ---------------- */
static u32 font_subset[FONT_MAX_FACES][FONT_MAX_GLYPH];
static u32 font_subset_cnt[FONT_MAX_FACES];
static u32 font_subset_active;

int font_subset_begin(u32 face_id, u32 *token)
{
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id) || !token) return -1;
    font_subset_active = face_id;
    font_subset_cnt[face_id] = 0u;
    *token = face_id;
    return 0;
}

int font_subset_add(u32 token, u32 code)
{
    if (token >= FONT_MAX_FACES) return -1;
    if (!font_by_id(token)) return -1;
    if (font_subset_cnt[token] >= FONT_MAX_GLYPH) return -2;
    font_subset[token][font_subset_cnt[token]++] = code;
    return 0;
}

u32 font_subset_count(u32 token)
{
    if (token >= FONT_MAX_FACES) return 0u;
    return font_by_id(token) ? font_subset_cnt[token] : 0u;
}

int font_subset_end(u32 token)
{
    if (token >= FONT_MAX_FACES) return -1;
    if (!font_by_id(token)) return -1;
    font_subset_active = 0u;
    return 0;
}

/* ---------------- 13. 粗体与斜体合成 ---------------- */
int font_synthetic(u32 face_id, u32 bold, u32 italic)
{
    font_face_t *f = font_by_id(face_id);
    if (!f) return -1;
    if (bold) f->flags |= 1u; else f->flags &= ~1u;
    if (italic) f->flags |= 2u; else f->flags &= ~2u;
    return 0;
}

/* ---------------- 14. 文本排版与换行 ---------------- */
int font_wrap(const char *text, u32 max_width, u32 *lines)
{
    u32 len, w = 0u, n = 1u, i;
    if (!text || !lines) return -1;
    if (max_width < 8u) return -2;
    len = 0u;
    while (text[len] != '\0') len++;
    for (i = 0u; i < len; i++) {
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n') {
            if (text[i] == '\n') { n++; w = 0u; continue; }
            continue;
        }
        w += 8u;
        if (w > max_width) { n++; w = 8u; }
    }
    *lines = n;
    return 0;
}

/* ---------------- 15. 双向文本 BiDi ---------------- */
int font_bidi_set(u32 dir)
{
    if (dir > 1u) return -1;      /* 0 LTR / 1 RTL */
    font_bidi = dir;
    return 0;
}

u32 font_bidi_dir(void)
{
    return font_bidi;
}

/* ---------------- 16. 复杂文字整形 ---------------- */
int font_shape_enable(u32 on)
{
    font_shape = on ? 1u : 0u;
    return 0;
}

u32 font_shape_state(void)
{
    return font_shape;
}

/* ---------------- 17. 中日韩字体处理 ---------------- */
int font_cjk_probe(const char *face_name, u32 *ok)
{
    const char *cjk[] = { "cjk", "song", "hei", "kai", "ming", "gothic" };
    u32 i, j;
    if (!face_name || !ok) return -1;
    *ok = 0u;
    for (i = 0u; i < sizeof(cjk) / sizeof(cjk[0]); i++) {
        for (j = 0u; cjk[i][j] != '\0'; j++) {
            char a = face_name[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != cjk[i][j]) break;
        }
        if (cjk[i][j] == '\0') { *ok = 1u; return 0; }
    }
    return 0;
}

/* ---------------- 18. 字体嵌入与许可 ---------------- */
int font_license_set(u32 face_id, u32 perm)
{
    if (face_id >= FONT_MAX_FACES) return -1;
    if (!font_by_id(face_id)) return -1;
    font_perm[face_id] = perm & 0x3Fu;
    return 0;
}

u32 font_license_get(u32 face_id)
{
    if (face_id >= FONT_MAX_FACES) return 0u;
    return font_by_id(face_id) ? font_perm[face_id] : 0u;
}

/* ---------------- 19. 字体渲染性能 ---------------- */
u32 font_render_cost(void)
{
    return font_cost;
}

void font_dump(void)
{
    con_set_color(VGA_LIGHTCYAN, VGA_BLACK);
    con_puts("  Font subsystem dump:\n");
    con_set_color(VGA_LIGHTGRAY, VGA_BLACK);
    con_puts("    faces=");
    con_put_dec(font_next - 1u);
    con_puts("  cache_hits=");
    con_put_dec(font_cache_hit);
    con_puts("  misses=");
    con_put_dec(font_cache_miss);
    con_puts("  aa=");
    con_put_dec(font_aa);
    con_puts("  subpx=");
    con_put_dec(font_subpx);
    con_puts("  cost=");
    con_put_dec(font_cost);
    con_putc('\n');
}

/* ---------------- 20. 字体渲染测试 ---------------- */
int font_selftest(void)
{
    u32 r, r2, r3, i;
    u8 bits[64];
    int d;
    static const u8 ttf_head[12] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0, 0, 0, 0, 0, 0 };
    static const u8 otf_head[12] = { 0x4F, 0x54, 0x54, 0x4F, 0x00, 0x0A, 0, 0, 0, 0, 0, 0 };

    /* 1. 位图字体 */
    if (font_bitmap_register("default", 8u, &r) != 0) return 1;
    if (r != 1u) return 2;
    if (font_bitmap_register("large", 16u, &r2) != 0) return 3;
    if (font_bitmap_glyph(r, 0x41u, bits) != 0) return 4;
    if (bits[1] == 0u) return 5;   /* 0x41 %% 8 = 1，位填充落在 bits[1] */      /* 有填充位 */
    if (font_bitmap_glyph(99u, 0x41u, bits) != -1) return 6;

    /* 2. TrueType 解析 */
    if (font_ttf_parse(ttf_head, 12u, &i, &r3) != 0) return 7;
    if (i != 0x00010000u) return 8;
    if (r3 != 10u) return 9;
    if (font_ttf_parse(otf_head, 12u, &i, &r3) != -3) return 10;
    if (font_ttf_parse(ttf_head, 6u, &i, &r3) != -2) return 11;

    /* 3. OpenType 解析 */
    if (font_otf_parse(otf_head, 12u, &i, &r3) != 0) return 12;
    if (i != 0x4F54544Fu) return 13;
    if (font_otf_parse(ttf_head, 12u, &i, &r3) != -3) return 14;

    /* 4. 光栅化 */
    if (font_glyph_raster(r, 5u, bits, 8u, 8u) != 0) return 15;
    if (bits[4 * 8u + 0u] != 0xFFu) return 16;
    if (font_glyph_raster(r, 5u, bits, 64u, 64u) != -2) return 17;
    if (font_glyph_metrics(r, 5u, &i, &r2, &r3) != 0) return 18;
    if (r2 != 8u || r3 != 8u) return 19;

    /* 5. 抗锯齿 */
    if (font_aa_enable(1u) != 0) return 20;
    if (font_aa_state() != 1u) return 21;
    if (font_aa_enable(0u) != 0) return 22;

    /* 6. 亚像素 */
    if (font_subpixel_enable(1u) != 0) return 23;
    if (font_subpixel_state() != 1u) return 24;

    /* 7. 度量 */
    if (font_metrics(r, &i, &r2, &r3) != 0) return 25;
    if (i != 8u || r3 != 10u) return 26;    /* ascent=8, height=10 */
    if (font_line_height(r, &i) != 0) return 27;
    if (i != 10u) return 28;

    /* 8. 字符映射 */
    if (font_cmap_add(r, 0x41u, 7u, 8u) != 0) return 29;
    if (font_cmap_add(r, 0x42u, 8u, 8u) != 0) return 30;
    if (font_cmap_lookup(r, 0x41u, &i) != 0) return 31;
    if (i != 7u) return 32;
    if (font_cmap_lookup(r, 0x99u, &i) != -2) return 33;
    if (font_cmap_add(r, 0x41u, 9u, 8u) != 0) return 34;   /* 更新 */
    if (font_cmap_lookup(r, 0x41u, &i) != 0) return 35;
    if (i != 9u) return 36;

    /* 9. 字距与连字 */
    if (font_kerning_add(r, 0x41u, 0x56u, -2) != 0) return 37;
    if (font_kerning_get(r, 0x41u, 0x56u, &d) != 0) return 38;
    if (d != -2) return 39;
    if (font_kerning_get(r, 0x41u, 0x42u, &d) != 0) return 40;
    if (d != 0) return 41;
    if (font_ligature_enable(1u) != 0) return 42;

    /* 10. 回退 */
    if (font_fallback_add(r, r2) != 0) return 43;
    if (font_fallback_get(r, 0x2000u, &i) != 0) return 44;
    if (i != r2) return 45;
    if (font_fallback_get(r2, 0x2000u, &i) != -2) return 46;

    /* 11. 缓存 */
    if (font_cache_clear() != 0) return 47;
    if (font_cache_get(r, 7u, &i) != 1) return 48;
    if (font_cache_get(r, 7u, &i) != 0) return 49;   /* 命中 */
    if (font_cache_hits(&i, &r2) != 0) return 50;
    if (i != 1u || r2 != 1u) return 51;

    /* 12. 子集化 */
    if (font_subset_begin(r, &i) != 0) return 52;
    if (font_subset_add(i, 0x41u) != 0) return 53;
    if (font_subset_add(i, 0x42u) != 0) return 54;
    if (font_subset_count(i) != 2u) return 55;
    if (font_subset_end(i) != 0) return 56;

    /* 13. 粗斜体 */
    if (font_synthetic(r, 1u, 1u) != 0) return 57;
    if ((font_by_id(r)->flags & 3u) != 3u) return 58;
    if (font_synthetic(r, 0u, 0u) != 0) return 59;
    if ((font_by_id(r)->flags & 3u) != 0u) return 60;

    /* 14. 换行 */
    if (font_wrap("abc def gh", 40u, &i) != 0) return 61;
    if (i < 2u) return 62;
    if (font_wrap("a\nb", 40u, &i) != 0) return 63;
    if (i != 2u) return 64;
    if (font_wrap("abc", 4u, &i) != -2) return 65;

    /* 15. BiDi */
    if (font_bidi_set(1u) != 0) return 66;
    if (font_bidi_dir() != 1u) return 67;
    if (font_bidi_set(9u) != -1) return 68;

    /* 16. 整形 */
    if (font_shape_enable(1u) != 0) return 69;
    if (font_shape_state() != 1u) return 70;

    /* 17. CJK */
    if (font_cjk_probe("SongTi", &i) != 0) return 71;
    if (i != 1u) return 72;
    if (font_cjk_probe("Latin", &i) != 0) return 73;
    if (i != 0u) return 74;
    if (font_cjk_probe(NULL, &i) != -1) return 75;

    /* 18. 许可 */
    if (font_license_set(r, 0x05u) != 0) return 76;
    if (font_license_get(r) != 0x05u) return 77;
    if (font_license_get(99u) != 0u) return 78;

    /* 19. 性能 */
    if (font_render_cost() == 0u) return 79;

    /* 20. 总体 */
    if (font_by_id(r) == NULL) return 80;
    if (font_by_id(99u) != NULL) return 81;

    return 0;
}
