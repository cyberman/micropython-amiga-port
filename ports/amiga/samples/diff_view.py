# diff_view.py - Side-by-side colored diff viewer for text files.
#
# Usage:
#   micropython diff_view.py [-w cols] [-l lines] <file1> <file2>
#
# Options:
#   -w cols    terminal width in columns (default 80)
#   -l lines   diff rows per page         (default 18)
#
# Navigation (each command followed by Enter):
#   Enter   next page
#   b       previous page
#   q       quit
#
# Author: written for Fabrice's MicroPython Amiga port (build 227+)

import sys

# --- ANSI escape codes (work in AmigaOS shell) ---
RESET = "\x1b[0m"
BOLD = "\x1b[1m"
RED = "\x1b[1;31m"     # bold red    - deleted
GREEN = "\x1b[1;32m"   # bold green  - added
YELLOW = "\x1b[1;33m"  # bold yellow - modified

WIDTH = 80           # total terminal width
PAGE_LINES = 18      # rows of diff per page (leaves room for header/footer)
TAB_WIDTH = 4
NUM_WIDTH = 4        # space reserved for line numbers


# ---------------------------------------------------------------------------
# File reading
# ---------------------------------------------------------------------------
def read_lines(path):
    try:
        f = open(path, "r")
    except OSError as e:
        print("Error: cannot open '%s': %s" % (path, e))
        sys.exit(1)
    try:
        data = f.read()
    finally:
        f.close()
    # splitlines() handles \n, \r\n, \r and strips the terminator
    return data.splitlines()


# ---------------------------------------------------------------------------
# Diff algorithm: LCS via dynamic programming
# ---------------------------------------------------------------------------
def lcs_diff(a, b):
    """
    Return a list of operations aligning a and b:
      ("eq",  line_a, line_b)   - lines are equal
      ("del", line_a, None)     - line only in a
      ("add", None,   line_b)   - line only in b
    """
    n = len(a)
    m = len(b)

    # DP table: t[i][j] = LCS length of a[:i] and b[:j]
    # Allocated as list of lists; for files up to ~500 lines this is fine.
    t = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n):
        ai = a[i]
        ti = t[i]
        ti1 = t[i + 1]
        for j in range(m):
            if ai == b[j]:
                ti1[j + 1] = ti[j] + 1
            else:
                up = ti[j + 1]
                left = ti1[j]
                ti1[j + 1] = up if up > left else left

    # Backtrack to recover the alignment
    ops = []
    i = n
    j = m
    while i > 0 or j > 0:
        if i > 0 and j > 0 and a[i - 1] == b[j - 1]:
            ops.append(("eq", a[i - 1], b[j - 1]))
            i -= 1
            j -= 1
        elif j > 0 and (i == 0 or t[i][j - 1] >= t[i - 1][j]):
            ops.append(("add", None, b[j - 1]))
            j -= 1
        else:
            ops.append(("del", a[i - 1], None))
            i -= 1
    ops.reverse()
    return ops


def pair_modifications(ops):
    """
    Merge adjacent del+add into 'mod' rows for nicer side-by-side display.
    A run of K dels followed by L adds becomes:
      min(K,L) 'mod' rows, then leftover 'del' or 'add' rows.
    """
    out = []
    n = len(ops)
    i = 0
    while i < n:
        if ops[i][0] == "del":
            dels = []
            while i < n and ops[i][0] == "del":
                dels.append(ops[i][1])
                i += 1
            adds = []
            while i < n and ops[i][0] == "add":
                adds.append(ops[i][2])
                i += 1
            k = len(dels) if len(dels) > len(adds) else len(adds)
            for x in range(k):
                l = dels[x] if x < len(dels) else None
                r = adds[x] if x < len(adds) else None
                if l is not None and r is not None:
                    out.append(("mod", l, r))
                elif l is not None:
                    out.append(("del", l, None))
                else:
                    out.append(("add", None, r))
        else:
            out.append(ops[i])
            i += 1
    return out


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def fit(s, width):
    """Pad or truncate s to exactly `width` visible characters."""
    if len(s) > width:
        return s[:width - 1] + ">"
    if len(s) < width:
        return s + " " * (width - len(s))
    return s


def expand_tabs(s, tabsize=4):
    """Manual tab expansion (str.expandtabs is not in MicroPython)."""
    if "\t" not in s:
        return s
    out = []
    col = 0
    for ch in s:
        if ch == "\t":
            spaces = tabsize - (col % tabsize)
            out.append(" " * spaces)
            col += spaces
        else:
            out.append(ch)
            col += 1
    return "".join(out)


