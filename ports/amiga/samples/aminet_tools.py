#
# aminet_tools.py - Interactive Aminet browser for MicroPython AmigaOS.
#
# Usage (see also samples/aminet_tools.md for the full design document):
#
#   aminet_tools.py              Recent packages for today
#   aminet_tools.py -N           Recent packages N days ago (0 <= N <= 14)
#   aminet_tools.py -all         Recent packages, all 14 days grouped
#   aminet_tools.py "query"      Search Aminet for "query"
#   aminet_tools.py -- "-x"      Search for a query starting with '-'
#
# Modifiers (combine with any mode):
#   -w N    Display width in columns, default 77
#   -l N    Screen height in lines for search pagination, default 23
#           (accounts for header, status line and prompt; budget for
#           package content is N - 3 lines). A package whose wrapped
#           description is larger than the budget is always shown in
#           full on its own screen.
#   -h      Help
#
# Requires http_util.py in the same directory.
#
# Author: written for Fabrice's MicroPython Amiga port (build 230+).
#

import sys
import time
import http_util


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

BASE_URL = "http://m68k.aminet.net"
RECENT_URL = BASE_URL + "/recent"
SEARCH_URL = BASE_URL + "/search"

DEFAULT_WIDTH = 77
DEFAULT_SCREEN_LINES = 23      # -l default: typical AmigaOS shell height
MIN_SCREEN_LINES = 5           # below this, -l is unusable
OVERHEAD_LINES = 3             # "(showing X-Y)" + header + prompt reserve
MIN_WIDTH = 40
MAX_DAYS_BACK = 14

# Column widths (min, ideal). desc has (min, None) because it takes the
# remainder of the width and has no fixed ideal.
COL_MIN = {
    "name": 12, "version": 6, "path": 8,
    "dls": 4, "size": 4, "date": 10, "desc": 20,
}
COL_IDEAL = {
    "name": 18, "version": 8, "path": 9,
    "dls": 5, "size": 5, "date": 10,
}

# Characters that are safe in URL query strings without percent-encoding.
URL_SAFE = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789._-")

# Decimal digit value lookup - used by _parse_int() to avoid the int()
# builtin, which triggers a MicroPython 1.28 Amiga compiler bug when used
# inside function bodies run in __main__ mode (the compiler emits broken
# bytecode and the runtime reports a misleading NameError on the enclosing
# function at the call site).
_DIGIT_VAL = {"0": 0, "1": 1, "2": 2, "3": 3, "4": 4,
              "5": 5, "6": 6, "7": 7, "8": 8, "9": 9}
_HEX_VAL = {"0": 0, "1": 1, "2": 2, "3": 3, "4": 4,
            "5": 5, "6": 6, "7": 7, "8": 8, "9": 9,
            "a": 10, "b": 11, "c": 12, "d": 13, "e": 14, "f": 15,
            "A": 10, "B": 11, "C": 12, "D": 13, "E": 14, "F": 15}


def _parse_int(s):
    """
    Parse a decimal string to an int without using the int() builtin.
    Returns None on invalid input (empty, contains non-digits, etc.).
    Accepts an optional leading '-' for negative numbers.
    """
    if not s:
        return None
    neg = False
    if s[0] == "-":
        neg = True
        s = s[1:]
        if not s:
            return None
    n = 0
    for c in s:
        if c not in _DIGIT_VAL:
            return None
        n = n * 10 + _DIGIT_VAL[c]
    if neg:
        return -n
    return n


def _parse_hex(s):
    """
    Parse a hexadecimal string to an int without using the int() builtin.
    Returns None on invalid input. No '0x' prefix handled.
    """
    if not s:
        return None
    n = 0
    for c in s:
        if c not in _HEX_VAL:
            return None
        n = n * 16 + _HEX_VAL[c]
    return n


# ---------------------------------------------------------------------------
# URL encoding
# ---------------------------------------------------------------------------

def urlencode(s):
    """Minimal percent-encoding for query strings (spaces become '+')."""
    out = []
    for ch in s:
        if ch in URL_SAFE:
            out.append(ch)
        elif ch == " ":
            out.append("+")
        else:
            # Encode per byte in UTF-8 as %HH.
            try:
                b = ch.encode("utf-8")
            except Exception:
                b = ch.encode("latin-1")
            for byte in b:
                out.append("%%%02X" % byte)
    return "".join(out)


