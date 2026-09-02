#include "inkview.h"
#include "rsvp_glue.h"

/* Starsze inkview.h nie definiują KEY_HOME — na nowszych firmware to 0x1a */
#ifndef KEY_HOME
#define KEY_HOME 0x1a
#endif

#include <zip.h>

#include <libxml/HTMLparser.h>
#include <libxml/HTMLtree.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>

// RSVP
#define RSVP_TIMER_NAME "rsvp_tick"

/* User-visible app name (binary stays rsvp.app) */
#define APP_DISPLAY_NAME "RSVP Reader"
#define APP_CREDIT "made by Mateusz Blumensztajn (Sableworks)"
#define SPLASH_TIMER_NAME "rsvp_splash"
#define SPLASH_MS 1500

// UI / rendering
#define WORD_FONT_SIZE 48
#define UI_FONT_SIZE 18
#define BROWSE_FONT_SIZE 28
#define BROWSE_TITLE_SIZE 24
#define BROWSE_ROW_H 64
#define BROWSE_HEADER_H 78
#define BROWSE_FOOTER_H 44
#define ORP_RATIO_NUM 35  // 0.35
#define ORP_RATIO_DEN 100
#define WORD_PAD_PX 10
#define TOP_BAR_H 58
#define CTRL_BAR_H 132
#define FOOTER_WORDS 5
#define FOOTER_H_PX CTRL_BAR_H
#define MENU_W_PX 300
#define MENU_H_PX 220

/* Panel opcji po pauzie: info postępu + lista akcji (e-ink, czytelne wiersze) */
#define PAUSE_INFO_H 118
#define PAUSE_ROW_H 54
#define PAUSE_OPT_COUNT 5
#define PAUSE_PANEL_H (PAUSE_INFO_H + PAUSE_OPT_COUNT * PAUSE_ROW_H)

enum {
  PAUSE_OPT_PLAY = 0,
  PAUSE_OPT_CHAPTERS = 1,
  PAUSE_OPT_NAV = 2, /* « rozdz. | początek | rozdz. » */
  PAUSE_OPT_WPM = 3,
  PAUSE_OPT_LEAVE = 4
};

// WPM (słowa/min)
#define WPM_DEFAULT 220
#define WPM_STEP 10
#define WPM_SWIPE_STEP 10
#define WPM_MIN 30
/* B300 (Kaleido 3): PartialUpdate potrzebuje ~240 ms na czytelne słowo */
#define EINK_MIN_WORD_MS 240
#define WPM_MAX 250
/* Jednostki wyświetlania: słowa funkcyjne (rsvp_glue.h) łączone z następnym */
#define UNIT_TEXT_MAX 384
#define CHAPTER_MAX 256

/* Pełne odświeżenie e-ink co N słów podczas odtwarzania (mniej ghostingu) */
#define FULL_REFRESH_EVERY 60
#define WPM_BADGE_MS 2500
#define WPM_BADGE_TIMER "wpm_badge"
#define INI_KEY_WPM "wpm"
#define SAVE_LINE_MAX 2048

// File dialog defaults
#define EPUB_EXT ".epub"
#define FILE_DIALOG_TITLE APP_DISPLAY_NAME ": wybierz EPUB"

// Save file
#define SAVE_FILE_NAME ".rsvp_saves.ini"

/* Kody błędów parse_epub_to_words (0 = OK, <0 = błąd) */
enum {
  PARSE_OK = 0,
  PARSE_ERR_OPEN = -1,
  PARSE_ERR_EMPTY = -2,
  PARSE_ERR_OOM = -3,
  PARSE_ERR_TRUNC = -4
};

typedef struct {
  const char *word; // wskaźnik do podłańcucha w g_text_buf (tokenizacja w miejscu)
  int width_px;
} WordEntry;

typedef struct {
  char *title;
  int word_idx; /* pierwsze słowo rozdziału */
} ChapterEntry;

enum {
  READER_MENU_NONE = 0,
  READER_MENU_WPM = 2,
  READER_MENU_CHAPTERS = 3
};

static struct {
  // Screen
  int sw;
  int sh;

  ifont *font_word;
  ifont *font_ui;
  ifont *font_browse;
  ifont *font_browse_title;
  int word_text_h;
  int ui_text_h;
  int browse_text_h;

  // Data
  char *epub_path;
  char *text_buf; // surowy tekst (tokenizowany in-place)
  WordEntry *words;
  int word_count;

  ChapterEntry *chapters;
  int chapter_count;

  int next_word_idx;    // indeks "następnego słowa do odczytania" (może być == word_count przy EOF)
  int display_word_idx; // indeks słowa narysowanego na ekranie (preview/odczyt)
  int rect_valid;
  int last_rect_x, last_rect_y, last_rect_w, last_rect_h;

  // Playback
  int playing;
  int reader_menu; /* READER_MENU_* */
  int chrome_visible; /* jak w PB: paski góra/dół po tapnięciu */
  int wpm;

  int pointer_down_y;
  int pointer_down_valid;

  // File dialog / mini-eksplorator (własny UI pod dotyk)
  int file_dialog_opened;
  int file_selected;
  char selected_epub_path[1024];
  int browse_active;
  int browse_scroll;
  int browse_sel; /* podświetlony wiersz (przyciski ◄►) */
  int browse_row_h;
  int browse_header_h;

  /* Lista rozdziałów */
  int chapter_scroll;
  int chapter_sel;

  int words_since_full;
  int wpm_badge_on;
  int last_parse_err;

  /* Splash na starcie (1 s) — ignoruj wejście aż skończy */
  int splash_active;
} g;

static void free_book_data(void);
static void render_word_at_preview(void);
static void render_footer(void);
static void restart_timer(void);
static void set_playing(int enable, int from_menu);
static void advance_and_render_one_word(void);
static void open_menu(void);
static void close_menu(void);
static void menu_redraw(void);
static void stop_playback_timer(void);
static void leave_book_to_browser(void);
static void open_chapter_picker(void);
static void draw_chapter_picker(void);
static void jump_to_chapter(int chap_idx);
static void jump_relative_chapter(int delta);
static void jump_to_book_start(void);
static void show_browser_list(void);
static void save_progress(void);
static void save_wpm(void);
static void load_wpm(void);
static void load_progress_and_set_next_index(void);
static int parse_epub_to_words(const char *path);
static int current_chapter_index(void);
static int pause_options_on(void);
static int pause_panel_h(void);
static void OpenFileDialog(void);
static void draw_splash(void);
static void splash_finish(void);
static void splash_timer(void);
static void render_pause_options(void);
static void show_wpm_badge(void);
static void clear_wpm_badge(void);
static void clamp_wpm(void);
static void wpm_badge_timer(void);

// Helpers
static int is_word_char(unsigned char c) {
  /* ASCII litery/cyfry + apostrof; bajty UTF-8 (>=0x80) też w słowie (PL: ąęć itd.) */
  if (isalnum(c) || c == '\'' || c == '-' ) return 1;
  if (c >= 0x80) return 1;
  return 0;
}

static const char *path_basename(const char *p) {
  const char *slash = strrchr(p, '/');
  return slash ? (slash + 1) : p;
}

static int ends_with_caseinsensitive(const char *s, const char *suffix) {
  size_t ls = strlen(s);
  size_t lfx = strlen(suffix);
  if (ls < lfx) return 0;
  s += (ls - lfx);
  for (size_t i = 0; i < lfx; i++) {
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return 0;
  }
  return 1;
}

static void safe_strncpy(char *dst, size_t dstsz, const char *src) {
  if (!dst || dstsz == 0) return;
  strncpy(dst, src, dstsz - 1);
  dst[dstsz - 1] = '\0';
}

// HTML -> text (libxml2)
static void html_collect_text_recursive(xmlNode *node, int skip_head, int skip_style, xmlBuffer *out) {
  for (; node; node = node->next) {
    if (node->type == XML_ELEMENT_NODE) {
      const char *name = (const char *)node->name;

      // Całkowicie ignoruj <head> i <style>
      if (name && strcmp(name, "head") == 0) {
        html_collect_text_recursive(node->children, 1, skip_style, out);
        continue;
      }
      if (name && strcmp(name, "style") == 0) {
        html_collect_text_recursive(node->children, skip_head, 1, out);
        continue;
      }

      // Dodatkowo: skrypt/noscript traktujemy jako "niewidoczne"
      if (!skip_head && !skip_style && name && (strcmp(name, "script") == 0 || strcmp(name, "noscript") == 0)) {
        html_collect_text_recursive(node->children, 1, skip_style, out);
        continue;
      }
      html_collect_text_recursive(node->children,
                                  skip_head,
                                  skip_style,
                                  out);
      continue;
    }

    if (node->type == XML_TEXT_NODE) {
      if (skip_head || skip_style) continue;
      xmlChar *content = xmlNodeGetContent(node);
      if (!content) continue;
      // Wyczyść whitespace na brzegach; zachowaj pojedynczą spację jako separator.
      const char *c = (const char *)content;
      int has_non_ws = 0;
      for (; *c; c++) {
        if (!isspace((unsigned char)*c)) {
          has_non_ws = 1;
          break;
        }
      }
      if (has_non_ws) {
        if (xmlBufferLength(out) > 0) xmlBufferAdd(out, (const xmlChar *)" ", 1);
        xmlBufferCat(out, content);
      }
      xmlFree(content);
    }
  }
}

static char *html_extract_visible_text(const char *html_bytes, size_t html_size) {
  if (!html_bytes || html_size == 0) return NULL;

  xmlDoc *doc = htmlReadMemory(html_bytes, (int)html_size, NULL, NULL,
                                HTML_PARSE_NOERROR |
                                    HTML_PARSE_RECOVER |
                                    HTML_PARSE_NOWARNING);
  if (!doc) return NULL;

  xmlBuffer *out = xmlBufferCreate();
  if (!out) {
    xmlFreeDoc(doc);
    return NULL;
  }

  xmlNode *root = xmlDocGetRootElement(doc);
  html_collect_text_recursive(root, 0, 0, out);

  // Skopiuj do malloc (łatwiejsze zarządzanie pamięcią w aplikacji)
  char *result = (char *)malloc(xmlBufferLength(out) + 1);
  if (result) {
    memcpy(result, xmlBufferContent(out), xmlBufferLength(out));
    result[xmlBufferLength(out)] = '\0';
  }

  xmlBufferFree(out);
  xmlFreeDoc(doc);
  return result;
}

static int str_append(char **buf, size_t *cap, size_t *len, const char *s) {
  if (!s) return 1;
  size_t sl = strlen(s);
  if (*len + sl + 1 > *cap) {
    size_t new_cap = (*cap == 0) ? (sl + 1024) : (*cap * 2);
    while (new_cap < *len + sl + 1) new_cap *= 2;
    char *nb = (char *)realloc(*buf, new_cap);
    if (!nb) return 0;
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, s, sl);
  *len += sl;
  (*buf)[*len] = '\0';
  return 1;
}

static char *html_strip_tags_fallback(const char *html, size_t n) {
  if (!html || n == 0) return NULL;
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  size_t o = 0;
  int in_tag = 0;
  for (size_t i = 0; i < n; i++) {
    char c = html[i];
    if (c == '<') {
      in_tag = 1;
      continue;
    }
    if (c == '>') {
      in_tag = 0;
      if (o > 0 && out[o - 1] != ' ') out[o++] = ' ';
      continue;
    }
    if (in_tag) continue;
    if (c == '&') {
      /* proste entity */
      if (i + 5 <= n && strncmp(html + i, "&amp;", 5) == 0) {
        out[o++] = '&';
        i += 4;
        continue;
      }
      if (i + 4 <= n && strncmp(html + i, "&lt;", 4) == 0) {
        out[o++] = '<';
        i += 3;
        continue;
      }
      if (i + 4 <= n && strncmp(html + i, "&gt;", 4) == 0) {
        out[o++] = '>';
        i += 3;
        continue;
      }
      if (i + 6 <= n && strncmp(html + i, "&nbsp;", 6) == 0) {
        out[o++] = ' ';
        i += 5;
        continue;
      }
      continue;
    }
    out[o++] = c;
  }
  out[o] = '\0';
  return out;
}

static char *read_file_via_iv(const char *path, size_t *out_size) {
  if (out_size) *out_size = 0;
  if (!path) return NULL;

  FILE *f = iv_fopen(path, "rb");
  int used_iv = 1;
  if (!f) {
    f = fopen(path, "rb");
    used_iv = 0;
  }
  if (!f) return NULL;

  /* Rozmiar */
  size_t sz = 0;
  struct stat st;
  memset(&st, 0, sizeof(st));
  if (iv_stat(path, &st) == 0 && st.st_size > 0) {
    sz = (size_t)st.st_size;
  } else if (stat(path, &st) == 0 && st.st_size > 0) {
    sz = (size_t)st.st_size;
  } else {
    /* Seek — iv_fseek / fseek */
    if (used_iv) {
      if (iv_fseek(f, 0, SEEK_END) == 0) {
        long pos = iv_ftell(f);
        if (pos > 0) sz = (size_t)pos;
        iv_fseek(f, 0, SEEK_SET);
      }
    } else {
      if (fseek(f, 0, SEEK_END) == 0) {
        long pos = ftell(f);
        if (pos > 0) sz = (size_t)pos;
        fseek(f, 0, SEEK_SET);
      }
    }
  }

  if (sz == 0 || sz > 80 * 1024 * 1024) {
    if (used_iv) iv_fclose(f);
    else fclose(f);
    return NULL;
  }

  char *buf = (char *)malloc(sz + 1);
  if (!buf) {
    if (used_iv) iv_fclose(f);
    else fclose(f);
    return NULL;
  }

  size_t got = 0;
  while (got < sz) {
    size_t chunk = sz - got;
    if (chunk > 8192) chunk = 8192;
    int n;
    if (used_iv) n = iv_fread(buf + got, 1, (int)chunk, f);
    else n = (int)fread(buf + got, 1, chunk, f);
    if (n <= 0) break;
    got += (size_t)n;
  }

  if (used_iv) iv_fclose(f);
  else fclose(f);

  if (got == 0) {
    free(buf);
    return NULL;
  }
  buf[got] = '\0';
  if (out_size) *out_size = got;
  return buf;
}

