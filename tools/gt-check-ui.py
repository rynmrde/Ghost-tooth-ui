#!/usr/bin/env python3
"""tools/gt-check-ui.py - static consistency check for the web UI.

Nothing else in this build catches a typo in an element id: `$("btn-scan")`
returning null is a runtime exception inside the console's WebKit container, on
a screen with no devtools.  This is the cheap version of a DOM test - it proves
that every id and translation key the script touches exists in the markup, that
both languages define the same keys, and that the HTML/CSS are self-consistent.

  python3 tools/gt-check-ui.py

Copyright (C) 2026 Ghost-tooth-ui contributors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import os
import re
import sys

ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "assets")
problems = []


def note(msg):
    problems.append(msg)


def read(name):
    with open(os.path.join(ASSETS, name), encoding="utf-8") as f:
        return f.read()


def check_ids(html, js):
    ids = set(re.findall(r'\bid="([^"]+)"', html))
    used = set(re.findall(r'\$\("([^"]+)"\)', js))
    used |= set(re.findall(r'getElementById\("([^"]+)"\)', js))
    for name in sorted(used - ids):
        note("main.js asks for #%s, but index.html has no such id" % name)
    for pill in re.finditer(r'<div class="pill"[^>]*id="([^"]+)"(.*?)</div>',
                            html, re.S):
        if "<em" not in pill.group(2):
            note("pill #%s has no <em> for the value main.js writes" % pill.group(1))


def lang_table(js, lang):
    """Keys of one language's table in `var STRINGS = { en: {...}, fa: {...} }`."""
    m = re.search(r"\n    %s: \{\n" % lang, js)
    if not m:
        note("main.js has no %s translation table" % lang)
        return set()
    body = js[m.end():]
    depth = 1
    for i, ch in enumerate(body):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                body = body[:i]
                break
    # Several keys share one line in these tables, so match on structure and not
    # on layout: drop the string literals, then every bare word in front of a
    # colon is a key.
    stripped = re.sub(r'"(?:[^"\\]|\\.)*"', '""', body)
    return set(re.findall(r"([A-Za-z_]\w*)\s*:", stripped))


def check_i18n(html, js):
    wanted = set(re.findall(r'data-i18n="([^"]+)"', html))
    wanted |= set(re.findall(r'\bt\("([^"]+)"\)', js))
    tables = {lang: lang_table(js, lang) for lang in ("en", "fa")}
    for key in sorted(wanted):
        for lang, table in tables.items():
            if key not in table:
                note("'%s' is used in the UI but missing from the %s table"
                     % (key, lang))
    if tables.get("en") and tables.get("fa") and tables["en"] != tables["fa"]:
        for key in sorted(tables["en"] ^ tables["fa"]):
            note("'%s' is defined in only one language" % key)


def check_api(js):
    called = set(re.findall(r'request\("(/api/[a-z-]+)"', js))
    called |= set(re.findall(r'request\("(/[a-z-]+)"', js))
    for path in sorted(called):
        if path not in ("/api/state", "/api/action"):
            note("main.js requests %s, which the server does not implement as a "
                 "UI endpoint" % path)


def check_markup(html):
    void = {"meta", "link", "br", "hr", "img", "input", "source", "path"}
    stack = []
    for closing, name, rest in re.findall(r"<(/?)([a-z0-9]+)([^>]*)>",
                                          re.sub(r"<!--.*?-->", "", html, flags=re.S)):
        if name in void or rest.rstrip().endswith("/"):
            continue
        if closing:
            if stack and stack[-1] == name:
                stack.pop()
            else:
                note("</%s> closes nothing (open stack ends with %s)"
                     % (name, stack[-2:] or "nothing"))
        else:
            stack.append(name)
    if stack:
        note("markup leaves these open: %s" % stack)


def check_css(css):
    if css.count("{") != css.count("}"):
        note("main.css has unbalanced braces (%d open, %d close)"
             % (css.count("{"), css.count("}")))
    for prop, value in re.findall(r"([a-z-]+)\s*:\s*([^;{}]+);", css):
        for colour in re.findall(r"#[0-9a-zA-Z]+", value):
            digits = colour[1:]
            if len(digits) not in (3, 4, 6, 8) or \
                    not re.fullmatch(r"[0-9a-fA-F]+", digits):
                note("main.css: %s: %s is not a colour" % (prop, colour))
        if "(" in value and value.count("(") != value.count(")"):
            note("main.css: %s: unbalanced parentheses in %r" % (prop, value))
    for sel in re.findall(r"([^{}]+)\{", css):
        if ";" in sel:
            note("main.css: a declaration looks like a selector: %r" % sel.strip()[:60])


def main():
    try:
        html, css, js = read("index.html"), read("main.css"), read("main.js")
    except OSError as e:
        print("cannot read the assets: %s" % e)
        return 2
    check_ids(html, js)
    check_i18n(html, js)
    check_api(js)
    check_markup(html)
    check_css(css)

    if problems:
        for p in problems:
            print("FAIL " + p)
        print("%d problem(s) in the UI assets" % len(problems))
        return 1
    print("index.html, main.css and main.js agree with each other - ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
