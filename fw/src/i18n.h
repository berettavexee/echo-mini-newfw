/* Interface languages (ETUDE §112). The text shown on the screen is written in English in the
 * sources, marked for translation; tools/mkpot.py extracts the marked strings into po/newfw.pot,
 * the template of the translations (gettext PO files, po/<language>.po, kept up to date on
 * Weblate).
 *
 *   T("text")    shown on the screen: translated where it is used (a literal only)
 *   N_("text")   marked only, in a table of strings: tr() where the string is shown
 *   EN("text")   shown, but stays in English (the diagnostic screens, the fault screen, units
 *                and names that are the same in every language): not extracted
 *   tr(s)        the translation of s, a string marked with N_() (any other: itself)
 *
 * A comment starting with "TRANSLATORS:" just before the marked string goes into the template,
 * for the translators. A string with a printf conversion (%u, %s...) is formatted with
 * log_format(): its translation keeps the same conversions, in the same order.
 *
 * The translations: po/<language>.po, packed into the image as catalogues (tools/mklang.py),
 * the one of the language chosen loaded by lang.c (docs/TRANSLATING.md). */
#ifndef I18N_H
#define I18N_H

/* The translation of s in the language chosen (lang.c), else s itself */
__attribute__((format_arg(1))) const char *tr(const char *s);

#define T(s)  tr(s)
#define N_(s) (s)
#define EN(s) (s)

#endif