def render_row(kind, l_num, l_text, r_num, r_text, half):
    text_w = half - NUM_WIDTH - 1  # NUM_WIDTH for number, 1 for space

    l_n = "%*s" % (NUM_WIDTH, str(l_num) if l_num else "")
    r_n = "%*s" % (NUM_WIDTH, str(r_num) if r_num else "")

    l_t = expand_tabs(l_text or "", TAB_WIDTH)
    r_t = expand_tabs(r_text or "", TAB_WIDTH)
    l_t = fit(l_t, text_w)
    r_t = fit(r_t, text_w)

    # Pick color and central marker based on row kind
    if kind == "eq":
        marker = "|"
    elif kind == "mod":
        l_t = YELLOW + l_t + RESET
        r_t = YELLOW + r_t + RESET
        marker = "~"
    elif kind == "del":
        l_t = RED + l_t + RESET
        marker = "<"
    elif kind == "add":
        r_t = GREEN + r_t + RESET
        marker = ">"
    else:
        marker = "?"

    return "%s %s %s %s %s" % (l_n, l_t, marker, r_n, r_t)


def render_header(file1, file2, half):
    text_w = half - NUM_WIDTH - 1
    f1 = fit(file1, text_w)
    f2 = fit(file2, text_w)
    blank = " " * NUM_WIDTH
    return BOLD + "%s %s   %s %s" % (blank, f1, blank, f2) + RESET


def compute_stats(rows):
    eq = mod = add = dele = 0
    for kind, _, _ in rows:
        if kind == "eq":
            eq += 1
        elif kind == "mod":
            mod += 1
        elif kind == "add":
            add += 1
        elif kind == "del":
            dele += 1
    return eq, mod, add, dele


# ---------------------------------------------------------------------------
# Pager
# ---------------------------------------------------------------------------
def run_pager(numbered, file1, file2, half, width, page_lines):
    total = len(numbered)
    eq, mod, add, dele = compute_stats(
        [(k, l, r) for k, _, l, _, r in numbered])
    pos = 0

    while True:
        # Header
        print()
        print(render_header(file1, file2, half))
        print("-" * width)

        # Page content
        page = numbered[pos:pos + page_lines]
        for row in page:
            print(render_row(row[0], row[1], row[2], row[3], row[4], half))

        # Pad short pages so footer always at same position
        for _ in range(page_lines - len(page)):
            print()

        # Footer
        print("-" * width)
        end = pos + len(page)
        eof = (end >= total)
        status = "Lines %d-%d / %d  (=%d  ~%d  +%d  -%d)" % (
            pos + 1 if total else 0, end, total, eq, mod, add, dele)
        if eof:
            status += "  [END]"
        print(status)
        print("[Enter=next  b=back  q=quit] ", end="")

        try:
            cmd = input().strip().lower()
        except EOFError:
            print()
            return

        if cmd == "q":
            return
        elif cmd == "b":
            if pos == 0:
                continue
            pos -= page_lines
            if pos < 0:
                pos = 0
        else:
            if eof:
                return
            pos += page_lines


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help"):
        print("Usage: diff_view.py [-w cols] [-l lines] <file1> <file2>")
        print("  Side-by-side colored diff viewer.")
        print("  -w cols    terminal width in columns (default 80)")
        print("  -l lines   diff rows per page (default 18)")
        print("  Navigation: Enter=next  b=back  q=quit")
        return

    width = WIDTH
    page_lines = PAGE_LINES
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-w":
            i += 1
            if i >= len(argv):
                print("Error: -w requires a value"); return
            try:
                width = int(argv[i])
            except ValueError:
                print("Error: -w value must be an integer"); return
            if width < 40:
                print("Error: width must be at least 40"); return
        elif a == "-l":
            i += 1
            if i >= len(argv):
                print("Error: -l requires a value"); return
            try:
                page_lines = int(argv[i])
            except ValueError:
                print("Error: -l value must be an integer"); return
            if page_lines < 4:
                print("Error: page must have at least 4 lines"); return
        elif a.startswith("-"):
            print("Error: unknown option %s" % a); return
        else:
            files.append(a)
        i += 1

    if len(files) != 2:
        print("Error: need exactly two filenames")
        print("Usage: diff_view.py [-w cols] [-l lines] <file1> <file2>")
        return

    file1, file2 = files[0], files[1]
    print("Reading %s..." % file1)
    a = read_lines(file1)
    print("Reading %s..." % file2)
    b = read_lines(file2)
    print("Computing diff (%d vs %d lines)..." % (len(a), len(b)))

    # Warn about heavy diffs
    if len(a) * len(b) > 250000:
        print("Note: large files, this may take a while and use significant heap.")

    ops = lcs_diff(a, b)
    rows = pair_modifications(ops)

    # Assign line numbers
    numbered = []
    li = 1
    ri = 1
    for kind, l, r in rows:
        l_num = li if l is not None else None
        r_num = ri if r is not None else None
        numbered.append((kind, l_num, l, r_num, r))
        if l is not None:
            li += 1
        if r is not None:
            ri += 1

    half = (width - 3) // 2  # 3 = " X " central marker

    # Free the diff intermediate to reclaim memory before paging
    ops = None
    rows = None
    try:
        import gc
        gc.collect()
    except ImportError:
        pass

    if not numbered:
        print("Both files are empty.")
        return

    run_pager(numbered, file1, file2, half, width, page_lines)


main()