static zip_t *epub_zip_open(const char *path, char **owned_buf) {
  *owned_buf = NULL;

  /* 1) Na PocketBooku standardowy open() bywa ślepy — czytamy przez iv_fopen. */
  size_t sz = 0;
  char *buf = read_file_via_iv(path, &sz);
  if (buf && sz > 0) {
    zip_error_t zerr;
    zip_error_init(&zerr);
    zip_source_t *src = zip_source_buffer_create(buf, (zip_uint64_t)sz, 1, &zerr);
    if (!src) {
      free(buf);
      zip_error_fini(&zerr);
    } else {
      /* freep=1: libzip zwolni buf przy zip_close */
      zip_t *za = zip_open_from_source(src, ZIP_RDONLY, &zerr);
      if (!za) {
        zip_source_free(src);
        /* buf już przejęty przez source? przy nieudanym open_from_source
           źródło trzeba zwolnić; freep mógł zwolnić buf — nie free ponownie */
        zip_error_fini(&zerr);
        buf = NULL;
      } else {
        *owned_buf = NULL; /* własność w zip */
        zip_error_fini(&zerr);
        return za;
      }
    }
  }

  /* 2) Fallback: zwykły zip_open (emulator / gdy iv nie zadziała) */
  int err = 0;
  zip_t *za = zip_open(path, ZIP_RDONLY, &err);
  return za;
}

static int entry_is_html(const char *name) {
  return ends_with_caseinsensitive(name, ".html") ||
         ends_with_caseinsensitive(name, ".xhtml") ||
         ends_with_caseinsensitive(name, ".htm");
}

static int media_type_is_html(const char *mt) {
  if (!mt || !mt[0]) return 0;
  if (strstr(mt, "html") != NULL) return 1;
  if (strcmp(mt, "application/xhtml+xml") == 0) return 1;
  return 0;
}

static void path_dirname_copy(const char *path, char *out, size_t outsz) {
  safe_strncpy(out, outsz, path ? path : "");
  char *slash = strrchr(out, '/');
  if (slash) *slash = '\0';
  else out[0] = '\0';
}

/* Połącz katalog bazowy z href (usuń ./ i uprość ../); wynik bez fragmentu # */
static void epub_resolve_href(const char *base_dir, const char *href, char *out, size_t outsz) {
  char tmp[1024];
  const char *h = href ? href : "";
  const char *hash = strchr(h, '#');
  size_t hlen = hash ? (size_t)(hash - h) : strlen(h);
  if (hlen >= sizeof(tmp)) hlen = sizeof(tmp) - 1;
  memcpy(tmp, h, hlen);
  tmp[hlen] = '\0';

  if (tmp[0] == '/') {
    safe_strncpy(out, outsz, tmp + 1);
    return;
  }

  char joined[1280];
  if (base_dir && base_dir[0]) {
    snprintf(joined, sizeof(joined), "%s/%s", base_dir, tmp);
  } else {
    safe_strncpy(joined, sizeof(joined), tmp);
  }

  char parts[32][96];
  int nparts = 0;
  const char *p = joined;
  while (*p && nparts < 32) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t len = (size_t)(p - start);
    if (len == 1 && start[0] == '.') continue;
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (nparts > 0) nparts--;
      continue;
    }
    if (len >= sizeof(parts[0])) len = sizeof(parts[0]) - 1;
    memcpy(parts[nparts], start, len);
    parts[nparts][len] = '\0';
    nparts++;
  }
  out[0] = '\0';
  for (int i = 0; i < nparts; i++) {
    if (i > 0) {
      size_t L = strlen(out);
      if (L + 1 < outsz) {
        out[L] = '/';
        out[L + 1] = '\0';
      }
    }
    size_t L = strlen(out);
    if (outsz > L) safe_strncpy(out + L, outsz - L, parts[i]);
  }
}

static char *zip_read_named(zip_t *za, const char *name, size_t *out_size) {
  if (out_size) *out_size = 0;
  if (!za || !name || !name[0]) return NULL;

  struct zip_stat st;
  if (zip_stat(za, name, 0, &st) != 0) {
    /* Spróbuj bez wiodącego ./ */
    if (name[0] == '.' && name[1] == '/') {
      if (zip_stat(za, name + 2, 0, &st) != 0) return NULL;
      name = name + 2;
    } else {
      return NULL;
    }
  }
  if (st.size <= 0 || st.size > 20 * 1024 * 1024) return NULL;

  zip_file_t *zf = zip_fopen(za, name, 0);
  if (!zf && name[0] == '.' && name[1] == '/') zf = zip_fopen(za, name + 2, 0);
  if (!zf) return NULL;

  char *bytes = (char *)malloc((size_t)st.size + 1);
  if (!bytes) {
    zip_fclose(zf);
    return NULL;
  }

  zip_int64_t total_read = 0;
  while (total_read < (zip_int64_t)st.size) {
    zip_int64_t to_read = (zip_int64_t)st.size - total_read;
    if (to_read > 4096) to_read = 4096;
    zip_int64_t r = zip_fread(zf, bytes + total_read, (size_t)to_read);
    if (r <= 0) break;
    total_read += r;
  }
  bytes[total_read] = '\0';
  zip_fclose(zf);
  if (total_read <= 0) {
    free(bytes);
    return NULL;
  }
  if (out_size) *out_size = (size_t)total_read;
  return bytes;
}

static xmlNode *xml_find_child_ci(xmlNode *parent, const char *local) {
  for (xmlNode *n = parent ? parent->children : NULL; n; n = n->next) {
    if (n->type != XML_ELEMENT_NODE) continue;
    if (n->name && strcasecmp((const char *)n->name, local) == 0) return n;
  }
  return NULL;
}

static xmlNode *xml_find_desc_ci(xmlNode *root, const char *local) {
  if (!root) return NULL;
  if (root->type == XML_ELEMENT_NODE && root->name &&
      strcasecmp((const char *)root->name, local) == 0) {
    return root;
  }
  for (xmlNode *n = root->children; n; n = n->next) {
    xmlNode *f = xml_find_desc_ci(n, local);
    if (f) return f;
  }
  return NULL;
}

typedef struct {
  char id[128];
  char href[512];
  char media[128];
  int is_nav;
} OpfItem;

typedef struct {
  char href[512];
  char title[96];
} TocEntry;

static void toc_set_title(TocEntry *toc, int *toc_n, int toc_cap,
                          const char *href, const char *title) {
  if (!href || !href[0] || !title || !title[0]) return;
  char key[512];
  safe_strncpy(key, sizeof(key), href);
  char *hash = strchr(key, '#');
  if (hash) *hash = '\0';
  if (!key[0]) return;

  for (int i = 0; i < *toc_n; i++) {
    if (strcmp(toc[i].href, key) == 0) {
      if (toc[i].title[0] == '\0')
        safe_strncpy(toc[i].title, sizeof(toc[i].title), title);
      return;
    }
  }
  if (*toc_n >= toc_cap) return;
  safe_strncpy(toc[*toc_n].href, sizeof(toc[*toc_n].href), key);
  safe_strncpy(toc[*toc_n].title, sizeof(toc[*toc_n].title), title);
  (*toc_n)++;
}

static void toc_lookup(const TocEntry *toc, int toc_n, const char *href,
                       char *out, size_t outsz) {
  out[0] = '\0';
  if (!href) return;
  char key[512];
  safe_strncpy(key, sizeof(key), href);
  char *hash = strchr(key, '#');
  if (hash) *hash = '\0';
  for (int i = 0; i < toc_n; i++) {
    if (strcmp(toc[i].href, key) == 0) {
      safe_strncpy(out, outsz, toc[i].title);
      return;
    }
  }
  /* Match po basename */
  const char *base = path_basename(key);
  for (int i = 0; i < toc_n; i++) {
    if (strcmp(path_basename(toc[i].href), base) == 0) {
      safe_strncpy(out, outsz, toc[i].title);
      return;
    }
  }
}

