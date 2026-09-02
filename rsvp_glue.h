/* Słowa funkcyjne (PL / EN / DE) łączone z następnym w RSVP — mniej odświeżeń e-ink.
 * Wykluczone: krótkie rzeczowniki (dom, kot), czasowniki (je, ma, die), liczebniki itd.
 * Porównanie bez względu na wielkość liter (UTF-8: polskie i niemieckie znaki diakrytyczne). */
#ifndef RSVP_GLUE_H
#define RSVP_GLUE_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const char *const rsvp_glue_words[] = {
    /* --- Polski: przyimki --- */
    "bez", "dla", "do", "ku", "na", "nad", "o", "od", "po", "pod", "przed", "przy",
    "spod", "u", "w", "we", "za", "ze", "z",
    /* --- Polski: spójniki / partykuły --- */
    "a", "aby", "albo", "ani", "bo", "by", "choć", "czy", "gdy", "gdyż", "i", "jak",
    "lecz", "lub", "niż", "o", "to", "więc", "że",
    /* --- Polski: zaimki / określniki --- */
    "ci", "co", "go", "ich", "im", "ja", "ją", "je", "mi", "mu", "nas", "ni", "on",
    "się", "ta", "te", "tego", "tej", "ten", "tę", "ty", "tych", "tym", "tu", "was",
    /* --- Polski: partykuły negacji / modalne --- */
    "nie", "no", "już", "jeszcze", "tylko", "też", "tak", "nawet", "znowu", "znów",

    /* --- English: articles --- */
    "a", "an", "the",
    /* --- English: prepositions --- */
    "as", "at", "by", "for", "from", "in", "into", "of", "off", "on", "onto", "out",
    "per", "than", "to", "up", "via", "with",
    /* --- English: conjunctions --- */
    "and", "but", "if", "nor", "or", "so", "yet",
    /* --- English: pronouns / determiners --- */
    "he", "her", "him", "his", "i", "it", "its", "me", "my", "our", "she", "that",
    "their", "them", "these", "they", "this", "those", "us", "we", "you",
    /* --- English: auxiliaries / negation (krótkie) --- */
    "am", "are", "be", "been", "can", "did", "do", "had", "has", "is", "may", "must",
    "not", "was", "were", "will",

    /* --- Deutsch: Artikel --- */
    "das", "dem", "den", "der", "des", "die", "ein", "eine", "einem", "einen",
    "einer", "eins",
    /* --- Deutsch: Präpositionen --- */
    "ab", "an", "auf", "aus", "bei", "bis", "durch", "für", "gegen", "in", "mit",
    "nach", "ohne", "um", "unter", "von", "vor", "zu", "zum", "zur",
    /* --- Deutsch: Konjunktionen / Partikeln --- */
    "als", "auch", "aber", "dass", "denn", "doch", "noch", "nur", "ob", "oder",
    "schon", "sehr", "so", "und", "wenn", "weil",
    /* --- Deutsch: Pronomen --- */
    "du", "er", "es", "euch", "ich", "ihm", "ihn", "ihr", "ihre", "ihren", "ihrer",
    "ihres", "man", "mich", "mir", "sie", "uns", "wir", "dir",
    /* --- Deutsch: Negation / Modalpartikeln --- */
    "nicht", "mal", "ja", "gern", "wohl", "halt", "eben",
};

static const size_t rsvp_glue_word_count =
    sizeof(rsvp_glue_words) / sizeof(rsvp_glue_words[0]);

/* Mapowanie wielkiej litery UTF-8 (PL/DE) na małą — 2 bajty. */
static int rsvp_utf8_lower_pair(unsigned char c0, unsigned char c1,
                                unsigned char *out0, unsigned char *out1) {
  /* Polskie: ĄĆĘŁŃÓŚŹŻ (C4 xx / C5 xx) */
  if (c0 == 0xC4) {
    switch (c1) {
      case 0x84: *out0 = 0xC4; *out1 = 0x85; return 1; /* Ą → ą */
      case 0x86: *out0 = 0xC4; *out1 = 0x87; return 1; /* Ć → ć */
      case 0x98: *out0 = 0xC4; *out1 = 0x99; return 1; /* Ę → ę */
      default: break;
    }
  }
  if (c0 == 0xC5) {
    switch (c1) {
      case 0x81: *out0 = 0xC5; *out1 = 0x82; return 1; /* Ł → ł */
      case 0x83: *out0 = 0xC5; *out1 = 0x84; return 1; /* Ń → ń */
      case 0x93: *out0 = 0xC5; *out1 = 0x94; return 1; /* Ó → ó */
      case 0x9A: *out0 = 0xC5; *out1 = 0x9B; return 1; /* Ś → ś */
      case 0xB9: *out0 = 0xC5; *out1 = 0xBA; return 1; /* Ź → ź */
      case 0xBB: *out0 = 0xC5; *out1 = 0xBC; return 1; /* Ż → ż */
      default: break;
    }
  }
  /* Niemieckie: Ä Ö Ü (C3 84/96/9C) */
  if (c0 == 0xC3) {
    switch (c1) {
      case 0x84: *out0 = 0xC3; *out1 = 0xA4; return 1; /* Ä → ä */
      case 0x96: *out0 = 0xC3; *out1 = 0xB6; return 1; /* Ö → ö */
      case 0x9C: *out0 = 0xC3; *out1 = 0xBC; return 1; /* Ü → ü */
      default: break;
    }
  }
  return 0;
}

static void rsvp_word_lower(const char *in, char *out, size_t outsz) {
  size_t i = 0;
  if (!in || !out || outsz == 0) return;
  out[0] = '\0';

  while (*in && i + 1 < outsz) {
    unsigned char c = (unsigned char)*in;
    if (c < 0x80) {
      out[i++] = (char)tolower(c);
      in++;
      continue;
    }
    if ((c & 0xE0) == 0xC0 && in[1]) {
      unsigned char lo0, lo1;
      if (rsvp_utf8_lower_pair(c, (unsigned char)in[1], &lo0, &lo1)) {
        if (i + 2 >= outsz) break;
        out[i++] = (char)lo0;
        out[i++] = (char)lo1;
        in += 2;
        continue;
      }
      if (i + 2 >= outsz) break;
      out[i++] = in[0];
      out[i++] = in[1];
      in += 2;
      continue;
    }
    out[i++] = (char)c;
    in++;
  }
  out[i] = '\0';
}

static int rsvp_is_glue_word(const char *word) {
  char lower[64];
  size_t i;

  if (!word || !word[0]) return 0;
  rsvp_word_lower(word, lower, sizeof(lower));
  if (!lower[0]) return 0;

  for (i = 0; i < rsvp_glue_word_count; i++) {
    if (strcmp(lower, rsvp_glue_words[i]) == 0) return 1;
  }
  return 0;
}

/* Ile kolejnych słów tworzy jedną jednostkę wyświetlania (od idx). Max 2. */
static int rsvp_unit_word_span(const char *const *words, int count, int idx) {
  if (idx < 0 || idx >= count) return 0;
  if (rsvp_is_glue_word(words[idx]) && idx + 1 < count) return 2;
  return 1;
}

#endif /* RSVP_GLUE_H */
