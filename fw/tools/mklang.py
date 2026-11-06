#!/usr/bin/env python3
"""mklang.py FONT.bdf OUT.bin | --check FONT.bdf: the catalogues of the interface's languages
(ETUDE §112), from po/<language>.po, for our firmware image (mkimg.py --lang; read by
src/lang.c, whose comment gives the format).

Each PO file gives one catalogue: its translated entries, neither fuzzy nor obsolete, whose
msgid is in po/newfw.pot; the name of the language is the translation of "English" (its code
otherwise). The languages are packed in the order of their codes.

Refused (exit code 1): a translation whose printf conversions differ from its English string's
(c-format), a catalogue larger than 16 KiB, two strings of the template with the same hash.
Noted only: an entry the template no longer has (po/ is merged on Weblate), a translation wider
than the screen (and than its English string), measured with the font.
--check builds nothing: the same checks, for make check.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PO = os.path.join(os.path.dirname(HERE), 'po')
CAT_MAX = 0x4000
WIDE = 312                                      # the screen, 320 px, less its margins
CONV = re.compile(r'%[-0-9.]*l*[sdcuxX%]')
ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}


def unquote(s):
    return re.sub(r'\\(.)', lambda m: ESC.get(m.group(1), m.group(1)), s[1:-1])


def parse_po(path):
    """[(msgid, msgstr, flags)] of the entries (obsolete ones left out), the header first"""
    entries, cur, key = [], None, None

    def done():
        if cur is not None and 'msgid' in cur:
            entries.append((cur['msgid'], cur.get('msgstr', ''), cur['flags']))

    for ln in open(path, encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#~'):
            if not ln:
                done()
                cur, key = None, None
            continue
        if cur is None:
            cur = {'flags': set()}
        if ln.startswith('#,'):
            cur['flags'] |= {f.strip() for f in ln[2:].split(',')}
        elif ln.startswith('#'):
            pass
        elif ln.startswith('"'):
            cur[key] += unquote(ln)
        else:
            key, _, val = ln.partition(' ')
            if key == 'msgstr[0]':
                key = 'msgstr'
            cur[key] = unquote(val.strip())
    done()
    return entries


def fnv(s):
    h = 0x811c9dc5
    for b in s.encode('utf-8'):
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def widths(bdf):
    adv, cp = {}, None
    for line in open(bdf, encoding='latin-1'):
        p = line.split()
        if p and p[0] == 'ENCODING':
            cp = int(p[1])
        elif p and p[0] == 'DWIDTH' and cp is not None:
            adv[cp] = int(p[1])
    return lambda s: sum(adv.get(ord(c), adv.get(ord('?'), 6)) for c in s)


def catalogue(entries):
    entries = sorted(entries)
    head = struct.pack('<4s3I', b'NWLC', 1, len(entries), 0)
    table, text = b'', b''
    base = len(head) + 8 * len(entries)
    for h, s in entries:
        table += struct.pack('<2I', h, base + len(text))
        text += s.encode('utf-8') + b'\0'
    cat = head + table + text
    return cat + bytes(-len(cat) % 4)


def main():
    a = sys.argv[1:]
    check = a[:1] == ['--check']
    if check:
        a = a[1:]
    if len(a) != (1 if check else 2):
        sys.exit(__doc__)
    width = widths(a[0])
    pot = [e[0] for e in parse_po(os.path.join(PO, 'newfw.pot')) if e[0]]
    hashes = {}
    bad = 0
    for m in pot:
        h = fnv(m)
        if h in hashes:
            print(f'hash collision: {m!r} and {hashes[h]!r}', file=sys.stderr)
            bad = 1
        hashes[h] = m
    known = set(pot)
    langs = []
    for name in sorted(os.listdir(PO)):
        if not name.endswith('.po'):
            continue
        code = name[:-3]
        out, own, stale = [], code, 0
        for msgid, msgstr, flags in parse_po(os.path.join(PO, name)):
            if not msgid or not msgstr or 'fuzzy' in flags:
                continue
            if msgid not in known:
                stale += 1
                continue
            if CONV.findall(msgid) != CONV.findall(msgstr):
                print(f'{name}: {msgid!r}: the conversions differ in {msgstr!r}', file=sys.stderr)
                bad = 1
                continue
            if width(msgstr) > max(WIDE, width(msgid)):
                print(f'note  {name}: {msgstr!r}: {width(msgstr)} px (English: {width(msgid)} px)')
            if msgid == 'English':
                own = msgstr
            out.append((fnv(msgid), msgstr))
        cat = catalogue(out)
        if len(cat) > CAT_MAX:
            print(f'{name}: catalogue of {len(cat)} bytes, more than {CAT_MAX}', file=sys.stderr)
            bad = 1
        print(f'{"ok   " if check else "     "} {code}: {own}, {len(out)}/{len(pot)} strings, '
              f'{len(cat)} bytes' + (f', {stale} not in the template' if stale else ''))
        if len(code.encode()) > 7 or len(own.encode()) > 23:
            print(f'{name}: code or name too long', file=sys.stderr)
            bad = 1
        langs.append((code, own, cat))
    if bad:
        return 1
    if check:
        return 0
    pack = struct.pack('<4s3I', b'NWLP', 1, len(langs), 0)
    off = len(pack) + 40 * len(langs)
    body = b''
    for code, own, cat in langs:
        pack += struct.pack('<8s24s2I', code.encode(), own.encode(), off + len(body), len(cat))
        body += cat
    with open(a[1], 'wb') as f:
        f.write(pack + body)
    print(f'wrote {a[1]}: {len(langs)} language(s), {len(pack) + len(body)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