static void parse_nav_xhtml_titles(const char *bytes, size_t sz,
                                   const char *opf_dir, TocEntry *toc,
                                   int *toc_n, int toc_cap) {
  if (!bytes || sz == 0) return;
  xmlDoc *doc = xmlReadMemory(bytes, (int)sz, NULL, NULL,
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
  if (!doc) {
    doc = htmlReadMemory(bytes, (int)sz, NULL, NULL,
                         HTML_PARSE_NOERROR | HTML_PARSE_RECOVER | HTML_PARSE_NOWARNING);
  }
  if (!doc) return;

  xmlNode *root = xmlDocGetRootElement(doc);
  /* Zbierz wszystkie <a href> w dokumencie nawigacji */
  for (xmlNode *n = root; n;) {
    if (n->type == XML_ELEMENT_NODE && n->name &&
        strcasecmp((const char *)n->name, "a") == 0) {
      xmlChar *href = xmlGetProp(n, (const xmlChar *)"href");
      if (href) {
        xmlChar *txt = xmlNodeGetContent(n);
        if (txt) {
          /* Przytnij whitespace */
          char title[96];
          const char *p = (const char *)txt;
          while (*p && isspace((unsigned char)*p)) p++;
          safe_strncpy(title, sizeof(title), p);
          size_t L = strlen(title);
          while (L > 0 && isspace((unsigned char)title[L - 1])) {
            title[--L] = '\0';
          }
          if (title[0]) {
            char resolved[512];
            epub_resolve_href(opf_dir, (const char *)href, resolved, sizeof(resolved));
            toc_set_title(toc, toc_n, toc_cap, resolved, title);
          }
          xmlFree(txt);
        }
        xmlFree(href);
      }
    }
    /* DFS iteracyjny */
    if (n->children) {
      n = n->children;
    } else {
      while (n && !n->next) n = n->parent;
      if (n) n = n->next;
    }
  }
  xmlFreeDoc(doc);
}

static void parse_ncx_titles(const char *bytes, size_t sz, const char *opf_dir,
                             TocEntry *toc, int *toc_n, int toc_cap) {
  if (!bytes || sz == 0) return;
  xmlDoc *doc = xmlReadMemory(bytes, (int)sz, NULL, NULL,
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
  if (!doc) return;
  xmlNode *root = xmlDocGetRootElement(doc);

  for (xmlNode *n = root; n;) {
    if (n->type == XML_ELEMENT_NODE && n->name &&
        strcasecmp((const char *)n->name, "navPoint") == 0) {
      char title_buf[96];
      title_buf[0] = '\0';
      char src_buf[512];
      src_buf[0] = '\0';

      for (xmlNode *c = n->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE || !c->name) continue;
        if (strcasecmp((const char *)c->name, "navLabel") == 0) {
          xmlNode *t = xml_find_child_ci(c, "text");
          if (t) {
            xmlChar *tx = xmlNodeGetContent(t);
            if (tx) {
              safe_strncpy(title_buf, sizeof(title_buf), (const char *)tx);
              xmlFree(tx);
            }
          }
        } else if (strcasecmp((const char *)c->name, "content") == 0) {
          xmlChar *s = xmlGetProp(c, (const xmlChar *)"src");
          if (s) {
            safe_strncpy(src_buf, sizeof(src_buf), (const char *)s);
            xmlFree(s);
          }
        }
      }
      if (title_buf[0] && src_buf[0]) {
        char resolved[512];
        epub_resolve_href(opf_dir, src_buf, resolved, sizeof(resolved));
        toc_set_title(toc, toc_n, toc_cap, resolved, title_buf);
      }
    }
    if (n->children) {
      n = n->children;
    } else {
      while (n && !n->next) n = n->parent;
      if (n) n = n->next;
    }
  }
  xmlFreeDoc(doc);
}

static int parse_container_rootfile(zip_t *za, char *opf_path, size_t opf_sz) {
  size_t sz = 0;
  char *bytes = zip_read_named(za, "META-INF/container.xml", &sz);
  if (!bytes) bytes = zip_read_named(za, "meta-inf/container.xml", &sz);
  if (!bytes) return 0;

  xmlDoc *doc = xmlReadMemory(bytes, (int)sz, NULL, NULL,
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
  free(bytes);
  if (!doc) return 0;

  xmlNode *root = xmlDocGetRootElement(doc);
  xmlNode *rf = xml_find_desc_ci(root, "rootfile");
  int ok = 0;
  if (rf) {
    xmlChar *fp = xmlGetProp(rf, (const xmlChar *)"full-path");
    if (fp && fp[0]) {
      safe_strncpy(opf_path, opf_sz, (const char *)fp);
      ok = 1;
    }
    if (fp) xmlFree(fp);
  }
  xmlFreeDoc(doc);
  return ok;
}

static int opf_collect(zip_t *za, const char *opf_path,
                       OpfItem *items, int *item_n, int item_cap,
                       char spine_ids[][128], int *spine_n, int spine_cap,
                       char *opf_dir, size_t opf_dir_sz,
                       char *ncx_href, size_t ncx_sz,
                       char *nav_href, size_t nav_sz) {
  *item_n = 0;
  *spine_n = 0;
  opf_dir[0] = '\0';
  ncx_href[0] = '\0';
  nav_href[0] = '\0';

  size_t sz = 0;
  char *bytes = zip_read_named(za, opf_path, &sz);
  if (!bytes) return 0;

  path_dirname_copy(opf_path, opf_dir, opf_dir_sz);

  xmlDoc *doc = xmlReadMemory(bytes, (int)sz, NULL, NULL,
                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER);
  free(bytes);
  if (!doc) return 0;

  xmlNode *root = xmlDocGetRootElement(doc);
  xmlNode *manifest = xml_find_desc_ci(root, "manifest");
  xmlNode *spine = xml_find_desc_ci(root, "spine");

  if (manifest) {
    for (xmlNode *n = manifest->children; n; n = n->next) {
      if (n->type != XML_ELEMENT_NODE || !n->name) continue;
      if (strcasecmp((const char *)n->name, "item") != 0) continue;
      if (*item_n >= item_cap) break;

      xmlChar *id = xmlGetProp(n, (const xmlChar *)"id");
      xmlChar *href = xmlGetProp(n, (const xmlChar *)"href");
      xmlChar *mt = xmlGetProp(n, (const xmlChar *)"media-type");
      xmlChar *props = xmlGetProp(n, (const xmlChar *)"properties");

      OpfItem *it = &items[*item_n];
      memset(it, 0, sizeof(*it));
      if (id) safe_strncpy(it->id, sizeof(it->id), (const char *)id);
      if (href) {
        epub_resolve_href(opf_dir, (const char *)href, it->href, sizeof(it->href));
      }
      if (mt) safe_strncpy(it->media, sizeof(it->media), (const char *)mt);

      int is_nav = 0;
      if (props && strstr((const char *)props, "nav") != NULL) is_nav = 1;
      if (mt && strstr((const char *)mt, "ncx") != NULL) {
        if (it->href[0] && !ncx_href[0])
          safe_strncpy(ncx_href, ncx_sz, it->href);
      }
      if (is_nav && it->href[0] && !nav_href[0])
        safe_strncpy(nav_href, nav_sz, it->href);
      it->is_nav = is_nav;

      if (it->id[0] && it->href[0]) (*item_n)++;

      if (id) xmlFree(id);
      if (href) xmlFree(href);
      if (mt) xmlFree(mt);
      if (props) xmlFree(props);
    }
  }

  if (spine) {
    xmlChar *toc_id = xmlGetProp(spine, (const xmlChar *)"toc");
    if (toc_id && !ncx_href[0]) {
      for (int i = 0; i < *item_n; i++) {
        if (strcmp(items[i].id, (const char *)toc_id) == 0) {
          safe_strncpy(ncx_href, ncx_sz, items[i].href);
          break;
        }
      }
    }
    if (toc_id) xmlFree(toc_id);

    for (xmlNode *n = spine->children; n; n = n->next) {
      if (n->type != XML_ELEMENT_NODE || !n->name) continue;
      if (strcasecmp((const char *)n->name, "itemref") != 0) continue;
      if (*spine_n >= spine_cap) break;
      xmlChar *idref = xmlGetProp(n, (const xmlChar *)"idref");
      if (idref) {
        safe_strncpy(spine_ids[*spine_n], sizeof(spine_ids[0]), (const char *)idref);
        (*spine_n)++;
        xmlFree(idref);
      }
    }
  }

  xmlFreeDoc(doc);
  return (*item_n > 0 && *spine_n > 0);
}

static int href_is_non_content(const char *href, const char *media) {
  if (media) {
    if (strstr(media, "css") || strstr(media, "font") ||
        strstr(media, "image") || strstr(media, "audio") ||
        strstr(media, "video") || strstr(media, "javascript") ||
        strstr(media, "ncx")) {
      return 1;
    }
  }
  if (!href) return 1;
  if (ends_with_caseinsensitive(href, ".css") ||
      ends_with_caseinsensitive(href, ".js") ||
      ends_with_caseinsensitive(href, ".png") ||
      ends_with_caseinsensitive(href, ".jpg") ||
      ends_with_caseinsensitive(href, ".jpeg") ||
      ends_with_caseinsensitive(href, ".gif") ||
      ends_with_caseinsensitive(href, ".svg") ||
      ends_with_caseinsensitive(href, ".ttf") ||
      ends_with_caseinsensitive(href, ".otf") ||
      ends_with_caseinsensitive(href, ".woff") ||
      ends_with_caseinsensitive(href, ".woff2") ||
      ends_with_caseinsensitive(href, ".ncx")) {
    return 1;
  }
  return 0;
}

static int append_chapter_html(zip_t *za, const char *entry_name, const char *title_hint,
                               char **agg, size_t *agg_cap, size_t *agg_len,
                               size_t *chap_offs, char chap_titles[][96], int *chap_n,
                               int *oom) {
  size_t sz = 0;
  char *bytes = zip_read_named(za, entry_name, &sz);
  if (!bytes) return 0;

  char *visible_text = html_extract_visible_text(bytes, sz);
  if (!visible_text || !visible_text[0]) {
    if (visible_text) free(visible_text);
    visible_text = html_strip_tags_fallback(bytes, sz);
  }
  free(bytes);

  if (!visible_text || !visible_text[0]) {
    if (visible_text) free(visible_text);
    return 0;
  }

  /* Pomiń prawie puste „rozdziały” (np. tylko NBSP) */
  int meaningful = 0;
  for (const char *p = visible_text; *p; p++) {
    if (!isspace((unsigned char)*p)) {
      meaningful = 1;
      break;
    }
  }
  if (!meaningful) {
    free(visible_text);
    return 0;
  }

  if (*chap_n < CHAPTER_MAX) {
    chap_offs[*chap_n] = *agg_len;
    if (title_hint && title_hint[0]) {
      safe_strncpy(chap_titles[*chap_n], sizeof(chap_titles[0]), title_hint);
    } else {
      const char *base = path_basename(entry_name);
      safe_strncpy(chap_titles[*chap_n], sizeof(chap_titles[0]), base);
      char *dot = strrchr(chap_titles[*chap_n], '.');
      if (dot) *dot = '\0';
      if (chap_titles[*chap_n][0] == '\0') {
        snprintf(chap_titles[*chap_n], sizeof(chap_titles[0]), "Rozdział %d", *chap_n + 1);
      }
    }
    (*chap_n)++;
  }
  /* Powyżej CHAPTER_MAX: doklejamy tekst bez nowych wpisów TOC */

  if (!str_append(agg, agg_cap, agg_len, visible_text) ||
      !str_append(agg, agg_cap, agg_len, " ")) {
    free(visible_text);
    *oom = 1;
    return 0;
  }
  free(visible_text);
  return 1;
}

static int parse_epub_to_words(const char *path) {
  free_book_data();
  g.last_parse_err = PARSE_OK;
  if (!path || !path[0]) {
    g.last_parse_err = PARSE_ERR_OPEN;
    return 0;
  }

  ShowHourglass();

  char *owned = NULL;
  zip_t *za = epub_zip_open(path, &owned);
  if (!za) {
    HideHourglass();
    g.last_parse_err = PARSE_ERR_OPEN;
    return 0;
  }

  size_t agg_cap = 0;
  size_t agg_len = 0;
  char *agg = NULL;
  int oom = 0;

  size_t chap_offs[CHAPTER_MAX];
  char chap_titles[CHAPTER_MAX][96];
  int chap_n = 0;

  xmlInitParser();

  OpfItem *items = (OpfItem *)calloc(512, sizeof(OpfItem));
  char (*spine_ids)[128] = calloc(512, 128);
  TocEntry *toc = (TocEntry *)calloc(512, sizeof(TocEntry));
  int item_n = 0, spine_n = 0, toc_n = 0;
  char opf_path[512];
  char opf_dir[512];
  char ncx_href[512];
  char nav_href[512];
  opf_path[0] = opf_dir[0] = ncx_href[0] = nav_href[0] = '\0';

  int used_spine = 0;
  if (items && spine_ids && toc &&
      parse_container_rootfile(za, opf_path, sizeof(opf_path)) &&
      opf_collect(za, opf_path, items, &item_n, 512, spine_ids, &spine_n, 512,
                  opf_dir, sizeof(opf_dir), ncx_href, sizeof(ncx_href),
                  nav_href, sizeof(nav_href))) {
    used_spine = 1;

    /* Tytuły z nav / ncx (tylko metadane, nie treść) */
    if (nav_href[0]) {
      size_t nsz = 0;
      char *nb = zip_read_named(za, nav_href, &nsz);
      if (nb) {
        parse_nav_xhtml_titles(nb, nsz, opf_dir, toc, &toc_n, 512);
        free(nb);
      }
    }
    if (ncx_href[0]) {
      size_t nsz = 0;
      char *nb = zip_read_named(za, ncx_href, &nsz);
      if (nb) {
        parse_ncx_titles(nb, nsz, opf_dir, toc, &toc_n, 512);
        free(nb);
      }
    }

    for (int s = 0; s < spine_n && !oom; s++) {
      OpfItem *it = NULL;
      for (int i = 0; i < item_n; i++) {
        if (strcmp(items[i].id, spine_ids[s]) == 0) {
          it = &items[i];
          break;
        }
      }
      if (!it) continue;
      if (it->is_nav) continue; /* nav — tylko tytuły */
      if (href_is_non_content(it->href, it->media)) continue;
      if (!entry_is_html(it->href) && !media_type_is_html(it->media)) continue;
      /* Porównaj z nav/ncx ścieżką — nie wczytuj ponownie jako treść */
      if (nav_href[0] && strcmp(it->href, nav_href) == 0) continue;
      if (ncx_href[0] && strcmp(it->href, ncx_href) == 0) continue;

      char title[96];
      toc_lookup(toc, toc_n, it->href, title, sizeof(title));
      append_chapter_html(za, it->href, title[0] ? title : NULL,
                          &agg, &agg_cap, &agg_len, chap_offs, chap_titles, &chap_n,
                          &oom);
    }

    /* Spine OK, ale zero tekstu — spróbuj kolejności ZIP */
    if (!oom && (!agg || !agg[0])) {
      used_spine = 0;
      chap_n = 0;
      agg_len = 0;
      if (agg) agg[0] = '\0';
    }
  }

  /* Fallback: kolejność ZIP (stare EPUB-y / brak OPF / pusty spine) */
  if (!used_spine && !oom) {
    zip_int64_t n = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < n && !oom; i++) {
      const char *entry_name = zip_get_name(za, i, ZIP_FL_ENC_GUESS);
      if (!entry_name) continue;
      size_t elen = strlen(entry_name);
      if (elen == 0 || entry_name[elen - 1] == '/') continue;
      if (!entry_is_html(entry_name)) continue;
      if (ends_with_caseinsensitive(entry_name, "container.xml")) continue;
      if (ends_with_caseinsensitive(entry_name, "encryption.xml")) continue;
      if (ends_with_caseinsensitive(entry_name, ".opf")) continue;
      if (ends_with_caseinsensitive(entry_name, ".ncx")) continue;
      if (strstr(entry_name, "META-INF/") || strstr(entry_name, "meta-inf/")) continue;
      if (ends_with_caseinsensitive(entry_name, "nav.xhtml") ||
          ends_with_caseinsensitive(entry_name, "nav.html") ||
          ends_with_caseinsensitive(entry_name, "toc.xhtml")) {
        continue;
      }

      append_chapter_html(za, entry_name, NULL, &agg, &agg_cap, &agg_len,
                          chap_offs, chap_titles, &chap_n, &oom);
    }
  }

  free(items);
  free(spine_ids);
  free(toc);
  zip_close(za);
  if (owned) free(owned);

  HideHourglass();

  if (oom) {
    if (agg) free(agg);
    g.last_parse_err = PARSE_ERR_OOM;
    return 0;
  }

  if (!agg || agg[0] == '\0') {
    if (agg) free(agg);
    g.last_parse_err = PARSE_ERR_EMPTY;
    return 0;
  }

  int cap_words = 4096;
  int count = 0;
  WordEntry *we = (WordEntry *)calloc((size_t)cap_words, sizeof(WordEntry));
  if (!we) {
    free(agg);
    g.last_parse_err = PARSE_ERR_OOM;
    return 0;
  }

  if (g.font_word) SetFont(g.font_word, BLACK);

  char *p = agg;
  char *wstart = NULL;
  int trunc = 0;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    if (is_word_char(c)) {
      if (!wstart) wstart = p;
    } else {
      if (wstart) {
        *p = '\0';
        if (count >= cap_words) {
          cap_words *= 2;
          WordEntry *nwe = (WordEntry *)realloc(we, (size_t)cap_words * sizeof(WordEntry));
          if (!nwe) {
            trunc = 1;
            break;
          }
          we = nwe;
        }
        we[count].word = wstart;
        we[count].width_px = g.font_word ? StringWidth(wstart) : 0;
        count++;
        wstart = NULL;
      }
    }
    p++;
  }
  if (!trunc && wstart) {
    if (count >= cap_words) {
      WordEntry *nwe = (WordEntry *)realloc(we, (size_t)(cap_words + 1) * sizeof(WordEntry));
      if (nwe) {
        we = nwe;
        cap_words++;
      } else {
        trunc = 1;
      }
    }
    if (!trunc && count < cap_words) {
      we[count].word = wstart;
      we[count].width_px = g.font_word ? StringWidth(wstart) : 0;
      count++;
    }
  }

  if (trunc && count <= 0) {
    free(we);
    free(agg);
    g.last_parse_err = PARSE_ERR_OOM;
    return 0;
  }
  if (trunc) {
    /* Krytyczne — nie kontynuuj z uciętym tekstem bez sygnału */
    free(we);
    free(agg);
    g.last_parse_err = PARSE_ERR_TRUNC;
    return 0;
  }

  g.text_buf = agg;
  g.words = we;
  g.word_count = count;
  g.next_word_idx = 0;
  g.display_word_idx = 0;
  g.rect_valid = 0;
  g.words_since_full = 0;

  if (chap_n > 0 && count > 0) {
    g.chapters = (ChapterEntry *)calloc((size_t)chap_n, sizeof(ChapterEntry));
    if (g.chapters) {
      g.chapter_count = chap_n;
      for (int c = 0; c < chap_n; c++) {
        g.chapters[c].title = (char *)malloc(strlen(chap_titles[c]) + 1);
        if (g.chapters[c].title) strcpy(g.chapters[c].title, chap_titles[c]);
        int widx = 0;
        for (int wi = 0; wi < count; wi++) {
          if ((size_t)(g.words[wi].word - g.text_buf) >= chap_offs[c]) {
            widx = wi;
            break;
          }
          widx = wi;
        }
        g.chapters[c].word_idx = widx;
      }
    }
  }

  g.last_parse_err = PARSE_OK;
  return (count > 0);
}

