#!/usr/bin/env python3
"""Builds the web UI in every language.

ESPMaster/data/index.html is the English source. Each ESPMaster/i18n/<lang>.json maps English
text to <lang>; this script writes, per language, into the output directory (default build/web):

  index-<lang>.html.gz  - the page for POST /api/web-file (upload it as "index.html.gz")
  littlefs-<lang>.bin   - LittleFS image with that page (flash at 0x300000), if mklittlefs is found

Replaced are only whole units, so code is never touched by accident:
  - HTML text between two tags (also with inline tags such as <code> or <b> inside),
  - the attributes title, placeholder, aria-label and value,
  - JS string literals wrapped in T(...).
--check fails on dictionary entries that no longer match anything and on T(...) literals
without a translation.
"""
import argparse, glob, gzip, json, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "ESPMaster", "data")
I18N_DIR = os.path.join(ROOT, "ESPMaster", "i18n")
# Same layout as eesz=4M1M in the ESP8266 core's boards.txt (see firmware/README.md).
MKLITTLEFS_ARGS = ["-p", "256", "-b", "8192", "-s", "1024000"]

JS_LITERAL = r"""("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'|`(?:[^`\\]|\\.)*`)"""


def translate(source, table, lang):
    used, missing = set(), []
    body = source.index("<body")
    script = source.index("<script>", body)
    head, html, js = source[:body], source[body:script], source[script:]

    if table:
        alternatives = "|".join(re.escape(k) for k in sorted(table, key=len, reverse=True))

        def text_sub(m):
            used.add(m.group(2))
            return ">" + m.group(1) + table[m.group(2)] + m.group(3) + "<"

        def attr_sub(m):
            used.add(m.group(2))
            return m.group(1) + '="' + table[m.group(2)] + '"'

        html = re.sub(r">(\s*)(" + alternatives + r")(\s*)<", text_sub, html)
        html = re.sub(r'\b(title|placeholder|aria-label|value)="(' + alternatives + r')"', attr_sub, html)

    def js_sub(m):
        quote, text = m.group(1)[0], m.group(1)[1:-1]
        if text in table:
            used.add(text)
            literal = quote + table[text] + quote
            if not re.fullmatch(JS_LITERAL, literal):
                sys.exit("%s: translation breaks the JS string (escape quotes as \\\"): %r" % (lang, table[text]))
            return "T(" + literal + ")"
        missing.append(text)
        return m.group(0)

    js = re.sub(r"(?<![\w$.])T\(" + JS_LITERAL + r"\)", js_sub, js)
    head = head.replace('<html lang="en">', '<html lang="%s">' % lang, 1)
    return head + html + js, set(table) - used, missing


def find_mklittlefs(explicit):
    if explicit:
        return explicit
    found = shutil.which("mklittlefs")
    if found:
        return found
    candidates = sorted(glob.glob(os.path.expanduser("~/.arduino15/packages/esp8266/tools/mklittlefs/*/mklittlefs")))
    return candidates[-1] if candidates else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(ROOT, "build", "web"), help="output directory")
    parser.add_argument("--mklittlefs", help="path to mklittlefs (default: PATH, then ~/.arduino15)")
    parser.add_argument("--check", action="store_true", help="only check the dictionaries, write nothing")
    args = parser.parse_args()

    source = open(os.path.join(DATA_DIR, "index.html"), encoding="utf-8").read()
    languages = {"en": {}}
    for path in sorted(glob.glob(os.path.join(I18N_DIR, "*.json"))):
        languages[os.path.splitext(os.path.basename(path))[0]] = json.load(open(path, encoding="utf-8"))

    pages, problems = {}, 0
    for lang, table in languages.items():
        page, unused, missing = translate(source, table, lang)
        pages[lang] = page
        if lang == "en":
            continue
        for text in sorted(unused):
            print("%s: unused entry: %r" % (lang, text[:100]), file=sys.stderr)
        for text in missing:
            print("%s: no translation for T(%r)" % (lang, text[:100]), file=sys.stderr)
        problems += len(unused) + len(missing)

    if args.check:
        sys.exit(1 if problems else 0)

    os.makedirs(args.out, exist_ok=True)
    mklittlefs = find_mklittlefs(args.mklittlefs)
    for lang, page in pages.items():
        compressed = gzip.compress(page.encode("utf-8"), 9, mtime=0)
        with open(os.path.join(args.out, "index-%s.html.gz" % lang), "wb") as f:
            f.write(compressed)
        if not mklittlefs:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            fs_dir = os.path.join(tmp, "data")
            shutil.copytree(DATA_DIR, fs_dir)
            os.remove(os.path.join(fs_dir, "index.html"))
            with open(os.path.join(fs_dir, "index.html.gz"), "wb") as f:
                f.write(compressed)
            subprocess.run([mklittlefs, "-c", fs_dir] + MKLITTLEFS_ARGS + [os.path.join(args.out, "littlefs-%s.bin" % lang)],
                           check=True, stdout=subprocess.DEVNULL)
    print("Built %s in %s%s" % (", ".join(pages), args.out, "" if mklittlefs else " (mklittlefs not found: no littlefs images)"))
    if problems:
        print("%d dictionary problem(s), see above" % problems, file=sys.stderr)


if __name__ == "__main__":
    main()
