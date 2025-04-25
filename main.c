/**********************************************************************
 * Variable-font live viewer (HB + FreeType + SDL3)                   *
 *                                                                    *
 *   License: public domain or CC0                                    *
 *                                                                    *
 *   Keys                                                             *
 *     Q / A  heavier | lighter (wght)                                *
 *     W / S  wider   | tighter (wdth)                                *
 *     E / D  more-italic | upright (slnt)                            *
 *     1-9    toggle Stylistic Set ss01 … ss09 (only if present)      *
 *     Esc    quit                                                    *
 *                                                                    *
 *   Run  :  ./vf font.ttf  [#RRGGBB]                                 *
 *********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H /* FT_Get_MM_Var */

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

/* ---------------- text to show (multiline allowed) ---------------- */
static const char *DISPLAY_TEXT =
    "I left to join a vigorous rowing crew in July 2023, logging 4,321\n\
strokes — 0 excuses, 1 goal, 2 oars, 3 victories, and great Growth (01234).";

/* --------------------- simple helpers ----------------------------- */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct
{
  Uint8 r, g, b;
} RGB;

static bool parse_hex_colour(const char *s, RGB *c)
{
  if (*s == '#')
    ++s;
  if (strlen(s) != 6)
    return false;
  char *e;
  long v = strtol(s, &e, 16);
  if (*e)
    return false;
  c->r = (v >> 16) & 0xff;
  c->g = (v >> 8) & 0xff;
  c->b = v & 0xff;
  return true;
}
static inline Uint32 px(Uint8 a, RGB c)
{ /* premultiplied */
  return ((Uint32)a << 24) | ((Uint32)c.b << 16) | ((Uint32)c.g << 8) | c.r;
}
static int igcd(int a, int b) { return b ? igcd(b, a % b) : abs(a); }

/* ---------------- axis record ------------------------------------- */
typedef struct
{
  bool present;
  int min, max, def, step; /* integer design-space units */
} Axis;

/* derive step from named-instance coordinates (fallback = 5 %) */
static void init_axis(Axis *ax, FT_MM_Var *mm, FT_ULong tag,
                      int fb_min, int fb_max, int fb_def)
{
  ax->present = false;
  ax->min = fb_min;
  ax->max = fb_max;
  ax->def = fb_def;
  if (!mm)
  {
    ax->step = MAX(1, (ax->max - ax->min) / 20);
    return;
  }

  /* locate axis record */
  int idx = -1;
  for (FT_UInt i = 0; i < mm->num_axis; i++)
    if (mm->axis[i].tag == tag)
    {
      idx = i;
      break;
    }
  if (idx == -1)
  {
    ax->step = MAX(1, (ax->max - ax->min) / 20);
    return;
  }

  FT_Var_Axis *a = &mm->axis[idx];
  ax->present = true;
  ax->min = a->minimum >> 16;
  ax->max = a->maximum >> 16;
  ax->def = a->def >> 16;

  /* collect all instance coordinates for this axis */
  int gcd = 0, count = 0;
  for (FT_UInt n = 0; n < mm->num_namedstyles; n++)
  {
    int coord = mm->namedstyle[n].coords[idx] >> 16;
    if (coord < ax->min || coord > ax->max)
      continue;
    if (count)
    {
      gcd = igcd(gcd, abs(coord - ax->def));
    }
    else
    {
      gcd = abs(coord - ax->def);
    }
    count++;
  }
  /* Heuristic: if too few samples or gcd spans the whole range,
     assume a continuous axis → 1-unit step (or 1 % of range). */
  int range = ax->max - ax->min;
  if (count < 3 || gcd == range)
    gcd = MAX(1, range / 100); /* fine-grained */

  if (gcd == 0)
    gcd = 1;
  ax->step = gcd;
}

/* ---------- feature-presence check that works on HarfBuzz ≥11 ----- */
static bool has_feature(hb_face_t *face, hb_tag_t feat_tag)
{
  /* Assume Latin */
  unsigned int script_index;
  if (!hb_ot_layout_table_find_script(face,
                                      HB_OT_TAG_GSUB,             /* GSUB table  */
                                      HB_TAG('l', 'a', 't', 'n'), /* 'latn'      */
                                      &script_index))
    return false;

  /* Use the default language-system (index 0) */
  unsigned int feat_index;
  return hb_ot_layout_language_find_feature(face,
                                            HB_OT_TAG_GSUB, /* table      */
                                            script_index,
                                            HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                            feat_tag,
                                            &feat_index);
}