static void free_book_data(void) {
  if (g.words) {
    free(g.words);
    g.words = NULL;
  }
  if (g.text_buf) {
    free(g.text_buf);
    g.text_buf = NULL;
  }
  if (g.chapters) {
    for (int i = 0; i < g.chapter_count; i++) {
      free(g.chapters[i].title);
    }
    free(g.chapters);
    g.chapters = NULL;
  }
  g.chapter_count = 0;
  g.word_count = 0;
  g.next_word_idx = 0;
  g.display_word_idx = 0;
  g.rect_valid = 0;
}

// Rendering — play = immersja (tylko słowo); pauza = panel opcji
static int reader_chrome_on(void) {
  if (g.reader_menu == READER_MENU_CHAPTERS) return 0;
  if (g.reader_menu == READER_MENU_WPM) return 1;
  if (!g.playing) return 1;
  return g.chrome_visible;
}

/* Podczas pauzy: jeden panel opcji na dole (bez osobnego paska ikon u góry).
 * Panel WPM zostawia cienki top bar (wstecz / spis / tytuł). */
static int pause_options_on(void) {
  return reader_chrome_on() && g.reader_menu == READER_MENU_NONE && !g.playing;
}

static int pause_panel_h(void) {
  if (g.reader_menu == READER_MENU_WPM) return CTRL_BAR_H;
  if (pause_options_on()) {
    int h = PAUSE_PANEL_H;
    /* Zostaw trochę miejsca na podgląd słowa nad panelem */
    if (h > g.sh * 2 / 3) h = g.sh * 2 / 3;
    if (h < CTRL_BAR_H) h = CTRL_BAR_H;
    return h;
  }
  return reader_chrome_on() ? CTRL_BAR_H : 0;
}

static int reader_has_top_bar(void) {
  return g.reader_menu == READER_MENU_WPM;
}

static int reader_content_top(void) {
  return reader_has_top_bar() ? TOP_BAR_H : 0;
}

static int reader_content_bottom(void) {
  if (!reader_chrome_on()) return g.sh;
  return g.sh - pause_panel_h();
}

static int unit_word_span(int idx) {
  if (idx < 0 || idx >= g.word_count) return 0;
  if (!rsvp_is_glue_word(g.words[idx].word)) return 1;

  {
    int j = idx;
    while (j < g.word_count && rsvp_is_glue_word(g.words[j].word)) j++;
    if (j < g.word_count) return j - idx + 1;
    return g.word_count - idx;
  }
}

static int unit_start_idx(int idx) {
  if (idx <= 0) return 0;
  if (idx >= g.word_count) return g.word_count;
  if (rsvp_is_glue_word(g.words[idx].word)) {
    while (idx > 0 && rsvp_is_glue_word(g.words[idx - 1].word)) idx--;
    return idx;
  }
  if (rsvp_is_glue_word(g.words[idx - 1].word)) {
    while (idx > 0 && rsvp_is_glue_word(g.words[idx - 1].word)) idx--;
    return idx;
  }
  return idx;
}

static void format_unit_text(int idx, char *out, size_t outsz) {
  int span = unit_word_span(idx);
  size_t used = 0;
  out[0] = '\0';
  if (span <= 0 || outsz == 0) return;

  for (int k = 0; k < span; k++) {
    const char *part = g.words[idx + k].word;
    if (!part) continue;
    if (k > 0 && used + 1 < outsz) out[used++] = ' ';
    while (*part && used + 1 < outsz) out[used++] = *part++;
  }
  out[used] = '\0';
}

static int get_preview_idx(void) {
  if (g.word_count <= 0) return 0;
  if (g.next_word_idx >= g.word_count) return g.word_count - 1;
  return g.next_word_idx;
}

static int progress_word_num(void) {
  int idx = get_preview_idx() + 1;
  if (g.word_count <= 0) return 0;
  if (idx > g.word_count) idx = g.word_count;
  return idx;
}

static int progress_percent(void) {
  if (g.word_count <= 0) return 0;
  return (int)(((long)progress_word_num() * 100) / g.word_count);
}

/* Indeks bieżącego rozdziału wg pozycji słowa; -1 jeśli brak TOC */
static int current_chapter_index(void) {
  if (g.chapter_count <= 0) return -1;
  int cur = -1;
  int pos = g.next_word_idx;
  if (pos > 0) pos--; /* ostatnio pokazane / w trakcie */
  for (int i = 0; i < g.chapter_count; i++) {
    if (g.chapters[i].word_idx <= pos) cur = i;
    else break;
  }
  return cur;
}

static void book_title_short(char *out, size_t outsz) {
  const char *base = g.epub_path ? path_basename(g.epub_path) : APP_DISPLAY_NAME;
  safe_strncpy(out, outsz, base);
  char *dot = strrchr(out, '.');
  if (dot) *dot = '\0';
  if (out[0] == '\0') safe_strncpy(out, outsz, APP_DISPLAY_NAME);
}

static void draw_orp_guides(int cx, int word_y, int word_h) {
  int top = word_y - 28;
  int bot = word_y + word_h + 16;
  int ctop = reader_content_top() + 8;
  int cbot = reader_content_bottom() - 8;
  if (top < ctop) top = ctop;
  if (bot > cbot) bot = cbot;
  DrawLine(cx, top, cx, word_y - 6, DGRAY);
  DrawLine(cx, word_y + word_h + 4, cx, bot, DGRAY);
}

static void render_top_bar(void) {
  FillArea(0, 0, g.sw, TOP_BAR_H, WHITE);
  DrawLine(0, TOP_BAR_H - 1, g.sw, TOP_BAR_H - 1, BLACK);

  if (g.font_browse) SetFont(g.font_browse, BLACK);
  /* Strefy jak w PB: wstecz | spis | tytuł */
  DrawTextRect(0, 0, 72, TOP_BAR_H, "‹", ALIGN_CENTER | VALIGN_MIDDLE);
  DrawLine(72, 10, 72, TOP_BAR_H - 10, LGRAY);
  DrawTextRect(72, 0, 72, TOP_BAR_H, "≡", ALIGN_CENTER | VALIGN_MIDDLE);
  DrawLine(144, 10, 144, TOP_BAR_H - 10, LGRAY);

  char title[96];
  book_title_short(title, sizeof(title));
  if (g.font_ui) SetFont(g.font_ui, BLACK);
  DrawTextRect(152, 0, g.sw - 160, TOP_BAR_H, title, ALIGN_LEFT | VALIGN_MIDDLE);
}

static void render_pause_options(void) {
  int ph = pause_panel_h();
  int y0 = g.sh - ph;
  int info_h = PAUSE_INFO_H;
  int row_h = (ph - info_h) / PAUSE_OPT_COUNT;
  if (row_h < 42) row_h = 42;
  if (info_h + PAUSE_OPT_COUNT * row_h > ph) {
    info_h = ph - PAUSE_OPT_COUNT * row_h;
    if (info_h < 72) info_h = 72;
  }

  FillArea(0, y0, g.sw, ph, WHITE);
  DrawLine(0, y0, g.sw, y0, BLACK);

  int idx = progress_word_num();
  int pct = progress_percent();
  int chap = current_chapter_index();

  char title[96];
  book_title_short(title, sizeof(title));
  if (g.font_browse_title) SetFont(g.font_browse_title, BLACK);
  else if (g.font_browse) SetFont(g.font_browse, BLACK);
  DrawTextRect(20, y0 + 4, g.sw - 40, 28, title, ALIGN_LEFT | VALIGN_MIDDLE);

  /* Pasek postępu */
  int bar_x = 20;
  int bar_w = g.sw - 40;
  int bar_y = y0 + 36;
  int bar_h = 14;
  DrawRect(bar_x, bar_y, bar_w, bar_h, BLACK);
  if (g.word_count > 0) {
    int fill = (int)(((long)idx * (bar_w - 2)) / g.word_count);
    if (fill < 0) fill = 0;
    if (fill > bar_w - 2) fill = bar_w - 2;
    if (fill > 0) FillArea(bar_x + 1, bar_y + 1, fill, bar_h - 2, BLACK);
  }

  if (g.font_ui) SetFont(g.font_ui, BLACK);
  char line[128];
  if (idx <= 1) {
    snprintf(line, sizeof(line), "Gotowe · %d słów · %d sł/min", g.word_count, g.wpm);
  } else {
    snprintf(line, sizeof(line), "Wznowienie · %d / %d · %d%% · %d sł/min",
             idx, g.word_count, pct, g.wpm);
  }
  DrawTextRect(20, y0 + 54, g.sw - 40, 24, line, ALIGN_LEFT | VALIGN_MIDDLE);

  if (chap >= 0 && g.chapters[chap].title) {
    snprintf(line, sizeof(line), "Rozdz. %d: %s", chap + 1, g.chapters[chap].title);
  } else if (g.chapter_count > 0) {
    snprintf(line, sizeof(line), "Rozdziały: %d", g.chapter_count);
  } else {
    safe_strncpy(line, sizeof(line), "Brak spisu rozdziałów");
  }
  DrawTextRect(20, y0 + 78, g.sw - 40, 28, line, ALIGN_LEFT | VALIGN_MIDDLE);

  DrawLine(16, y0 + info_h - 1, g.sw - 16, y0 + info_h - 1, LGRAY);

  char play_label[64];
  if (idx <= 1) {
    snprintf(play_label, sizeof(play_label), "▶   Start");
  } else {
    snprintf(play_label, sizeof(play_label), "▶   Wznów (%d%%)", pct);
  }

  for (int i = 0; i < PAUSE_OPT_COUNT; i++) {
    int ry = y0 + info_h + i * row_h;
    if (g.font_browse) SetFont(g.font_browse, BLACK);

    if (i == PAUSE_OPT_PLAY) {
      DrawTextRect(24, ry, g.sw - 48, row_h, play_label, ALIGN_LEFT | VALIGN_MIDDLE);
    } else if (i == PAUSE_OPT_CHAPTERS) {
      DrawTextRect(24, ry, g.sw - 48, row_h, "≡   Rozdziały",
                   ALIGN_LEFT | VALIGN_MIDDLE);
    } else if (i == PAUSE_OPT_NAV) {
      int third = g.sw / 3;
      DrawTextRect(0, ry, third, row_h, "« rozdz.", ALIGN_CENTER | VALIGN_MIDDLE);
      DrawTextRect(third, ry, third, row_h, "Początek", ALIGN_CENTER | VALIGN_MIDDLE);
      DrawTextRect(2 * third, ry, g.sw - 2 * third, row_h, "rozdz. »",
                   ALIGN_CENTER | VALIGN_MIDDLE);
      DrawLine(third, ry + 8, third, ry + row_h - 8, LGRAY);
      DrawLine(2 * third, ry + 8, 2 * third, ry + row_h - 8, LGRAY);
    } else if (i == PAUSE_OPT_WPM) {
      int third = g.sw / 3;
      DrawTextRect(0, ry, third, row_h, "−10", ALIGN_CENTER | VALIGN_MIDDLE);
      char wpm_l[48];
      snprintf(wpm_l, sizeof(wpm_l), "%d sł/min", g.wpm);
      DrawTextRect(third, ry, third, row_h, wpm_l, ALIGN_CENTER | VALIGN_MIDDLE);
      DrawTextRect(2 * third, ry, g.sw - 2 * third, row_h, "+10",
                   ALIGN_CENTER | VALIGN_MIDDLE);
      DrawLine(third, ry + 10, third, ry + row_h - 10, LGRAY);
      DrawLine(2 * third, ry + 10, 2 * third, ry + row_h - 10, LGRAY);
    } else if (i == PAUSE_OPT_LEAVE) {
      DrawTextRect(24, ry, g.sw - 48, row_h, "‹   Inna książka",
                   ALIGN_LEFT | VALIGN_MIDDLE);
    }

    if (i < PAUSE_OPT_COUNT - 1) {
      DrawLine(20, ry + row_h - 1, g.sw - 20, ry + row_h - 1, LGRAY);
    }
  }

  PartialUpdate(0, y0, g.sw, ph);
}

