#!/usr/bin/env python3
"""mkpot.py [--check]: the template of the interface's translations (ETUDE §112).

Extracts the strings marked with T("...") and N_("...") in src/*.c and src/*.h (i18n.h) into
po/newfw.pot, a gettext template (the format of Weblate's component). With --check, the template
is compared with the one in the tree instead of written: a source changed without the template
(make pot) fails, and so does a string shown on the screen without a mark (below).

  - adjacent literals are joined ("a" "b"), the C escapes read;
  - a comment "/* TRANSLATORS: ... */" ending on the line of the string or the line before goes
    into the template (#. lines);
  - a string with a printf conversion is flagged c-format (Weblate then checks that the
    translation keeps the same conversions);
  - the template has no date, and its references name the files, not the lines: it is the same
    for the same strings, so that the check can compare (and a line moved changes nothing).

The unmarked strings: on a line that calls one of the drawing functions (SHOWN below), a literal
with a letter in it outside T(), N_() or EN() is refused. A call spread over several lines is
checked on its first line only.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
SRC = os.path.join(FW, 'src')
POT = os.path.join(FW, 'po', 'newfw.pot')

SHOWN = ('lcd_text', 'lcd_text_clip', 'lcd_text_clip_x', 'lcd_text_scroll', 'sys_line',
         'sys_status_line', 'text_right', 'clock_msg', 'put_str', 'top_entry', 'copy_name',
         'marquee_set')

LIT = r'"(?:[^"\\\n]|\\.)*"'
LIT_RUN = rf'{LIT}(?:\s*{LIT})*'
MARK = re.compile(rf'\b(T|N_|EN)\(\s*({LIT_RUN})\s*\)')
MARK_ANY = re.compile(r'\b(T|N_)\(')
CALL = re.compile(r'\b(' + '|'.join(SHOWN) + r')\(')
CONV = re.compile(r'%[-0-9.]*l*[sdcuxX]')
ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'", '0': '\0', 'a': '\a',
       'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def c_string(literals):
    """The value of adjacent C literals (the source is UTF-8)"""
    out = []
    for lit in re.findall(LIT, literals):
        b = lit[1:-1]
        i = 0
        while i < len(b):
            c = b[i]
            if c != '\\':
                out.append(c)
                i += 1
                continue
            e = b[i + 1]
            if e == 'x':
                m = re.match(r'[0-9a-fA-F]+', b[i + 2:])
                out.append(chr(int(m.group(), 16)))
                i += 2 + len(m.group())
            elif e in '01234567':
                m = re.match(r'[0-7]{1,3}', b[i + 1:])
                out.append(chr(int(m.group(), 8)))
                i += 1 + len(m.group())
            else:
                out.append(ESC[e])
                i += 2
    return ''.join(out)


def po_string(s):
    """A PO string: escaped, split after each newline"""
    e = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
    parts = e.split('\n')
    if len(parts) == 1:
        return f'"{e}"'
    lines = [p + '\\n' for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    return '""\n' + '\n'.join(f'"{p}"' for p in lines)


def strip_comments(text):
    """The text with its comments blanked (lines and columns kept), and the comments: (end line,
    text)"""
    out, comments = [], []
    i, n = 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.index('*/', i + 2) + 2
            comments.append((text.count('\n', 0, j) + 1, text[i + 2:j - 2]))
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text[i] in '"\'':
            m = re.compile(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'').match(text, i)
            j = m.end() if m else i + 1
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out), comments


def scan(path, rel, entries, errors):
    text = open(path, encoding='utf-8').read()
    code, comments = strip_comments(text)
    notes = {}
    for end, c in comments:
        c = ' '.join(ln.strip().lstrip('*').strip() for ln in c.strip().splitlines())
        if c.startswith('TRANSLATORS:'):
            notes[end] = c
    for m in MARK_ANY.finditer(code):
        line = code.count('\n', 0, m.start()) + 1
        full = MARK.match(code, m.start())
        if not full:
            errors.append(f'{rel}:{line}: {m.group(1)}() takes a literal (tr() for a pointer)')
            continue
        msgid = c_string(full.group(2))
        if not msgid:
            errors.append(f'{rel}:{line}: an empty string marked')
            continue
        e = entries.setdefault(msgid, {'refs': [], 'notes': []})
        if rel not in e['refs']:
            e['refs'].append(rel)
        note = notes.get(line) or notes.get(line - 1)
        if note and note not in e['notes']:
            e['notes'].append(note)
    for no, ln in enumerate(code.split('\n'), 1):
        if not CALL.search(ln):
            continue
        rest = MARK.sub('', ln)
        for lit in re.findall(LIT, rest):
            if re.search(r'[A-Za-z]', c_string(lit)):
                errors.append(f'{rel}:{no}: {lit} shown without T(), N_() or EN()')


def template(entries):
    out = ['# The interface of the ECHO MINI new firmware: the template of the translations.',
           '# Generated by fw/tools/mkpot.py from the sources (make pot): do not edit.',
           '# The translations: fw/po/<language>.po (Weblate).',
           '#, fuzzy',
           'msgid ""',
           'msgstr ""',
           '"Project-Id-Version: echo-mini-newfw\\n"',
           '"Report-Msgid-Bugs-To: https://github.com/berettavexee/echo-mini-newfw/issues\\n"',
           '"Language: \\n"',
           '"MIME-Version: 1.0\\n"',
           '"Content-Type: text/plain; charset=UTF-8\\n"',
           '"Content-Transfer-Encoding: 8bit\\n"',
           '']
    for msgid, e in entries.items():
        for n in e['notes']:
            out.append('#. ' + n)
        out.append('#: ' + ' '.join(e['refs']))
        if CONV.search(msgid.replace('%%', '')):
            out.append('#, c-format')
        out.append('msgid ' + po_string(msgid))
        out.append('msgstr ""')
        out.append('')
    return '\n'.join(out)


def main():
    check = '--check' in sys.argv[1:]
    entries, errors = {}, []
    for name in sorted(os.listdir(SRC)):
        if name.endswith(('.c', '.h')) and name != 'i18n.h':
            scan(os.path.join(SRC, name), 'src/' + name, entries, errors)
    for e in errors:
        print(e, file=sys.stderr)
    pot = template(entries)
    if check:
        old = open(POT, encoding='utf-8').read() if os.path.exists(POT) else ''
        if old != pot:
            print('po/newfw.pot is not up to date: make pot', file=sys.stderr)
            return 1
        print(f'ok    po/newfw.pot: {len(entries)} strings')
    else:
        os.makedirs(os.path.dirname(POT), exist_ok=True)
        with open(POT, 'w', encoding='utf-8') as f:
            f.write(pot)
        print(f'wrote po/newfw.pot: {len(entries)} strings')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
