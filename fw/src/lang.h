/* The interface's languages (ETUDE §112): the catalogues of our firmware image (lang.c); tr()
 * itself is declared in i18n.h. Language 0 is English (no catalogue), the others are the
 * catalogues of the image, in its order. */
#ifndef LANG_H
#define LANG_H

#define LANG_MAX 15                             /* catalogues read from the image */

int lang_init(void);                    /* the directory of the catalogues (after font_init) */
int lang_count(void);                   /* English included */
const char *lang_code(int i);           /* "en", "fr", "zh_CN"... */
const char *lang_name(int i);           /* in the language itself: "Français" */
int lang_current(void);
int lang_set(int i);                    /* its catalogue loaded: 0; else English and < 0 */
int lang_set_code(const char *code);    /* the same by its code (English when unknown) */

#endif