static void render_control_bar(void) {
  if (!reader_chrome_on()) return;

  if (pause_options_on()) {
    render_pause_options();
    return;
  }

  if (reader_has_top_bar()) render_top_bar();

  int y0 = g.sh - CTRL_BAR_H;
  FillArea(0, y0, g.sw, CTRL_BAR_H, WHITE);
  DrawLine(0, y0, g.sw, y0, BLACK);

  if (g.reader_menu == READER_MENU_WPM) {
    if (g.font_browse_title) SetFont(g.font_browse_title, BLACK);
    char title[64];
    snprintf(title, sizeof(title), "%d", g.wpm);
    DrawTextRect(0, y0 + 8, g.sw, 40, title, ALIGN_CENTER | VALIGN_MIDDLE);
    if (g.font_ui) SetFont(g.font_ui, BLACK);
    DrawTextRect(0, y0 + 50, g.sw, 24, "słów na minutę · swipe ±10",
                 ALIGN_CENTER | VALIGN_MIDDLE);
    DrawTextRect(0, y0 + 90, g.sw, 28, "◄ −10      tap: gotowe      +10 ►",
                 ALIGN_CENTER | VALIGN_MIDDLE);
    PartialUpdate(0, 0, g.sw, TOP_BAR_H);
    PartialUpdate(0, y0, g.sw, CTRL_BAR_H);
    return;
  }

  PartialUpdate(0, y0, g.sw, CTRL_BAR_H);
}

static void render_word_at_idx(int word_idx) {
  if (g.word_count <= 0) return;
  if (word_idx < 0) word_idx = 0;
  if (word_idx >= g.word_count) word_idx = g.word_count - 1;
  word_idx = unit_start_idx(word_idx);

  char unit_text[UNIT_TEXT_MAX];
  format_unit_text(word_idx, unit_text, sizeof(unit_text));
  if (!unit_text[0]) return;

  int span = unit_word_span(word_idx);
  int orp_idx = word_idx + span - 1;
  if (orp_idx >= g.word_count) orp_idx = g.word_count - 1;

  int ctop = reader_content_top();
  int cbot = reader_content_bottom();
  int content_h = cbot - ctop;
  if (content_h < 40) content_h = 40;

  FillArea(0, ctop, g.sw, content_h, WHITE);

  if (g.font_word) SetFont(g.font_word, BLACK);

  char prefix[UNIT_TEXT_MAX];
  size_t used = 0;
  prefix[0] = '\0';
  for (int k = word_idx; k < orp_idx; k++) {
    const char *part = g.words[k].word;
    if (!part) continue;
    if (k > word_idx && used + 1 < sizeof(prefix)) prefix[used++] = ' ';
    while (*part && used + 1 < sizeof(prefix)) prefix[used++] = *part++;
  }
  if (orp_idx > word_idx && used + 1 < sizeof(prefix)) prefix[used++] = ' ';
  prefix[used] = '\0';

  const char *orp_word = g.words[orp_idx].word;
  int prefix_w = prefix[0] && g.font_word ? StringWidth(prefix) : 0;
  int orp_w = g.words[orp_idx].width_px;
  if (orp_w <= 0 && g.font_word && orp_word) orp_w = StringWidth((char *)orp_word);

  int orp_off = (orp_w * ORP_RATIO_NUM) / ORP_RATIO_DEN;
  int cx = g.sw / 2;
  int x = cx - prefix_w - orp_off;
  int y = ctop + (content_h / 2) - (g.word_text_h / 2);
  if (y < ctop + 8) y = ctop + 8;

  draw_orp_guides(cx, y, g.word_text_h);
  DrawString(x, y, unit_text);

  g.display_word_idx = word_idx;
  g.last_rect_x = 0;
  g.last_rect_y = ctop;
  g.last_rect_w = g.sw;
  g.last_rect_h = content_h;
  g.rect_valid = 1;

  PartialUpdate(0, ctop, g.sw, content_h);
}

static void render_word_at_preview(void) {
  render_word_at_idx(get_preview_idx());
}

static void render_footer(void) {
  if (g.word_count <= 0) return;
  render_control_bar();
}

static void render_reader_full(void) {
  if (g.word_count <= 0) return;
  clear_wpm_badge();
  ClearScreen();
  if (reader_has_top_bar()) {
    render_top_bar();
  }
  render_word_at_preview();
  render_control_bar();
  FullUpdate();
}

static void show_reader_chrome(void) {
  g.chrome_visible = 1;
  if (g.playing) {
    g.playing = 0;
    stop_playback_timer();
  }
  save_progress();
  render_reader_full();
}

static void leave_book_to_browser(void) {
  save_progress();
  save_wpm();
  stop_playback_timer();
  clear_wpm_badge();
  g.playing = 0;
  g.reader_menu = READER_MENU_NONE;
  free_book_data();
  if (g.epub_path) {
    free(g.epub_path);
    g.epub_path = NULL;
  }
  g.file_selected = 0;
  show_browser_list();
}

static void jump_to_chapter(int chap_idx) {
  if (chap_idx < 0 || chap_idx >= g.chapter_count) return;
  if (g.playing) {
    g.playing = 0;
    stop_playback_timer();
  }
  g.next_word_idx = unit_start_idx(g.chapters[chap_idx].word_idx);
  if (g.next_word_idx < 0) g.next_word_idx = 0;
  if (g.next_word_idx > g.word_count) g.next_word_idx = g.word_count;
  g.reader_menu = READER_MENU_NONE;
  g.chrome_visible = 1;
  save_progress();
  render_reader_full();
}

static void jump_to_book_start(void) {
  if (g.playing) {
    g.playing = 0;
    stop_playback_timer();
  }
  g.next_word_idx = 0;
  g.reader_menu = READER_MENU_NONE;
  g.chrome_visible = 1;
  save_progress();
  render_reader_full();
}

static void jump_relative_chapter(int delta) {
  if (g.chapter_count <= 0) {
    Message(ICON_INFORMATION, APP_DISPLAY_NAME, "Brak listy rozdziałów w tej książce.", 3000);
    return;
  }
  int cur = current_chapter_index();
  if (cur < 0) cur = 0;
  int next = cur + delta;
  if (next < 0) next = 0;
  if (next >= g.chapter_count) next = g.chapter_count - 1;
  jump_to_chapter(next);
}

static int chapter_list_rows(void) {
  int body = g.sh - g.browse_header_h - CTRL_BAR_H;
  int row_h = g.browse_row_h > 0 ? g.browse_row_h : BROWSE_ROW_H;
  if (body < row_h) return 1;
  return body / row_h;
}

static void draw_chapter_picker(void) {
  ClearScreen();
  if (g.browse_row_h <= 0) g.browse_row_h = BROWSE_ROW_H;
  if (g.browse_header_h <= 0) g.browse_header_h = BROWSE_HEADER_H;

  if (g.font_browse_title) SetFont(g.font_browse_title, BLACK);
  FillArea(0, 0, g.sw, g.browse_header_h, WHITE);
  DrawTextRect(20, 0, g.sw - 40, g.browse_header_h, "Rozdziały",
               ALIGN_LEFT | VALIGN_MIDDLE);
  DrawLine(0, g.browse_header_h - 2, g.sw, g.browse_header_h - 2, BLACK);

  int rows = chapter_list_rows();
  if (g.chapter_sel < 0) g.chapter_sel = 0;
  if (g.chapter_count > 0 && g.chapter_sel >= g.chapter_count)
    g.chapter_sel = g.chapter_count - 1;
  if (g.chapter_sel < g.chapter_scroll) g.chapter_scroll = g.chapter_sel;
  if (g.chapter_sel >= g.chapter_scroll + rows)
    g.chapter_scroll = g.chapter_sel - rows + 1;
  if (g.chapter_scroll < 0) g.chapter_scroll = 0;

  if (g.font_browse) SetFont(g.font_browse, BLACK);
  for (int i = 0; i < rows; i++) {
    int idx = g.chapter_scroll + i;
    if (idx >= g.chapter_count) break;
    int y = g.browse_header_h + i * g.browse_row_h;
    int selected = (idx == g.chapter_sel);
    if (selected) {
      FillArea(0, y, g.sw, g.browse_row_h, BLACK);
      if (g.font_browse) SetFont(g.font_browse, WHITE);
    } else {
      FillArea(0, y, g.sw, g.browse_row_h, WHITE);
      if (g.font_browse) SetFont(g.font_browse, BLACK);
    }
    const char *t = g.chapters[idx].title ? g.chapters[idx].title : "?";
    char line[160];
    snprintf(line, sizeof(line), "%d.  %s", idx + 1, t);
    DrawTextRect(24, y, g.sw - 48, g.browse_row_h, line,
                 ALIGN_LEFT | VALIGN_MIDDLE);
    if (!selected)
      DrawLine(20, y + g.browse_row_h - 1, g.sw - 20, y + g.browse_row_h - 1, LGRAY);
  }

  int fy = g.sh - CTRL_BAR_H;
  FillArea(0, fy, g.sw, CTRL_BAR_H, WHITE);
  DrawLine(0, fy, g.sw, fy, BLACK);
  if (g.font_ui) SetFont(g.font_ui, BLACK);
  DrawTextRect(12, fy, g.sw - 24, CTRL_BAR_H,
               "tap: skocz   ◄►: wybór   MENU: wstecz",
               ALIGN_CENTER | VALIGN_MIDDLE);
  FullUpdate();
}

/* Dolny panel WPM (gdy READER_MENU_WPM) — top bar zostaje */
static void menu_redraw(void) {
  render_control_bar();
}

static void open_menu(void) {
  /* Jak tap u góry w PB — pokaż chrome */
  show_reader_chrome();
}

static void close_menu(void) {
  g.reader_menu = READER_MENU_NONE;
  g.chrome_visible = 1;
  g.playing = 0;
  render_reader_full();
}

static void open_chapter_picker(void) {
  if (g.chapter_count <= 0) {
    Message(ICON_INFORMATION, APP_DISPLAY_NAME, "Brak listy rozdziałów w tej książce.", 4000);
    g.reader_menu = READER_MENU_NONE;
    g.chrome_visible = 1;
    render_reader_full();
    return;
  }
  if (g.playing) {
    g.playing = 0;
    stop_playback_timer();
  }
  save_progress();
  g.reader_menu = READER_MENU_CHAPTERS;
  g.chrome_visible = 0;
  g.chapter_scroll = 0;
  g.chapter_sel = 0;
  for (int i = g.chapter_count - 1; i >= 0; i--) {
    if (g.chapters[i].word_idx <= g.next_word_idx) {
      g.chapter_sel = i;
      break;
    }
  }
  draw_chapter_picker();
}

static void clamp_wpm(void) {
  if (g.wpm < WPM_MIN) g.wpm = WPM_MIN;
  if (g.wpm > WPM_MAX) g.wpm = WPM_MAX;
}

static void wpm_badge_timer(void) {
  if (!g.wpm_badge_on) return;
  g.wpm_badge_on = 0;
  /* Odśwież obszar odznaki — podczas play tylko słowo */
  if (g.playing && g.word_count > 0) {
    int idx = g.display_word_idx;
    if (idx < 0) idx = 0;
    if (idx >= g.word_count) idx = g.word_count - 1;
    render_word_at_idx(idx);
  }
}

static void clear_wpm_badge(void) {
  if (g.wpm_badge_on) {
    g.wpm_badge_on = 0;
    ClearTimer(wpm_badge_timer);
  }
}

static void show_wpm_badge(void) {
  int bw = 180;
  int bh = 48;
  int bx = (g.sw - bw) / 2;
  int by = 16;
  if (by < 8) by = 8;

  FillArea(bx, by, bw, bh, WHITE);
  DrawRect(bx, by, bw, bh, BLACK);
  if (g.font_browse) SetFont(g.font_browse, BLACK);
  else if (g.font_ui) SetFont(g.font_ui, BLACK);
  char t[48];
  snprintf(t, sizeof(t), "%d sł/min", g.wpm);
  DrawTextRect(bx, by, bw, bh, t, ALIGN_CENTER | VALIGN_MIDDLE);
  PartialUpdate(bx, by, bw, bh);

  g.wpm_badge_on = 1;
  ClearTimer(wpm_badge_timer);
  SetWeakTimer(WPM_BADGE_TIMER, wpm_badge_timer, WPM_BADGE_MS);
}

// Playback & timer
static void timer_proc(void);
static int g_play_ticks;

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static int ms_per_word(void) {
  int wpm = g.wpm > 0 ? g.wpm : WPM_DEFAULT;
  return 60000 / wpm;
}

static int unit_display_ms(int span) {
  int target;
  if (span < 1) span = 1;
  target = ms_per_word() * span;
  if (target < EINK_MIN_WORD_MS) target = EINK_MIN_WORD_MS;
  return target;
}

static int playback_delay_ms(int span, long long render_start_ms) {
  int target = unit_display_ms(span);
  int elapsed = (int)(now_ms() - render_start_ms);
  int wait = target - elapsed;
  if (wait < 1) wait = 1; /* SetWeakTimer(0) is unreliable on InkView */
  return wait;
}

static void stop_playback_timer(void) {
  ClearTimer(timer_proc);
}

static void arm_playback_timer(void) {
  int span = unit_word_span(g.next_word_idx);
  /* SetWeakTimer: jak w CoolReader — one-shot, przeładowywany w callbacku */
  SetWeakTimer(RSVP_TIMER_NAME, timer_proc, unit_display_ms(span));
  SetAutoPowerOff(0); /* nie usypiaj podczas czytania */
}

static void restart_timer(void) {
  stop_playback_timer();
  if (g.playing && !g.reader_menu && !g.browse_active && g.word_count > 0) {
    arm_playback_timer();
  }
}

