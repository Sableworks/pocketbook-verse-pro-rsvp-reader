/* Lekki test hosta: strip HTML + round-trip INI (bez InkView / libzip).
 *   cc -O2 -Wall -o tests/host_smoke tests/host_smoke.c && ./tests/host_smoke
 */
#include "../rsvp_glue.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
      if (i + 5 <= n && strncmp(html + i, "&amp;", 5) == 0) {
        out[o++] = '&';
        i += 4;
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

static int test_strip(void) {
  const char *html = "<p>Ala &amp; kot</p><div>ma&nbsp;psa</div>";
  char *t = html_strip_tags_fallback(html, strlen(html));
  if (!t) return 1;
  int ok = (strstr(t, "Ala") && strstr(t, "&") && strstr(t, "kot") &&
            strstr(t, "ma") && strstr(t, "psa"));
  free(t);
  return ok ? 0 : 1;
}

/* Uproszczony zapis/odczyt jak .rsvp_saves.ini */
static int test_ini(void) {
  const char *path = "tests/_smoke_saves.ini";
  FILE *f = fopen(path, "w");
  if (!f) return 1;
  fprintf(f, "wpm=250\n");
  fprintf(f, "/mnt/ext1/Books/x.epub=42\n");
  fclose(f);

  int wpm = 0, idx = -1;
  f = fopen(path, "r");
  if (!f) return 1;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *val = eq + 1;
    size_t L = strlen(val);
    while (L > 0 && (val[L - 1] == '\n' || val[L - 1] == '\r')) val[--L] = '\0';
    if (strcmp(line, "wpm") == 0) wpm = atoi(val);
    if (strcmp(line, "/mnt/ext1/Books/x.epub") == 0) idx = atoi(val);
  }
  fclose(f);
  remove(path);
  return (wpm == 250 && idx == 42) ? 0 : 1;
}

/* Resolve ścieżki EPUB (uproszczenie ../ i #fragment) */
static void resolve_href(const char *base, const char *href, char *out, size_t outsz) {
  char tmp[256];
  const char *hash = strchr(href, '#');
  size_t hlen = hash ? (size_t)(hash - href) : strlen(href);
  if (hlen >= sizeof(tmp)) hlen = sizeof(tmp) - 1;
  memcpy(tmp, href, hlen);
  tmp[hlen] = '\0';

  char joined[320];
  snprintf(joined, sizeof(joined), "%s/%s", base, tmp);

  char parts[16][64];
  int n = 0;
  const char *p = joined;
  while (*p && n < 16) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *s = p;
    while (*p && *p != '/') p++;
    size_t len = (size_t)(p - s);
    if (len == 1 && s[0] == '.') continue;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      if (n > 0) n--;
      continue;
    }
    if (len >= sizeof(parts[0])) len = sizeof(parts[0]) - 1;
    memcpy(parts[n], s, len);
    parts[n][len] = '\0';
    n++;
  }
  out[0] = '\0';
  for (int i = 0; i < n; i++) {
    if (i) strncat(out, "/", outsz - strlen(out) - 1);
    strncat(out, parts[i], outsz - strlen(out) - 1);
  }
}

static int test_resolve(void) {
  char out[256];
  resolve_href("OEBPS", "../Text/ch1.xhtml#s1", out, sizeof(out));
  return strcmp(out, "Text/ch1.xhtml") == 0 ? 0 : 1;
}

static int test_unit_grouping(void) {
  const char *pl[] = {"Poszedł", "w", "tym", "domu", "i", "spał"};
  if (rsvp_unit_word_span(pl, 6, 0) != 1) return 1;
  if (rsvp_unit_word_span(pl, 6, 1) != 2) return 1; /* w tym — max 2 */
  if (rsvp_unit_word_span(pl, 6, 2) != 2) return 1; /* tym domu */
  if (rsvp_unit_word_span(pl, 6, 3) != 1) return 1; /* domu solo when taken as start */
  if (rsvp_unit_word_span(pl, 6, 4) != 2) return 1; /* i spał */
  if (rsvp_unit_word_span(pl, 6, 5) != 1) return 1;

  const char *en[] = {"The", "quick", "fox"};
  if (rsvp_unit_word_span(en, 3, 0) != 2) return 1; /* The quick */
  if (rsvp_unit_word_span(en, 3, 2) != 1) return 1;

  const char *nouns[] = {"dom", "kot", "sen"};
  if (rsvp_unit_word_span(nouns, 3, 0) != 1) return 1;
  if (rsvp_unit_word_span(nouns, 3, 1) != 1) return 1;

  const char *verbs[] = {"ma", "dom"};
  if (rsvp_unit_word_span(verbs, 2, 0) != 1) return 1; /* ma — nie łączyć */

  const char *de[] = {"der", "Mann", "und", "die", "Katze"};
  if (rsvp_unit_word_span(de, 5, 0) != 2) return 1; /* der Mann */
  if (rsvp_unit_word_span(de, 5, 2) != 2) return 1; /* und die — max 2 */
  if (rsvp_unit_word_span(de, 5, 3) != 2) return 1; /* die Katze */

  const char *tail[] = {"dom", "i", "w"};
  if (rsvp_unit_word_span(tail, 3, 0) != 1) return 1;
  if (rsvp_unit_word_span(tail, 3, 1) != 2) return 1; /* i w */

  if (!rsvp_is_glue_word("W")) return 1;
  if (!rsvp_is_glue_word("The")) return 1;
  if (!rsvp_is_glue_word("Der")) return 1;
  if (rsvp_is_glue_word("dom")) return 1;
  if (rsvp_is_glue_word("spał")) return 1;

  return 0;
}

int main(void) {
  int fail = 0;
  if (test_strip()) {
    fprintf(stderr, "FAIL strip\n");
    fail++;
  }
  if (test_ini()) {
    fprintf(stderr, "FAIL ini\n");
    fail++;
  }
  if (test_resolve()) {
    fprintf(stderr, "FAIL resolve\n");
    fail++;
  }
  if (test_unit_grouping()) {
    fprintf(stderr, "FAIL unit_grouping\n");
    fail++;
  }
  if (fail) {
    fprintf(stderr, "%d test(s) failed\n", fail);
    return 1;
  }
  printf("host_smoke: OK\n");
  return 0;
}