# ---------------------------------------------------------------------------
# HTML helpers (no re module - the Aminet HTML is very regular)
# ---------------------------------------------------------------------------

def strip_tags(s):
    """Remove HTML tags, decode entities, strip whitespace."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "<":
            j = s.find(">", i + 1)
            if j < 0:
                break  # malformed, give up
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return decode_entities("".join(out)).strip()


def decode_entities(s):
    """Decode the HTML entities Aminet typically uses."""
    if "&" not in s:
        return s
    s = s.replace("&amp;", "&")
    s = s.replace("&lt;", "<")
    s = s.replace("&gt;", ">")
    s = s.replace("&quot;", '"')
    s = s.replace("&nbsp;", " ")
    s = s.replace("&#39;", "'")
    # Numeric &#NN; entities. Simple loop.
    while True:
        i = s.find("&#")
        if i < 0:
            break
        j = s.find(";", i + 2)
        if j < 0:
            break
        body = s[i + 2:j]
        code = None
        if body and (body[0] == "x" or body[0] == "X"):
            code = _parse_hex(body[1:])
        else:
            code = _parse_int(body)
        if code is None:
            break  # malformed, bail
        s = s[:i] + chr(code) + s[j + 1:]
    return s


def extract_href(s):
    """
    Return the first href="..." value in `s`, HTML-entity-decoded,
    or None.
    Entity decoding matters for URLs with query strings: Aminet writes
    pagination links as href="/search?query=x&amp;page=2" and we need
    to turn those &amp; into & before using the URL for HTTP.
    """
    i = s.find('href="')
    if i < 0:
        return None
    start = i + 6
    end = s.find('"', start)
    if end < 0:
        return None
    return decode_entities(s[start:end])


def _absolute_url(url):
    """
    Prepend BASE_URL to server-relative paths so the URL is usable by
    http_util which requires http:// or https:// prefixes.
    Absolute http(s) URLs are returned unchanged.
    """
    if not url:
        return url
    if url.startswith("http://") or url.startswith("https://"):
        return url
    if url.startswith("/"):
        return BASE_URL + url
    return url


def extract_rows(html):
    """
    Split an HTML blob into a list of row cell-lists.
    Each row is represented as a list of raw cell HTML strings
    (content between <td...> and </td>).
    """
    rows = []
    i = 0
    n = len(html)
    while True:
        tr_start = html.find("<tr", i)
        if tr_start < 0:
            break
        gt = html.find(">", tr_start)
        if gt < 0:
            break
        row_end = html.find("</tr>", gt + 1)
        if row_end < 0:
            break
        row_html = html[gt + 1:row_end]

        cells = []
        j = 0
        rn = len(row_html)
        while True:
            td_start = row_html.find("<td", j)
            if td_start < 0:
                break
            tgt = row_html.find(">", td_start)
            if tgt < 0:
                break
            td_end = row_html.find("</td>", tgt + 1)
            if td_end < 0:
                break
            cells.append(row_html[tgt + 1:td_end])
            j = td_end + 5

        if cells:
            rows.append(cells)
        i = row_end + 5
    return rows


# ---------------------------------------------------------------------------
# Data class
# ---------------------------------------------------------------------------

class Package:
    def __init__(self):
        self.name = ""
        self.version = ""
        self.path = ""
        self.dls = ""
        self.size = ""
        self.date = ""
        self.desc = ""
        self.url = ""  # direct download URL

    def __repr__(self):
        return "Package(%r)" % self.name


# ---------------------------------------------------------------------------
# Row parsing
# ---------------------------------------------------------------------------

def is_date_header(cells):
    """If the row is a YYYY-MM-DD date header, return the date string."""
    if not cells:
        return None
    text = strip_tags(cells[0])
    if len(text) != 10 or text[4] != "-" or text[7] != "-":
        return None
    # Validate that year / month / day are all digits.
    # We use str.isdigit() instead of int() because int() in a function
    # body triggers a MicroPython 1.28 Amiga compiler bug.
    if not text[0:4].isdigit():
        return None
    if not text[5:7].isdigit():
        return None
    if not text[8:10].isdigit():
        return None
    return text


def _clean_desc(cell_html):
    """Strip tags and remove the trailing '- (readme)' anchor."""
    text = strip_tags(cell_html)
    if text.endswith("(readme)"):
        idx = text.rfind(" - ")
        if idx >= 0:
            text = text[:idx].rstrip()
    return text


def parse_recent_row(cells):
    """
    Parse a /recent package row. Expected cells (7):
      name, version, path, dls, size, arch-icon, desc
    Returns a Package or None if the row isn't a package.
    """
    if len(cells) < 7:
        return None
    pkg = Package()
    pkg.url = _absolute_url(extract_href(cells[0]) or "")
    pkg.name = strip_tags(cells[0])
    pkg.version = strip_tags(cells[1])
    pkg.path = strip_tags(cells[2])
    pkg.dls = strip_tags(cells[3])
    pkg.size = strip_tags(cells[4])
    # cells[5] is the arch icon - skip
    pkg.desc = _clean_desc(cells[6])
    if not pkg.name or not pkg.url:
        return None
    # Reject the column header row ("name:", "version:", etc.).
    if pkg.name.endswith(":"):
        return None
    return pkg


def parse_search_row(cells):
    """
    Parse a /search result row. Expected cells (8):
      name, version, path, dls, size, date, arch-icon, desc
    Returns a Package or None.
    """
    if len(cells) < 8:
        return None
    pkg = Package()
    pkg.url = _absolute_url(extract_href(cells[0]) or "")
    pkg.name = strip_tags(cells[0])
    pkg.version = strip_tags(cells[1])
    pkg.path = strip_tags(cells[2])
    pkg.dls = strip_tags(cells[3])
    pkg.size = strip_tags(cells[4])
    pkg.date = strip_tags(cells[5])
    # cells[6] is the arch icon - skip
    pkg.desc = _clean_desc(cells[7])
    if not pkg.name or not pkg.url:
        return None
    # Reject the column header row. On the search page Aminet makes the
    # headers ("name:", "version:", ...) clickable sort links, so they
    # pass the url-presence check above - we have to filter them here.
    if pkg.name.endswith(":"):
        return None
    return pkg


# ---------------------------------------------------------------------------
# Page parsing
# ---------------------------------------------------------------------------

def parse_recent_page(html):
    """
    Parse /recent.
    Returns a list of (date, [Package]) tuples in display order
    (most recent first).
    """
    rows = extract_rows(html)
    groups = []
    current_date = None
    current_pkgs = []

    for cells in rows:
        date = is_date_header(cells)
        if date:
            if current_date is not None:
                groups.append((current_date, current_pkgs))
            current_date = date
            current_pkgs = []
            continue
        pkg = parse_recent_row(cells)
        if pkg and current_date is not None:
            current_pkgs.append(pkg)

    if current_date is not None:
        groups.append((current_date, current_pkgs))
    return groups


def extract_total_count(html):
    """Look for 'Found N matching packages' and return N, or None."""
    marker = "Found "
    i = html.find(marker)
    if i < 0:
        return None
    j = i + len(marker)
    k = j
    while k < len(html) and html[k].isdigit():
        k += 1
    if k > j:
        return _parse_int(html[j:k])
    return None


def find_nav_link(html, label):
    """
    Look for an <a href="...">label</a> in html and return the href or None.
    Kept for potential future use; Aminet pagination uses page numbers
    rather than text labels, see _find_page_links below.
    """
    needle = ">" + label + "<"
    idx = html.find(needle)
    if idx < 0:
        return None
    a_start = html.rfind('href="', 0, idx)
    if a_start < 0:
        return None
    start = a_start + 6
    end = html.find('"', start)
    if end < 0:
        return None
    return decode_entities(html[start:end])


def _find_page_links(html):
    """
    Scan the HTML for all <a href="..."> URLs that contain a page=N
    query parameter, and return a dict mapping N -> absolute URL.

    Aminet paginates search results via a &page=N parameter, and shows
    each page number as a clickable link in the nav (the current page
    is rendered as plain text without a link, so it won't appear here).
    """
    result = {}
    i = 0
    n = len(html)
    while True:
        j = html.find('href="', i)
        if j < 0:
            break
        start = j + 6
        end = html.find('"', start)
        if end < 0:
            break
        i = end + 1
        raw = html[start:end]
        url = decode_entities(raw)
        # Need both "page=" and (optionally) "search" to filter out unrelated
        # links. We accept any URL that has page= as a query param though,
        # since Aminet's only user of page= IS the search pagination.
        p = url.find("page=")
        if p < 0:
            continue
        # Extract the integer value after page=
        v_start = p + 5
        v_end = v_start
        while v_end < len(url) and url[v_end] in _DIGIT_VAL:
            v_end += 1
        if v_end == v_start:
            continue
        page_num = _parse_int(url[v_start:v_end])
        if page_num is None or page_num < 1:
            continue
        # Store only the first occurrence of each page number (top and
        # bottom nav emit duplicates).
        if page_num not in result:
            result[page_num] = _absolute_url(url)
    return result


def _get_page_from_url(url):
    """
    Extract the &page=N parameter from a URL. Returns 1 if absent.
    Used to know which Aminet page we're currently on.
    """
    if not url:
        return 1
    p = url.find("page=")
    if p < 0:
        return 1
    v_start = p + 5
    v_end = v_start
    while v_end < len(url) and url[v_end] in _DIGIT_VAL:
        v_end += 1
    if v_end == v_start:
        return 1
    n = _parse_int(url[v_start:v_end])
    return n if n is not None and n >= 1 else 1


def parse_search_page(html, current_page=1):
    """
    Parse a /search page.
    Returns (packages, next_url, prev_url, total_count).
    next_url / prev_url may be None. total_count may be None.

    current_page: the Aminet page number we're currently displaying.
    Used to determine which page links are "next" (N > current) and
    "previous" (N < current) from the pagination nav.
    """
    rows = extract_rows(html)
    packages = []
    for cells in rows:
        pkg = parse_search_row(cells)
        if pkg:
            packages.append(pkg)

    page_links = _find_page_links(html)
    next_url = None
    prev_url = None
    next_n = None
    prev_n = None
    for n in page_links:
        if n > current_page:
            if next_n is None or n < next_n:
                next_n = n
                next_url = page_links[n]
        elif n < current_page:
            if prev_n is None or n > prev_n:
                prev_n = n
                prev_url = page_links[n]

    # Special case: if we're on page 2+, the link back to page 1 might
    # be expressed without a page= parameter at all (Aminet's default).
    # We can't detect that automatically here because any href that
    # looks like a bare search URL is ambiguous. Instead, cmd_search
    # keeps a history stack that preserves the previous state.

    return packages, next_url, prev_url, extract_total_count(html)


# ---------------------------------------------------------------------------
# Column width algorithm (desc-priority, see design doc §6.1)
# ---------------------------------------------------------------------------

def compute_widths(total_width, has_date):
    """
    Return a dict mapping column name -> width.
    desc is always given the remainder after the other columns.
    If the remainder is below COL_MIN['desc'], other columns are shrunk
    in order: path, version, name, date.
    """
    cols = ["name", "version", "path", "dls", "size"]
    if has_date:
        cols.append("date")
    widths = {c: COL_IDEAL[c] for c in cols}

    # One 2-space separator between each of the (len(cols)+1) columns.
    seps = (len(cols)) * 2

    def remainder():
        return total_width - sum(widths.values()) - seps

    r = remainder()
    desc_min = COL_MIN["desc"]

    if r >= desc_min:
        widths["desc"] = r
        return widths

    # Shrink in order of sacrifice.
    shrink_order = ["path", "version", "name"]
    if has_date:
        shrink_order.append("date")

    for col in shrink_order:
        if remainder() >= desc_min:
            break
        needed = desc_min - remainder()
        can_give = widths[col] - COL_MIN[col]
        take = min(needed, can_give)
        widths[col] -= take

    r = remainder()
    if r >= desc_min:
        widths["desc"] = r
        return widths

    # Not enough room: warn once and use desc_min (overflow is tolerated).
    min_total = sum(COL_MIN[c] for c in cols) + seps + desc_min
    print("Warning: -w %d too small (need >= %d), columns will overflow."
          % (total_width, min_total))
    widths["desc"] = desc_min
    return widths


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def _truncate(text, width):
    """Truncate `text` to `width`, marking with '>' if cut."""
    if len(text) <= width:
        return text
    return text[:width - 1] + ">"


def _ljust(text, width):
    """Left-pad `text` with spaces to `width` (MicroPython has no str.ljust)."""
    n = len(text)
    if n >= width:
        return text
    return text + " " * (width - n)


def _rjust(text, width):
    """Right-pad `text` with spaces to `width` (MicroPython has no str.rjust)."""
    n = len(text)
    if n >= width:
        return text
    return " " * (width - n) + text


def _fixed_part(pkg, widths, has_date):
    """
    Build the fixed-width prefix for a package row (everything before desc).
    Returns the prefix string and its display length.
    """
    parts = [
        _ljust(_truncate(pkg.name, widths["name"]), widths["name"]),
        _ljust(_truncate(pkg.version, widths["version"]), widths["version"]),
        _ljust(_truncate(pkg.path, widths["path"]), widths["path"]),
        _rjust(_truncate(pkg.dls, widths["dls"]), widths["dls"]),
        _rjust(_truncate(pkg.size, widths["size"]), widths["size"]),
    ]
    if has_date:
        parts.append(_ljust(_truncate(pkg.date, widths["date"]), widths["date"]))
    prefix = "  ".join(parts)
    return prefix


def format_package_lines(pkg, widths, has_date):
    """
    Render a Package as one or more lines (hanging-indent wrap on desc).
    Returns a list of strings.
    """
    prefix = _fixed_part(pkg, widths, has_date)
    # desc starts 2 chars after prefix
    desc_col = len(prefix) + 2
    desc_lines = http_util.wrap_text(pkg.desc, widths["desc"])
    if not desc_lines:
        desc_lines = [""]
    result = [prefix + "  " + desc_lines[0]]
    indent = " " * desc_col
    for cont in desc_lines[1:]:
        result.append(indent + cont)
    return result


def format_header_line(widths, has_date):
    parts = [
        _ljust("name", widths["name"]),
        _ljust("version", widths["version"]),
        _ljust("path", widths["path"]),
        _rjust("dls", widths["dls"]),
        _rjust("size", widths["size"]),
    ]
    if has_date:
        parts.append(_ljust("date", widths["date"]))
    parts.append("desc")
    return "  ".join(parts)


def _compute_table_widths(total_items, width, has_date, with_numbers):
    """
    Factored width computation. Returns (widths, num_width, num_prefix_len).

    `total_items` drives the numbering column width so that, within a
    single Aminet search page rendered over several screens, the prefix
    stays stable (and therefore `widths` and the per-package line-count
    used for the line-budget planner stay consistent).
    """
    if with_numbers:
        max_num = total_items if total_items > 0 else 1
        num_width = len(str(max_num))
        num_prefix_len = num_width + 2  # "NN) "
        effective_width = width - num_prefix_len
    else:
        num_width = 0
        num_prefix_len = 0
        effective_width = width
    widths = compute_widths(effective_width, has_date)
    return widths, num_width, num_prefix_len


def _print_screen(displayed, widths, has_date, num_width, num_prefix_len):
    """
    Print one screen: header line followed by `displayed` packages.
    Numbers are screen-relative (1..len(displayed)) — this matches the
    existing search UX where the download command `<number>` indexes
    into the currently visible slice.
    """
    header = format_header_line(widths, has_date)
    if num_prefix_len == 0:
        print(header)
        for pkg in displayed:
            for line in format_package_lines(pkg, widths, has_date):
                print(line)
        return

    num_prefix = " " * num_prefix_len
    print(num_prefix + header)
    for i, pkg in enumerate(displayed):
        num_str = ("%" + str(num_width) + "d) ") % (i + 1)
        lines = format_package_lines(pkg, widths, has_date)
        print(num_str + lines[0])
        for cont in lines[1:]:
            print(num_prefix + cont)


def print_package_table(packages, width, has_date, with_numbers=False):
    """Print a formatted table of packages with optional line numbers."""
    widths, num_width, num_prefix_len = _compute_table_widths(
        len(packages), width, has_date, with_numbers)
    _print_screen(packages, widths, has_date, num_width, num_prefix_len)


# ---------------------------------------------------------------------------
# Date helpers (no mktime epoch trickery - simple calendar arithmetic)
# ---------------------------------------------------------------------------

def _days_in_month(year, month):
    if month in (1, 3, 5, 7, 8, 10, 12):
        return 31
    if month in (4, 6, 9, 11):
        return 30
    # February
    if (year % 4 == 0 and year % 100 != 0) or year % 400 == 0:
        return 29
    return 28


def date_offset_str(offset_days):
    """Return today minus `offset_days` days as a YYYY-MM-DD string."""
    t = time.localtime()
    y, m, d = t[0], t[1], t[2]
    n = offset_days
    while n > 0:
        if d > 1:
            d -= 1
        else:
            m -= 1
            if m < 1:
                m = 12
                y -= 1
            d = _days_in_month(y, m)
        n -= 1
    return "%04d-%02d-%02d" % (y, m, d)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_recent_day(offset, width):
    target = date_offset_str(offset)
    try:
        html = http_util.http_get_text(RECENT_URL)
    except Exception as e:
        print("Error fetching %s: %s" % (RECENT_URL, e))
        return
    groups = parse_recent_page(html)
    for date, pkgs in groups:
        if date == target:
            if not pkgs:
                print("Aucun paquet publie sur Aminet pour le %s." % target)
                return
            print("=== %s ===" % target)
            print_package_table(pkgs, width, has_date=False)
            return
    print("Aucun paquet publie sur Aminet pour le %s." % target)


def cmd_recent_all(width):
    try:
        html = http_util.http_get_text(RECENT_URL)
    except Exception as e:
        print("Error fetching %s: %s" % (RECENT_URL, e))
        return
    groups = parse_recent_page(html)
    if not groups:
        print("No data returned from Aminet.")
        return
    first = True
    for date, pkgs in groups:
        if not first:
            print()
        first = False
        print("=== %s ===" % date)
        if pkgs:
            print_package_table(pkgs, width, has_date=False)
        else:
            print("(no packages)")


def cmd_search(query, width, screen_lines):
    encoded = urlencode(query)
    url = "%s?query=%s&sort=date&ord=DESC" % (SEARCH_URL, encoded)

    # Navigation state.
    display_offset = 0        # First item index shown on the current screen
    screen_label = 1          # User-visible page counter (monotonically growing)
    history = []              # Stack of (url, display_offset) for 'p'

    # Line budget for package content per screen (the rest of `screen_lines`
    # is reserved for the "(showing X-Y)" status line, the table header and
    # the prompt itself).
    line_budget = screen_lines - OVERHEAD_LINES
    if line_budget < 1:
        line_budget = 1

    # Current page cache (avoid refetching on in-page navigation and after
    # downloads).
    packages = None
    next_url = None
    prev_url_from_aminet = None
    total = None
    needs_fetch = True
    is_first_screen = True

    while True:
        if needs_fetch:
            try:
                html = http_util.http_get_text(url)
            except Exception as e:
                print("Error fetching search: %s" % e)
                return
            current_aminet_page = _get_page_from_url(url)
            packages, next_url, prev_url_from_aminet, total = (
                parse_search_page(html, current_aminet_page))
            needs_fetch = False

            if is_first_screen:
                if not packages:
                    print("No results for '%s'." % query)
                    return
                if total is not None:
                    print("Found %d matching packages." % total)
                is_first_screen = False

            if not packages:
                # Should be rare; means we navigated to an empty page
                print("(empty page)")
                # Fall through to the prompt so the user can go back

        total_on_page = len(packages) if packages else 0

        # Precompute widths once per Aminet page, using total_on_page so
        # that numbering width (and therefore `widths` and per-package
        # line costs) stay consistent across all screens of that page.
        widths, num_width, num_prefix_len = _compute_table_widths(
            total_on_page, width, has_date=True, with_numbers=True)

        # Pack packages into the line budget starting at display_offset.
        # Rules:
        #   - always advance by at least one package, even if its rendered
        #     height exceeds the budget (otherwise a very long description
        #     would freeze navigation);
        #   - otherwise stop as soon as adding the next package would
        #     exceed line_budget.
        end = display_offset
        lines_used = 0
        while end < total_on_page:
            cost = len(format_package_lines(
                packages[end], widths, has_date=True))
            if end == display_offset:
                # Force-show at least one package.
                end += 1
                lines_used += cost
                continue
            if lines_used + cost > line_budget:
                break
            end += 1
            lines_used += cost
        displayed = packages[display_offset:end] if packages else []

        # Status line: shown whenever the current Aminet page doesn't fit
        # on one screen. We reserved a line for it in the budget either
        # way, so printing it is always safe.
        if total_on_page > 0 and (display_offset > 0 or end < total_on_page):
            print("(showing %d-%d of %d on this page)"
                  % (display_offset + 1, end, total_on_page))

        if displayed:
            _print_screen(displayed, widths, has_date=True,
                          num_width=num_width,
                          num_prefix_len=num_prefix_len)

        # Compute navigation capability BEFORE the inner loop so the prompt
        # reflects the real state.
        has_more_on_page = end < total_on_page
        can_next = has_more_on_page or (next_url is not None)
        can_prev = (display_offset > 0
                    or len(history) > 0
                    or prev_url_from_aminet is not None)

        # Inner action loop: stay on this screen until next/prev/quit.
        action = None
        while action is None:
            nav = []
            if can_next:
                nav.append("[n]ext")
            if can_prev:
                nav.append("[p]rev")
            nav.append("[q]uit")
            if displayed:
                nav.append("[1-%d] download" % len(displayed))
            nav.append("[?] help")
            prompt = "Page %d - %s > " % (screen_label, "  ".join(nav))

            try:
                choice = input(prompt).strip().lower()
            except (KeyboardInterrupt, EOFError):
                print()
                return

            if choice == "" or choice == "n":
                if can_next:
                    action = "next"
                else:
                    print("(already on last page)")
            elif choice == "p":
                if can_prev:
                    action = "prev"
                else:
                    print("(already on first page)")
            elif choice == "q":
                return
            elif choice == "?":
                print("Keys:  n or Enter = next page")
                print("       p          = previous page")
                print("       q          = quit")
                print("       <number>   = select and download that package")
                print("       ?          = this help")
            else:
                n = _parse_int(choice)
                if n is None:
                    print("(unknown command: %r)" % choice)
                    continue
                if n < 1 or n > len(displayed):
                    print("(number out of range: %d)" % n)
                    continue
                cmd_download(displayed[n - 1])
                print()
                # Stay on same screen - no refetch needed thanks to cache.

        # Apply navigation.
        if action == "next":
            if has_more_on_page:
                # Advance within the current Aminet page - no refetch.
                # Push the current state so 'prev' can restore this exact
                # screen (screens have variable size in line-budget mode,
                # so we can't derive the previous offset from a delta).
                history.append((url, display_offset))
                display_offset = end
                screen_label += 1
                needs_fetch = False
            else:
                # Move to Aminet's next page - refetch.
                history.append((url, display_offset))
                url = next_url
                display_offset = 0
                screen_label += 1
                needs_fetch = True
        elif action == "prev":
            if history:
                # Restore the exact previous screen (intra- or cross-page).
                prev_url_state, prev_offset_state = history.pop()
                if prev_url_state != url:
                    url = prev_url_state
                    needs_fetch = True
                else:
                    needs_fetch = False
                display_offset = prev_offset_state
                screen_label = max(1, screen_label - 1)
            elif prev_url_from_aminet:
                # Fallback: we don't have a history entry but Aminet gave
                # us a Prev link. We can't know the exact offset, show the
                # start of that page.
                url = prev_url_from_aminet
                display_offset = 0
                screen_label = max(1, screen_label - 1)
                needs_fetch = True


def cmd_download(pkg):
    """Download flow: show details, ask destination, call http_util."""
    print()
    print("Selected: %s" % pkg.name)
    if pkg.version:
        print("  Version: %s" % pkg.version)
    print("  Path:    %s" % pkg.path)
    print("  Size:    %s" % pkg.size)
    if pkg.date:
        print("  Date:    %s" % pkg.date)
    print("  Desc:    %s" % pkg.desc)
    print()

    try:
        dest = input("Destination directory [T:] : ").strip()
    except (KeyboardInterrupt, EOFError):
        print()
        return
    if not dest:
        dest = "T:"

    # AmigaOS path separators: ':' for volume roots, '/' for subdirs.
    if not (dest.endswith("/") or dest.endswith(":")):
        dest = dest + "/"
    output_path = dest + pkg.name

    try:
        http_util.download_to_file(pkg.url, output_path, show_progress=True)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    except Exception as e:
        print("\nError: %s" % e)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def print_usage():
    print("Usage: aminet_tools.py [options] [query]")
    print("")
    print("Recent mode (no query):")
    print("  aminet_tools.py             Packages published today")
    print("  aminet_tools.py -N          Packages published N days ago")
    print("                              (N from 0 to 14)")
    print("  aminet_tools.py -all        All 14 days, grouped by date")
    print("")
    print("Search mode:")
    print("  aminet_tools.py <query>     Search Aminet for <query>")
    print("  aminet_tools.py -- -query   Use -- if query starts with '-'")
    print("")
    print("Options (combine with any mode):")
    print("  -w N    Display width in columns (default %d, min %d)"
          % (DEFAULT_WIDTH, MIN_WIDTH))
    print("  -l N    Screen height in lines, search mode"
          " (default %d, min %d)"
          % (DEFAULT_SCREEN_LINES, MIN_SCREEN_LINES))
    print("  -h      This help")


def parse_args(argv):
    """
    Parse CLI arguments. Returns a dict of options or None on error/help.
    """
    opts = {
        "mode": None,       # 'day', 'all', or 'search'
        "offset": 0,
        "query": None,
        "width": DEFAULT_WIDTH,
        "screen_lines": DEFAULT_SCREEN_LINES,
    }

    end_of_options = False
    i = 0
    while i < len(argv):
        a = argv[i]

        if end_of_options:
            if opts["query"] is not None:
                print("aminet_tools: multiple query strings")
                return None
            opts["query"] = a
            i += 1
            continue

        if a == "--":
            end_of_options = True
            i += 1
            continue

        if a in ("-h", "--help"):
            print_usage()
            return None

        if a == "-all":
            if opts["mode"] is not None:
                print("aminet_tools: -all conflicts with another mode")
                return None
            opts["mode"] = "all"
            i += 1
            continue

        if a == "-w":
            i += 1
            if i >= len(argv):
                print("aminet_tools: -w requires a number")
                return None
            val = _parse_int(argv[i])
            if val is None:
                print("aminet_tools: -w needs an integer")
                return None
            opts["width"] = val
            i += 1
            continue

        if a == "-l":
            i += 1
            if i >= len(argv):
                print("aminet_tools: -l requires a number")
                return None
            val = _parse_int(argv[i])
            if val is None:
                print("aminet_tools: -l needs an integer")
                return None
            opts["screen_lines"] = val
            i += 1
            continue

        # -N where N is a non-negative integer.
        if len(a) > 1 and a[0] == "-" and a[1:].isdigit():
            if opts["mode"] is not None:
                print("aminet_tools: -N conflicts with another mode")
                return None
            offset = _parse_int(a[1:])
            if offset is None or offset > MAX_DAYS_BACK:
                print("aminet_tools: Aminet only keeps %d days of history"
                      % MAX_DAYS_BACK)
                return None
            opts["mode"] = "day"
            opts["offset"] = offset
            i += 1
            continue

        if a.startswith("-"):
            print("aminet_tools: unknown option %r" % a)
            return None

        # Positional query.
        if opts["query"] is not None:
            print("aminet_tools: multiple query strings")
            return None
        opts["query"] = a
        i += 1

    # Resolve final mode.
    if opts["query"] is not None:
        if opts["mode"] is not None:
            print("aminet_tools: query conflicts with -N or -all")
            return None
        opts["mode"] = "search"
    elif opts["mode"] is None:
        opts["mode"] = "day"
        opts["offset"] = 0

    # Sanity checks.
    if opts["width"] < MIN_WIDTH:
        print("aminet_tools: -w too small (min %d)" % MIN_WIDTH)
        return None
    if opts["screen_lines"] < MIN_SCREEN_LINES:
        print("aminet_tools: -l must be >= %d" % MIN_SCREEN_LINES)
        return None

    return opts


def main():
    opts = parse_args(sys.argv[1:])
    if opts is None:
        return

    try:
        if opts["mode"] == "day":
            cmd_recent_day(opts["offset"], opts["width"])
        elif opts["mode"] == "all":
            cmd_recent_all(opts["width"])
        elif opts["mode"] == "search":
            cmd_search(opts["query"], opts["width"], opts["screen_lines"])
    except KeyboardInterrupt:
        print("\nInterrupted.")


if __name__ == "__main__":
    main()