static void set_playing(int enable, int from_menu) {
  (void)from_menu;
  if (g.reader_menu == READER_MENU_WPM) {
    g.reader_menu = READER_MENU_NONE;
  }
  if (g.reader_menu == READER_MENU_CHAPTERS) return;

  if (enable) {
    int shown_span;
    if (g.next_word_idx >= g.word_count && g.word_count > 0) {
      g.next_word_idx = 0;
    }
    clear_wpm_badge();
    g.playing = 1;
    g.chrome_visible = 0;
    g_play_ticks = 0;
    g.words_since_full = 0;
    ClearScreen();
    FullUpdate();
    /* Delay until next frame must match the unit we just painted, not the next one. */
    shown_span = unit_word_span(unit_start_idx(g.next_word_idx));
    if (shown_span < 1) shown_span = 1;
    advance_and_render_one_word();
    if (g.playing) {
      SetWeakTimer(RSVP_TIMER_NAME, timer_proc, unit_display_ms(shown_span));
      SetAutoPowerOff(0);
    }
    return;
  }

  save_progress();
  save_wpm();
  g.playing = 0;
  g.chrome_visible = 1;
  stop_playback_timer();
  SetAutoPowerOff(1);
  render_reader_full();
}

static void advance_and_render_one_word(void) {
  if (g.word_count <= 0) return;
  if (g.next_word_idx >= g.word_count) {
    g.playing = 0;
    g.chrome_visible = 1;
    stop_playback_timer();
    save_progress();
    SetAutoPowerOff(1);
    render_reader_full();
    return;
  }

  int idx = unit_start_idx(g.next_word_idx);
  int span = unit_word_span(idx);
  if (span < 1) span = 1;

  render_word_at_idx(idx);
  g.next_word_idx = idx + span;
  g_play_ticks++;
  g.words_since_full++;

  /* Okresowe pełne odświeżenie — mniej ghostingu na B300 */
  if (g.playing && g.words_since_full >= FULL_REFRESH_EVERY) {
    g.words_since_full = 0;
    FullUpdate();
  }

  if (g.next_word_idx >= g.word_count) {
    g.playing = 0;
    g.chrome_visible = 1;
    stop_playback_timer();
    save_progress();
    SetAutoPowerOff(1);
    render_reader_full();
  }
}

static void timer_proc(void) {
  long long render_start;
  int span;

  if (!g.playing || g.reader_menu || g.browse_active || g.word_count <= 0) {
    return;
  }
  span = unit_word_span(g.next_word_idx);
  if (span < 1) span = 1;
  render_start = now_ms();
  advance_and_render_one_word();
  if (g.playing) {
    SetWeakTimer(RSVP_TIMER_NAME, timer_proc, playback_delay_ms(span, render_start));
  }
}

// Progress persistence: ~/.rsvp_saves.ini (na PocketBooku w FLASHDIR)
static const char *save_file_path(void) {
  static char p[256];
  snprintf(p, sizeof(p), "%s/%s", FLASHDIR, SAVE_FILE_NAME);
  return p;
}

