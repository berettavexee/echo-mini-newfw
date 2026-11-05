/* Select the user-visible string for the current interface language. */
#include "ui_priv.h"

const char *ui_tr(const char *en, const char *zh, const char *ja)
{
    switch (cfg.language) {
    case 1: return zh;
    case 2: return ja;
    default: return en;
    }
}