/* ================================================================== */
int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3)
  {
    fprintf(stderr, "Usage: %s font.ttf [#RRGGBB]\n", argv[0]);
    return 1;
  }
  RGB colour = {0, 0, 0};
  if (argc == 3 && !parse_hex_colour(argv[2], &colour))
  {
    fprintf(stderr, "Colour must be #RRGGBB\n");
    return 1;
  }

  /* ---------- FreeType & axis metadata -------------------------- */
  FT_Library lib;
  FT_Face face;
  FT_Init_FreeType(&lib);
  if (FT_New_Face(lib, argv[1], 0, &face))
  {
    fprintf(stderr, "Cannot open %s\n", argv[1]);
    return 1;
  }

  FT_MM_Var *mm = NULL;
  FT_Get_MM_Var(face, &mm);

  Axis wght, wdth, slnt;
  init_axis(&wght, mm, FT_MAKE_TAG('w', 'g', 'h', 't'), 100, 900, 400);
  init_axis(&wdth, mm, FT_MAKE_TAG('w', 'd', 't', 'h'), 50, 100, 100);
  init_axis(&slnt, mm, FT_MAKE_TAG('s', 'l', 'n', 't'), -15, 0, 0);

  /* Clamp to the real range just in case */
  wght.def = MIN(MAX(400, wght.min), wght.max);
  wdth.def = MIN(MAX(100, wdth.min), wdth.max);

  /* ---------- SDL window & DPI scale ---------------------------- */
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *win = SDL_CreateWindow("Variable-font viewer", 1000, 200,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
  int pt_h, px_h;
  SDL_GetWindowSize(win, NULL, &pt_h);
  SDL_GetWindowSizeInPixels(win, NULL, &px_h);
  float scale = (float)px_h / pt_h;

  /* Set raster size */
  const int POINT_SIZE = 24;

  FT_Set_Char_Size(face, 0, (int)(POINT_SIZE * 64 * scale), 0, 0);
  hb_font_t *hb_font = hb_ft_font_create_referenced(face);
  hb_face_t *hb_face = hb_font_get_face(hb_font);

  /* detect which ss0n exist, print info -------------------------- */
  bool ss_present[20] = {0};
  for (int i = 0; i < 20; i++)
  {
    hb_tag_t tag = HB_TAG('s', 's', '0' + (i / 10), '0' + (i % 10));
    ss_present[i] = has_feature(hb_face, tag);
  }
  fprintf(stderr, "Axis ranges  wght:%d-%d  wdth:%d-%d  slnt:%d-%d\n",
          wght.min, wght.max, wdth.min, wdth.max, slnt.min, slnt.max);
  fprintf(stderr, "Stylistic sets present:");
  for (int i = 0; i < 9; i++)
    if (ss_present[i])
      fprintf(stderr, " ss0%d", i + 1);
  fprintf(stderr, "\n");

  bool ss_flags[9] = {0};

  fprintf(stderr, "Debug: DISPLAY_TEXT length = %zu\n", strlen(DISPLAY_TEXT));

  /* ------------ render loop helper ------------------------------ */
render:
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  SDL_RenderClear(ren);

  /* apply current variation coords */
  const hb_variation_t vars[] = {
      {HB_TAG('w', 'g', 'h', 't'), wght.def},
      {HB_TAG('w', 'd', 't', 'h'), wdth.def},
      {HB_TAG('s', 'l', 'n', 't'), slnt.def}};
  hb_font_set_variations(hb_font, vars, 3);

  /* build feature list */
  hb_feature_t f[9];
  unsigned fc = 0;
  for (int i = 0; i < 9; i++)
    if (ss_flags[i] && ss_present[i])
    {
      f[fc++] = (hb_feature_t){HB_TAG('s', 's', '0', '1' + i), 1, 0, ~0u};
    }

  /* split text into lines */
  char *text = strdup(DISPLAY_TEXT);
  char *line = text;
  float pen_y = 80.0f * scale;
  float line_gap = (POINT_SIZE * 1.3f) * scale;
  for (char *p = text;; p++)
  {
    if (*p == '\n' || *p == '\0')
    {
      char terminator = *p; /* save it */
      *p = '\0';            /* end current line              */
      /* shape and draw this line with features */
      hb_buffer_t *buf = hb_buffer_create();
      hb_buffer_add_utf8(buf, line, -1, 0, -1);
      hb_buffer_guess_segment_properties(buf);
      hb_shape(hb_font, buf, f, fc);

      unsigned gc;
      hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &gc);
      hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &gc);

      float pen_x = 50.0f * scale;
      for (unsigned i = 0; i < gc; i++)
      {
        FT_Load_Glyph(face, info[i].codepoint, FT_LOAD_DEFAULT);
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
        FT_Bitmap *b = &face->glyph->bitmap;
        if (b->width && b->rows)
        {
          int w = b->width, h = b->rows;
          Uint32 *pix = malloc(sizeof(Uint32) * w * h);
          for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
              pix[y * w + x] = px(b->buffer[y * b->pitch + x], colour);
          SDL_Surface *s = SDL_CreateSurfaceFrom(
              w, h, SDL_PIXELFORMAT_RGBA32, pix, w * sizeof(Uint32));
          SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
          SDL_FRect dst = {pen_x + pos[i].x_offset / 64.0f + face->glyph->bitmap_left,
                           pen_y - pos[i].y_offset / 64.0f - face->glyph->bitmap_top,
                           (float)w, (float)h};
          SDL_RenderTexture(ren, t, NULL, &dst);
          SDL_DestroyTexture(t);
          SDL_DestroySurface(s);
          free(pix);
        }
        pen_x += pos[i].x_advance / 64.0f;
      }
      hb_buffer_destroy(buf);
      pen_y += line_gap;
      line = p + 1;
      if (terminator == '\0') /* original byte was real NUL?   */
        break;                /* yes → finished                */
    }
  }
  free(text);
  SDL_RenderPresent(ren);

  /* ------------- event loop ------------------------------------- */
  SDL_Event ev;
  while (SDL_WaitEvent(&ev))
  {
    if (ev.type == SDL_EVENT_QUIT)
      goto quit;
    if (ev.type != SDL_EVENT_KEY_DOWN)
      continue;
    bool dirty = false;
    switch (ev.key.key)
    {
    case SDLK_ESCAPE:
      goto quit;

    case SDLK_Q:
      if (wght.present)
      {
        wght.def = MIN(wght.def + wght.step, wght.max);
        dirty = true;
      }
      break;
    case SDLK_A:
      if (wght.present)
      {
        wght.def = MAX(wght.def - wght.step, wght.min);
        dirty = true;
      }
      break;
    case SDLK_W:
      if (wdth.present)
      {
        wdth.def = MIN(wdth.def + wdth.step, wdth.max);
        dirty = true;
      }
      break;
    case SDLK_S:
      if (wdth.present)
      {
        wdth.def = MAX(wdth.def - wdth.step, wdth.min);
        dirty = true;
      }
      break;
    case SDLK_E:
      if (slnt.present)
      {
        slnt.def = MAX(slnt.def - slnt.step, slnt.min);
        dirty = true;
      }
      break;
    case SDLK_D:
      if (slnt.present)
      {
        slnt.def = MIN(slnt.def + slnt.step, slnt.max);
        dirty = true;
      }
      break;

    case SDLK_1:
    case SDLK_2:
    case SDLK_3:
    case SDLK_4:
    case SDLK_5:
    case SDLK_6:
    case SDLK_7:
    case SDLK_8:
    case SDLK_9:
    {
      int i = ev.key.key - SDLK_1;
      if (ss_present[i])
      {
        ss_flags[i] = !ss_flags[i];
        dirty = true;
      }
    }
    break;
    default:
      break;
    }
    if (dirty)
      goto render;
  }

quit:
  if (mm)
    FT_Done_MM_Var(lib, mm);
  hb_font_destroy(hb_font);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  return 0;
}