static void load_wpm(void) {
  FILE *f = iv_fopen(save_file_path(), "r");
  if (!f) return;

  char line[SAVE_LINE_MAX];
  while (iv_fgets(line, (int)sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    char *e = val + strlen(val);
    while (e > val && (*(e - 1) == '\n' || *(e - 1) == '\r')) {
      *(e - 1) = '\0';
      e--;
    }
    if (strcmp(key, INI_KEY_WPM) == 0) {
      int w = atoi(val);
      if (w > 0) {
        g.wpm = w;
        clamp_wpm();
      }
      break;
    }
  }
  iv_fclose(f);
}

static void load_progress_and_set_next_index(void) {
  g.next_word_idx = 0;
  if (!g.epub_path || g.word_count <= 0) return;

  FILE *f = iv_fopen(save_file_path(), "r");
  if (!f) return;

  char line[SAVE_LINE_MAX];
  while (iv_fgets(line, (int)sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;

    char *e = val + strlen(val);
    while (e > val && (*(e - 1) == '\n' || *(e - 1) == '\r')) {
      *(e - 1) = '\0';
      e--;
    }

    if (strcmp(key, g.epub_path) == 0) {
      int idx = atoi(val);
      if (idx < 0) idx = 0;
      if (idx >= g.word_count) idx = g.word_count - 1;
      g.next_word_idx = unit_start_idx(idx);
      break;
    }
  }
  iv_fclose(f);
}

/* Zapis INI: zachowaj inne klucze, zaktualizuj path=idx oraz opcjonalnie wpm= */
static void save_ini_keys(const char *path_key, int word_idx, int also_wpm) {
  const char *path = save_file_path();
  char tmp_path[320];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  FILE *in = iv_fopen(path, "r");
  FILE *out = iv_fopen(tmp_path, "w");
  if (!out) {
    if (in) iv_fclose(in);
    return;
  }

  int updated_path = (path_key == NULL);
  int updated_wpm = !also_wpm;

  if (in) {
    char line[SAVE_LINE_MAX];
    while (iv_fgets(line, (int)sizeof(line), in)) {
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      char *key = line;
      char *rest = eq + 1;

      if (path_key && strcmp(key, path_key) == 0) {
        char buf[SAVE_LINE_MAX];
        int n = snprintf(buf, sizeof(buf), "%s=%d\n", path_key, word_idx);
        if (n > 0 && n < (int)sizeof(buf)) {
          iv_fwrite(buf, 1, (int)strlen(buf), out);
        }
        updated_path = 1;
      } else if (also_wpm && strcmp(key, INI_KEY_WPM) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s=%d\n", INI_KEY_WPM, g.wpm);
        iv_fwrite(buf, 1, (int)strlen(buf), out);
        updated_wpm = 1;
      } else {
        iv_fwrite(key, 1, (int)strlen(key), out);
        iv_fwrite("=", 1, 1, out);
        iv_fwrite(rest, 1, (int)strlen(rest), out);
      }
    }
    iv_fclose(in);
  }

  if (!updated_path && path_key) {
    char buf[SAVE_LINE_MAX];
    int n = snprintf(buf, sizeof(buf), "%s=%d\n", path_key, word_idx);
    if (n > 0 && n < (int)sizeof(buf)) {
      iv_fwrite(buf, 1, (int)strlen(buf), out);
    }
  }
  if (!updated_wpm && also_wpm) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%d\n", INI_KEY_WPM, g.wpm);
    iv_fwrite(buf, 1, (int)strlen(buf), out);
  }

  iv_fclose(out);
  iv_rename(tmp_path, path);
}

static void save_progress(void) {
  if (!g.epub_path || g.word_count <= 0) return;

  int idx_to_save = g.next_word_idx;
  if (idx_to_save < 0) idx_to_save = 0;
  if (idx_to_save >= g.word_count) idx_to_save = g.word_count - 1;

  save_ini_keys(g.epub_path, idx_to_save, 1);
}

static void save_wpm(void) {
  clamp_wpm();
  save_ini_keys(NULL, 0, 1);
}

// Mini-eksplorator: katalogi + pliki .epub (własny UI pod dotyk)
#define BROWSE_MAX_ENTRIES 500
#define BROWSE_ROOT "/mnt/ext1"

enum {
  BROWSE_KIND_PARENT = 0,
  BROWSE_KIND_DIR = 1,
  BROWSE_KIND_EPUB = 2
};

typedef struct {
  int kind;
  char *label;
  char *fullpath;
} BrowseEntry;

static char g_browse_cwd[1024];
static BrowseEntry *g_browse_entries;
static int g_browse_count;
static int g_browse_cap;

static int path_is_skipped_dir(const char *name) {
  if (!name || name[0] == '\0') return 1;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 1;
  if (name[0] == '.') return 1;
  if (strcasecmp(name, "system") == 0) return 1;
  if (strcmp(name, "Digital Editions") == 0) return 1;
  if (strcmp(name, "Adobe Digital Editions") == 0) return 1;
  if (strcasecmp(name, "applications") == 0) return 1;
  if (strcasecmp(name, "photo") == 0) return 1;
  if (strcasecmp(name, "music") == 0) return 1;
  if (strcasecmp(name, "playlists") == 0) return 1;
  if (strcasecmp(name, "current") == 0) return 1;
  return 0;
}

static void free_browse_entries(void) {
  if (g_browse_entries) {
    for (int i = 0; i < g_browse_count; i++) {
      free(g_browse_entries[i].label);
      free(g_browse_entries[i].fullpath);
    }
    free(g_browse_entries);
  }
  g_browse_entries = NULL;
  g_browse_count = 0;
  g_browse_cap = 0;
}

static int browse_ensure_cap(int need) {
  if (need <= g_browse_cap) return 1;
  int ncap = g_browse_cap ? g_browse_cap : 64;
  while (ncap < need) ncap *= 2;
  BrowseEntry *n = (BrowseEntry *)realloc(g_browse_entries, (size_t)ncap * sizeof(BrowseEntry));
  if (!n) return 0;
  g_browse_entries = n;
  g_browse_cap = ncap;
  return 1;
}

static void browse_add(int kind, const char *label, const char *fullpath) {
  if (g_browse_count >= BROWSE_MAX_ENTRIES) return;
  if (!browse_ensure_cap(g_browse_count + 1)) return;
  char *lb = (char *)malloc(strlen(label) + 1);
  if (!lb) return;
  strcpy(lb, label);
  char *fp = NULL;
  if (fullpath) {
    fp = (char *)malloc(strlen(fullpath) + 1);
    if (!fp) {
      free(lb);
      return;
    }
    strcpy(fp, fullpath);
  }
  g_browse_entries[g_browse_count].kind = kind;
  g_browse_entries[g_browse_count].label = lb;
  g_browse_entries[g_browse_count].fullpath = fp;
  g_browse_count++;
}

static int path_is_directory(const char *full, const char *name) {
  if (ends_with_caseinsensitive(name, EPUB_EXT) ||
      ends_with_caseinsensitive(name, ".fb2") ||
      ends_with_caseinsensitive(name, ".zip") ||
      ends_with_caseinsensitive(name, ".pdf") ||
      ends_with_caseinsensitive(name, ".djvu")) {
    return 0;
  }

  struct stat st;
  memset(&st, 0, sizeof(st));
  if (iv_stat(full, &st) == 0) return S_ISDIR(st.st_mode) ? 1 : 0;
  memset(&st, 0, sizeof(st));
  if (stat(full, &st) == 0) return S_ISDIR(st.st_mode) ? 1 : 0;

  DIR *d = opendir(full);
  if (d) {
    closedir(d);
    return 1;
  }
  d = iv_opendir(full);
  if (d) {
    iv_closedir(d);
    return 1;
  }
  return 0;
}

static int browse_cwd_is_root(void) {
  return strcmp(g_browse_cwd, BROWSE_ROOT) == 0 ||
         strcmp(g_browse_cwd, "/") == 0 ||
         strcmp(g_browse_cwd, FLASHDIR) == 0;
}

static const char *browse_title_path(void) {
  if (strncmp(g_browse_cwd, BROWSE_ROOT, strlen(BROWSE_ROOT)) == 0) {
    const char *rest = g_browse_cwd + strlen(BROWSE_ROOT);
    if (*rest == '\0' || strcmp(rest, "/") == 0) return "/";
    if (*rest == '/') return rest;
  }
  if (strncmp(g_browse_cwd, FLASHDIR, strlen(FLASHDIR)) == 0) {
    const char *rest = g_browse_cwd + strlen(FLASHDIR);
    if (*rest == '\0' || strcmp(rest, "/") == 0) return "/";
    if (*rest == '/') return rest;
  }
  return g_browse_cwd;
}

static void browse_parent_path(char *out, size_t outsz) {
  safe_strncpy(out, outsz, g_browse_cwd);
  char *slash = strrchr(out, '/');
  if (!slash) {
    safe_strncpy(out, outsz, BROWSE_ROOT);
    return;
  }
  if (slash == out) {
    out[1] = '\0';
    return;
  }
  *slash = '\0';
  if (out[0] == '\0') safe_strncpy(out, outsz, BROWSE_ROOT);
}

static void show_browser_list(void);
static void start_from_selected_file_if_ready(void);

static int browse_entry_cmp(const void *a, const void *b) {
  const BrowseEntry *ea = (const BrowseEntry *)a;
  const BrowseEntry *eb = (const BrowseEntry *)b;
  if (ea->kind != eb->kind) return ea->kind - eb->kind;
  const char *la = ea->label ? ea->label : "";
  const char *lb = eb->label ? eb->label : "";
  return strcasecmp(la, lb);
}

static int browse_footer_h(void) {
  int ph = PanelHeight();
  int need = BROWSE_FOOTER_H + ((ph > 0) ? ph : 0);
  return need;
}

static int browse_visible_rows(void) {
  int body = g.sh - g.browse_header_h - browse_footer_h();
  if (body < g.browse_row_h) return 1;
  return body / g.browse_row_h;
}

static void browse_clamp_sel(void) {
  if (g_browse_count <= 0) {
    g.browse_sel = 0;
    return;
  }
  if (g.browse_sel < 0) g.browse_sel = 0;
  if (g.browse_sel >= g_browse_count) g.browse_sel = g_browse_count - 1;
}

static void browse_ensure_sel_visible(void) {
  int rows = browse_visible_rows();
  browse_clamp_sel();
  if (g.browse_sel < g.browse_scroll) g.browse_scroll = g.browse_sel;
  if (g.browse_sel >= g.browse_scroll + rows) {
    g.browse_scroll = g.browse_sel - rows + 1;
  }
  if (g.browse_scroll < 0) g.browse_scroll = 0;
}

static void draw_browser(void) {
  ClearScreen();

  if (g.browse_row_h <= 0) g.browse_row_h = BROWSE_ROW_H;
  if (g.browse_header_h <= 0) g.browse_header_h = BROWSE_HEADER_H;

  browse_clamp_sel();
  browse_ensure_sel_visible();

  int footer = browse_footer_h();
  int rows = browse_visible_rows();
  if (g_browse_count > rows) {
    int maxs = g_browse_count - rows;
    if (g.browse_scroll > maxs) g.browse_scroll = maxs;
  } else {
    g.browse_scroll = 0;
  }

  FillArea(0, 0, g.sw, g.browse_header_h, WHITE);
  if (g.font_browse_title) SetFont(g.font_browse_title, BLACK);
  else if (g.font_browse) SetFont(g.font_browse, BLACK);

  const char *path = browse_title_path();
  const char *folder_name = path;
  if (strcmp(path, "/") == 0) {
    folder_name = "Pamięć urządzenia";
  } else {
    const char *slash = strrchr(path, '/');
    if (slash && slash[1]) folder_name = slash + 1;
    else if (path[0] == '/' && path[1]) folder_name = path + 1;
  }

  DrawTextRect(20, 4, g.sw - 40, g.browse_header_h / 2 + 4,
               folder_name, ALIGN_LEFT | VALIGN_MIDDLE);
  if (g.font_ui) SetFont(g.font_ui, DGRAY);
  DrawTextRect(20, g.browse_header_h / 2 + 2, g.sw - 40, g.browse_header_h / 2 - 6,
               "Dotknij albo użyj przycisków na dole", ALIGN_LEFT | VALIGN_MIDDLE);
  DrawLine(0, g.browse_header_h - 2, g.sw, g.browse_header_h - 2, BLACK);

  for (int i = 0; i < rows; i++) {
    int idx = g.browse_scroll + i;
    if (idx >= g_browse_count) break;
    int y = g.browse_header_h + i * g.browse_row_h;
    BrowseEntry *e = &g_browse_entries[idx];
    const char *name = e->label ? e->label : "?";
    int selected = (idx == g.browse_sel);

    if (selected) {
      FillArea(0, y, g.sw, g.browse_row_h, BLACK);
      if (g.font_browse) SetFont(g.font_browse, WHITE);
      else if (g.font_ui) SetFont(g.font_ui, WHITE);
    } else {
      FillArea(0, y, g.sw, g.browse_row_h, WHITE);
      if (g.font_browse) SetFont(g.font_browse, BLACK);
      else if (g.font_ui) SetFont(g.font_ui, BLACK);
    }

    char left[320];
    if (e->kind == BROWSE_KIND_PARENT) {
      snprintf(left, sizeof(left), "‹  Wstecz");
    } else {
      snprintf(left, sizeof(left), "%s", name);
    }

    DrawTextRect(24, y, g.sw - (e->kind == BROWSE_KIND_DIR ? 64 : 36),
                 g.browse_row_h, left, ALIGN_LEFT | VALIGN_MIDDLE);

    if (e->kind == BROWSE_KIND_DIR) {
      DrawTextRect(g.sw - 52, y, 40, g.browse_row_h, "›",
                   ALIGN_CENTER | VALIGN_MIDDLE);
    }

    if (!selected) {
      DrawLine(20, y + g.browse_row_h - 1, g.sw - 20, y + g.browse_row_h - 1, LGRAY);
    }
  }

  int fy = g.sh - footer;
  FillArea(0, fy, g.sw, footer, WHITE);
  DrawLine(0, fy, g.sw, fy, BLACK);
  if (g.font_ui) SetFont(g.font_ui, BLACK);

  char hint[128];
  if (g_browse_count <= 0) {
    snprintf(hint, sizeof(hint), "Pusty folder · Home = wyjście");
  } else {
    snprintf(hint, sizeof(hint),
             "◄► wybór   MENU otwórz   Home wyjście");
  }
  DrawTextRect(12, fy, g.sw - 24, BROWSE_FOOTER_H, hint,
               ALIGN_CENTER | VALIGN_MIDDLE);

  DrawPanel(NULL, "", APP_DISPLAY_NAME, -1);
  FullUpdate();
}

static void browse_activate_index(int idx) {
  if (idx < 0 || idx >= g_browse_count) return;
  BrowseEntry *e = &g_browse_entries[idx];

  if (e->kind == BROWSE_KIND_PARENT) {
    char parent[1024];
    browse_parent_path(parent, sizeof(parent));
    safe_strncpy(g_browse_cwd, sizeof(g_browse_cwd), parent);
    g.browse_scroll = 0;
    g.browse_sel = 0;
    show_browser_list();
    return;
  }
  if (e->kind == BROWSE_KIND_DIR && e->fullpath) {
    safe_strncpy(g_browse_cwd, sizeof(g_browse_cwd), e->fullpath);
    g.browse_scroll = 0;
    g.browse_sel = 0;
    show_browser_list();
    return;
  }
  if (e->kind == BROWSE_KIND_EPUB && e->fullpath) {
    safe_strncpy(g.selected_epub_path, sizeof(g.selected_epub_path), e->fullpath);
    g.file_selected = 1;
    g.browse_active = 0;
    start_from_selected_file_if_ready();
  }
}

static void show_browser_list(void) {
  free_browse_entries();

  g.browse_row_h = BROWSE_ROW_H;
  g.browse_header_h = BROWSE_HEADER_H;
  if (g.browse_text_h > 0 && g.browse_text_h + 28 > g.browse_row_h) {
    g.browse_row_h = g.browse_text_h + 28;
  }

  ShowHourglass();
  SetPanelType(1);

  if (!browse_cwd_is_root()) {
    browse_add(BROWSE_KIND_PARENT, "Wstecz", NULL);
  }

  int used_iv = 0;
  DIR *d = opendir(g_browse_cwd);
  if (!d) {
    d = iv_opendir(g_browse_cwd);
    if (d) used_iv = 1;
  }

  if (!d) {
    HideHourglass();
    g.browse_active = 1;
    g.browse_row_h = BROWSE_ROW_H;
    g.browse_header_h = BROWSE_HEADER_H;
    ClearScreen();
    if (g.font_browse) SetFont(g.font_browse, BLACK);
    DrawTextRect(24, 80, g.sw - 48, 160,
                 "Nie można otworzyć katalogu.\n\n"
                 "Odłącz USB (PC Link)\ni naciśnij MENU.",
                 ALIGN_CENTER | VALIGN_TOP);
    DrawPanel(NULL, "", APP_DISPLAY_NAME, -1);
    FullUpdate();
    return;
  }

  struct dirent *ent;
  while ((ent = used_iv ? iv_readdir(d) : readdir(d)) != NULL) {
    if (ent->d_name[0] == '\0') continue;
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

    char full[1024];
    int n = snprintf(full, sizeof(full), "%s/%s", g_browse_cwd, ent->d_name);
    if (n <= 0 || n >= (int)sizeof(full)) continue;

    if (ends_with_caseinsensitive(ent->d_name, EPUB_EXT)) {
      browse_add(BROWSE_KIND_EPUB, ent->d_name, full);
      continue;
    }

    if (path_is_skipped_dir(ent->d_name)) continue;

#ifdef DT_DIR
    if (ent->d_type == DT_REG) continue;
    if (ent->d_type == DT_DIR) {
      browse_add(BROWSE_KIND_DIR, ent->d_name, full);
      continue;
    }
#endif

    if (path_is_directory(full, ent->d_name)) {
      browse_add(BROWSE_KIND_DIR, ent->d_name, full);
    }
  }

  if (used_iv) iv_closedir(d);
  else closedir(d);

  /* Sortuj: Wstecz, foldery A–Z, EPUB A–Z */
  if (g_browse_count > 1) {
    qsort(g_browse_entries, (size_t)g_browse_count, sizeof(BrowseEntry), browse_entry_cmp);
  }

  HideHourglass();
  g.browse_active = 1;
  g.file_dialog_opened = 1;
  if (g.browse_sel < 0 || g.browse_sel >= g_browse_count) g.browse_sel = 0;
  draw_browser();
}

/* Start eksploratora w /mnt/ext1 */
static void OpenFileDialog(void) {
  g.file_selected = 0;
  g.selected_epub_path[0] = '\0';
  free_browse_entries();
  g.browse_scroll = 0;
  g.browse_sel = 0;

  safe_strncpy(g_browse_cwd, sizeof(g_browse_cwd), BROWSE_ROOT);
  DIR *probe = opendir(BROWSE_ROOT);
  int probe_iv = 0;
  if (!probe) {
    probe = iv_opendir(BROWSE_ROOT);
    if (probe) probe_iv = 1;
  }
  if (probe) {
    if (probe_iv) iv_closedir(probe);
    else closedir(probe);
  } else {
    safe_strncpy(g_browse_cwd, sizeof(g_browse_cwd), FLASHDIR);
  }

  show_browser_list();
}

static void draw_splash(void) {
  ClearScreen();
  SetPanelType(0);

  if (g.font_word) {
    SetFont(g.font_word, BLACK);
  } else if (g.font_browse) {
    SetFont(g.font_browse, BLACK);
  } else if (g.font_ui) {
    SetFont(g.font_ui, BLACK);
  }

  int mid = g.sh / 2;
  DrawTextRect(20, mid - 90, g.sw - 40, 70, APP_DISPLAY_NAME,
               ALIGN_CENTER | VALIGN_MIDDLE);

  if (g.font_ui) SetFont(g.font_ui, BLACK);
  else if (g.font_browse) SetFont(g.font_browse, BLACK);
  DrawTextRect(24, mid + 10, g.sw - 48, 80, APP_CREDIT,
               ALIGN_CENTER | VALIGN_TOP);

  FullUpdate();
}

static void splash_finish(void) {
  if (!g.splash_active) return;
  g.splash_active = 0;
  ClearTimer(splash_timer);
  SetPanelType(1);
  OpenFileDialog();
}

static void splash_timer(void) {
  splash_finish();
}

static void start_splash(void) {
  g.splash_active = 1;
  draw_splash();
  /* SetWeakTimer jak w reszcie apki; HardTimer (inna nazwa) jako zapas */
  SetWeakTimer(SPLASH_TIMER_NAME, splash_timer, SPLASH_MS);
  SetHardTimer(SPLASH_TIMER_NAME "_h", splash_timer, SPLASH_MS);
}

static void start_from_selected_file_if_ready(void) {
  if (!g.file_selected) return;

  free_browse_entries();
  g.browse_active = 0;

  if (g.epub_path) {
    free(g.epub_path);
    g.epub_path = NULL;
  }
  g.epub_path = (char *)malloc(strlen(g.selected_epub_path) + 1);
  if (!g.epub_path) {
    Message(ICON_ERROR, APP_DISPLAY_NAME, "Brak pamięci — nie można otworzyć książki.", 5000);
    g.file_selected = 0;
    show_browser_list();
    return;
  }
  strcpy(g.epub_path, g.selected_epub_path);

  ClearScreen();
  if (g.font_ui) SetFont(g.font_ui, BLACK);
  DrawTextRect(24, g.sh / 2 - 40, g.sw - 48, 80, "Wczytywanie EPUB…",
               ALIGN_CENTER | VALIGN_MIDDLE);
  FullUpdate();

  if (!parse_epub_to_words(g.epub_path)) {
    const char *msg = "Nie udało się odczytać EPUB.\n"
                      "Spróbuj innej książki.\n"
                      "MENU wraca do listy.";
    if (g.last_parse_err == PARSE_ERR_OOM) {
      msg = "Brak pamięci podczas wczytywania.\n"
            "Książka jest zbyt duża lub urządzenie ma mało RAM.";
    } else if (g.last_parse_err == PARSE_ERR_EMPTY) {
      msg = "Brak tekstu w EPUB\n"
            "(pusty plik albo tylko multimedia).";
    } else if (g.last_parse_err == PARSE_ERR_TRUNC) {
      msg = "Nie udało się wczytać całej książki\n"
            "(brak pamięci — przerwano, by uniknąć uciętego tekstu).";
    } else if (g.last_parse_err == PARSE_ERR_OPEN) {
      msg = "Nie udało się otworzyć pliku EPUB.\n"
            "Sprawdź, czy USB jest odłączone.";
    }
    Message(ICON_ERROR, APP_DISPLAY_NAME, msg, 6000);
    g.file_selected = 0;
    free(g.epub_path);
    g.epub_path = NULL;
    show_browser_list();
    return;
  }

  load_progress_and_set_next_index();
  load_wpm();
  g.playing = 0;
  g.reader_menu = READER_MENU_NONE;
  g.chrome_visible = 1;
  g.browse_active = 0;

  SetPanelType(0);

  stop_playback_timer();
  /* Soft start: panel pauzy z postępem / WPM — Start lub Wznów po tapnięciu */
  render_reader_full();

  g.file_selected = 0;
  g.file_dialog_opened = 0;
}

// Event handler
static int main_handler(int type, int par1, int par2) {
  if (type == EVT_INIT) {
    memset(&g, 0, sizeof(g));

    g.sw = ScreenWidth();
    g.sh = ScreenHeight();

    g.wpm = WPM_DEFAULT;
    load_wpm();
    g.font_word = OpenFont(DEFAULTFONT, WORD_FONT_SIZE, 1);
    g.font_ui = OpenFont(DEFAULTFONT, UI_FONT_SIZE, 1);
    g.font_browse = OpenFont(DEFAULTFONT, BROWSE_FONT_SIZE, 1);
    g.font_browse_title = OpenFont(DEFAULTFONTB, BROWSE_TITLE_SIZE, 1);
    if (!g.font_browse_title) {
      g.font_browse_title = OpenFont(DEFAULTFONT, BROWSE_TITLE_SIZE, 1);
    }

    SetPanelType(1);

    if (!g.font_word || !g.font_ui) {
      Message(ICON_ERROR, APP_DISPLAY_NAME, "Nie udało się otworzyć fontów", 5000);
      /* Bez fontów — od razu przeglądarka (bez splash) */
      g.splash_active = 0;
    } else {
      SetFont(g.font_word, BLACK);
      g.word_text_h = TextRectHeight(g.sw, "Hg", 0);
      if (g.word_text_h <= 0) g.word_text_h = WORD_FONT_SIZE + 4;

      SetFont(g.font_ui, BLACK);
      g.ui_text_h = TextRectHeight(g.sw, "Hg", 0);
      if (g.ui_text_h <= 0) g.ui_text_h = UI_FONT_SIZE + 2;

      if (g.font_browse) {
        SetFont(g.font_browse, BLACK);
        g.browse_text_h = TextRectHeight(g.sw, "Hg", 0);
        if (g.browse_text_h <= 0) g.browse_text_h = BROWSE_FONT_SIZE + 6;
      } else {
        g.font_browse = g.font_ui;
        g.browse_text_h = g.ui_text_h;
      }

      start_splash();
    }

    return 0;
  }

  if (type == EVT_SHOW) {
    if (g.splash_active) {
      draw_splash();
      return 0;
    }
    if (!g.file_dialog_opened && g.word_count <= 0) {
      g.file_dialog_opened = 1;
      OpenFileDialog();
    } else if (g.browse_active && g.word_count <= 0) {
      draw_browser();
    }

    start_from_selected_file_if_ready();
    return 0;
  }

  if (type == EVT_REPAINT) {
    if (g.splash_active) {
      draw_splash();
      return 0;
    }
    if (g.browse_active && g.word_count <= 0) {
      draw_browser();
      return 0;
    }
    if (g.word_count > 0 && !g.browse_active) {
      if (g.reader_menu == READER_MENU_CHAPTERS) {
        draw_chapter_picker();
        return 0;
      }
      render_reader_full();
      if (g.reader_menu == READER_MENU_WPM)
        menu_redraw();
      return 0;
    }
    start_from_selected_file_if_ready();
    return 0;
  }

  if (type == EVT_EXIT) {
    if (g.epub_path && g.word_count > 0) {
      save_progress();
      save_wpm();
    }
    clear_wpm_badge();
    ClearTimer(splash_timer);
    free_book_data();
    if (g.epub_path) free(g.epub_path);
    free_browse_entries();
    if (g.font_word) CloseFont(g.font_word);
    if (g.font_ui) CloseFont(g.font_ui);
    if (g.font_browse && g.font_browse != g.font_ui) CloseFont(g.font_browse);
    if (g.font_browse_title && g.font_browse_title != g.font_browse &&
        g.font_browse_title != g.font_ui) {
      CloseFont(g.font_browse_title);
    }
    return 0;
  }

  /* Splash: czekaj 1 s — ignoruj tap/klawisze (Home nadal wychodzi) */
  if (g.splash_active) {
    if (type == EVT_KEYPRESS && par1 == KEY_HOME) {
      ClearTimer(splash_timer);
      g.splash_active = 0;
      CloseApp();
      return 0;
    }
    if (type == EVT_POINTERDOWN || type == EVT_POINTERUP ||
        type == EVT_KEYPRESS || type == EVT_KEYRELEASE) {
      return 0;
    }
  }

  if (type == EVT_POINTERDOWN) {
    g.pointer_down_y = par2;
    g.pointer_down_valid = 1;
    return 0;
  }

  if (type == EVT_POINTERUP) {
    int up_x = par1;
    int up_y = par2;
    int down_y = g.pointer_down_valid ? g.pointer_down_y : up_y;
    int delta = down_y - up_y;
    g.pointer_down_valid = 0;

    /* Eksplorator */
    if (g.browse_active && g.word_count <= 0) {
      int rows = browse_visible_rows();
      if (abs(delta) >= 40) {
        if (delta > 0) g.browse_sel -= (rows > 1 ? rows - 1 : 1);
        else g.browse_sel += (rows > 1 ? rows - 1 : 1);
        browse_clamp_sel();
        browse_ensure_sel_visible();
        draw_browser();
        return 0;
      }

      int list_bottom = g.sh - browse_footer_h();
      if (up_y >= g.browse_header_h && up_y < list_bottom) {
        int row = (up_y - g.browse_header_h) / g.browse_row_h;
        int idx = g.browse_scroll + row;
        if (row >= 0 && row < rows && idx >= 0 && idx < g_browse_count) {
          g.browse_sel = idx;
          int y = g.browse_header_h + row * g.browse_row_h;
          InvertAreaBW(0, y, g.sw, g.browse_row_h);
          PartialUpdate(0, y, g.sw, g.browse_row_h);
          browse_activate_index(idx);
        }
      }
      return 0;
    }

    /* Czytanie / pauza = panel opcji / rozdziały */
    if (g.word_count > 0 && !g.browse_active) {
      int bar_y = g.sh - pause_panel_h();

      if (g.reader_menu == READER_MENU_CHAPTERS) {
        int rows = chapter_list_rows();
        if (abs(delta) >= 40) {
          if (delta > 0) g.chapter_sel -= (rows > 1 ? rows - 1 : 1);
          else g.chapter_sel += (rows > 1 ? rows - 1 : 1);
          if (g.chapter_sel < 0) g.chapter_sel = 0;
          if (g.chapter_sel >= g.chapter_count) g.chapter_sel = g.chapter_count - 1;
          draw_chapter_picker();
          return 0;
        }
        if (up_y >= g.browse_header_h && up_y < g.sh - CTRL_BAR_H) {
          int row = (up_y - g.browse_header_h) / g.browse_row_h;
          int idx = g.chapter_scroll + row;
          if (row >= 0 && row < rows && idx >= 0 && idx < g.chapter_count) {
            jump_to_chapter(idx);
          }
        }
        return 0;
      }

      if (g.reader_menu == READER_MENU_WPM) {
        if (abs(delta) >= 40) {
          if (delta > 0) g.wpm += WPM_SWIPE_STEP;
          else g.wpm -= WPM_SWIPE_STEP;
          clamp_wpm();
          save_wpm();
          menu_redraw();
          return 0;
        }
        /* top: wstecz / spis */
        if (reader_has_top_bar() && up_y < TOP_BAR_H) {
          if (up_x < 72) {
            leave_book_to_browser();
          } else if (up_x < 144) {
            open_chapter_picker();
          } else {
            g.reader_menu = READER_MENU_NONE;
            render_reader_full();
          }
          return 0;
        }
        if (up_y >= bar_y) {
          if (up_x < g.sw / 3) {
            g.wpm -= WPM_STEP;
            clamp_wpm();
            save_wpm();
            menu_redraw();
          } else if (up_x > (2 * g.sw) / 3) {
            g.wpm += WPM_STEP;
            clamp_wpm();
            save_wpm();
            menu_redraw();
          } else {
            g.reader_menu = READER_MENU_NONE;
            render_reader_full();
          }
        }
        return 0;
      }

      /* Panel opcji po pauzie */
      if (pause_options_on()) {
        if (abs(delta) >= 40) {
          if (delta > 0) g.wpm += WPM_SWIPE_STEP;
          else g.wpm -= WPM_SWIPE_STEP;
          clamp_wpm();
          save_wpm();
          render_control_bar();
          return 0;
        }

        if (up_y >= bar_y) {
          int ph = pause_panel_h();
          int info_h = PAUSE_INFO_H;
          int row_h = (ph - info_h) / PAUSE_OPT_COUNT;
          if (row_h < 42) row_h = 42;
          if (info_h + PAUSE_OPT_COUNT * row_h > ph) {
            info_h = ph - PAUSE_OPT_COUNT * row_h;
            if (info_h < 72) info_h = 72;
          }

          if (up_y < bar_y + info_h) return 0;

          int row = (up_y - bar_y - info_h) / row_h;
          if (row < 0) row = 0;
          if (row >= PAUSE_OPT_COUNT) row = PAUSE_OPT_COUNT - 1;

          if (row == PAUSE_OPT_PLAY) {
            set_playing(1, 0);
          } else if (row == PAUSE_OPT_CHAPTERS) {
            open_chapter_picker();
          } else if (row == PAUSE_OPT_NAV) {
            int third = g.sw / 3;
            if (up_x < third) {
              jump_relative_chapter(-1);
            } else if (up_x >= 2 * third) {
              jump_relative_chapter(+1);
            } else {
              jump_to_book_start();
            }
          } else if (row == PAUSE_OPT_WPM) {
            int third = g.sw / 3;
            if (up_x < third) {
              g.wpm -= WPM_STEP;
              clamp_wpm();
              save_wpm();
              render_control_bar();
            } else if (up_x >= 2 * third) {
              g.wpm += WPM_STEP;
              clamp_wpm();
              save_wpm();
              render_control_bar();
            } else {
              g.reader_menu = READER_MENU_WPM;
              render_reader_full();
            }
          } else if (row == PAUSE_OPT_LEAVE) {
            leave_book_to_browser();
          }
          return 0;
        }

        /* Tap w podgląd słowa nad panelem = wznów */
        set_playing(1, 0);
        return 0;
      }

      /* Odtwarzanie immersyjne — swipe ±WPM, tap = pauza + panel opcji */
      if (abs(delta) >= 40) {
        if (delta > 0) g.wpm += WPM_SWIPE_STEP;
        else g.wpm -= WPM_SWIPE_STEP;
        clamp_wpm();
        save_wpm();
        if (g.playing) {
          restart_timer();
          show_wpm_badge();
        }
        return 0;
      }
      show_reader_chrome();
      return 0;
    }

    return 0;
  }

  if (type == EVT_KEYPRESS) {
    int key = par1;

    if (key == KEY_HOME) {
      if (g.epub_path && g.word_count > 0) {
        save_progress();
        save_wpm();
      }
      CloseApp();
      return 0;
    }

    if (g.browse_active && g.word_count <= 0) {
      if (key == KEY_BACK) {
        if (!browse_cwd_is_root()) {
          char parent[1024];
          browse_parent_path(parent, sizeof(parent));
          safe_strncpy(g_browse_cwd, sizeof(g_browse_cwd), parent);
          g.browse_scroll = 0;
          g.browse_sel = 0;
          show_browser_list();
        } else {
          CloseApp();
        }
        return 0;
      }
      if (key == KEY_MENU || key == KEY_OK) {
        browse_activate_index(g.browse_sel);
        return 0;
      }
      if (key == KEY_PREV || key == KEY_UP || key == KEY_PREV2) {
        g.browse_sel--;
        browse_clamp_sel();
        browse_ensure_sel_visible();
        draw_browser();
        return 0;
      }
      if (key == KEY_NEXT || key == KEY_DOWN || key == KEY_NEXT2) {
        g.browse_sel++;
        browse_clamp_sel();
        browse_ensure_sel_visible();
        draw_browser();
        return 0;
      }
      return 0;
    }

    /* Czytanie: Back = powrót do wyboru książki (nie wyjście z apki) */
    if (g.word_count > 0 && !g.browse_active) {
      if (key == KEY_BACK) {
        if (g.reader_menu == READER_MENU_CHAPTERS || g.reader_menu == READER_MENU_WPM) {
          g.reader_menu = READER_MENU_NONE;
          g.chrome_visible = 1;
          render_reader_full();
          return 0;
        }
        leave_book_to_browser();
        return 0;
      }

      if (key == KEY_MENU) {
        if (g.reader_menu == READER_MENU_CHAPTERS || g.reader_menu == READER_MENU_WPM) {
          g.reader_menu = READER_MENU_NONE;
          g.chrome_visible = 1;
          render_reader_full();
          return 0;
        }
        if (g.playing || !g.chrome_visible) show_reader_chrome();
        else set_playing(1, 0);
        return 0;
      }

      if (g.reader_menu == READER_MENU_CHAPTERS) {
        if (key == KEY_PREV || key == KEY_UP || key == KEY_PREV2) {
          g.chapter_sel--;
          if (g.chapter_sel < 0) g.chapter_sel = 0;
          draw_chapter_picker();
        } else if (key == KEY_NEXT || key == KEY_DOWN || key == KEY_NEXT2) {
          g.chapter_sel++;
          if (g.chapter_sel >= g.chapter_count) g.chapter_sel = g.chapter_count - 1;
          draw_chapter_picker();
        } else if (key == KEY_OK) {
          jump_to_chapter(g.chapter_sel);
        }
        return 0;
      }

      if (g.reader_menu == READER_MENU_WPM) {
        if (key == KEY_PREV || key == KEY_UP || key == KEY_PREV2 || key == KEY_LEFT) {
          g.wpm -= WPM_STEP;
          clamp_wpm();
          save_wpm();
          menu_redraw();
        } else if (key == KEY_NEXT || key == KEY_DOWN || key == KEY_NEXT2 || key == KEY_RIGHT) {
          g.wpm += WPM_STEP;
          clamp_wpm();
          save_wpm();
          menu_redraw();
        } else if (key == KEY_OK) {
          g.reader_menu = READER_MENU_NONE;
          render_reader_full();
        }
        return 0;
      }

      if (key == KEY_OK) {
        if (g.playing) show_reader_chrome();
        else set_playing(1, 0);
        return 0;
      }
      return 0;
    }

    if (key == KEY_BACK) {
      CloseApp();
      return 0;
    }

    if (key == KEY_MENU) {
      if (g.word_count <= 0) OpenFileDialog();
      return 0;
    }

    return 0;
  }

  return 0;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // Uruchom InkView - aplikacja będzie obsługiwać EVT_* w main_handler
  InkViewMain(main_handler);
  return 0;
}

