#
# http_util.py - Shared HTTP utilities for MicroPython samples.
#
# Factored out of wget.py for reuse by wget.py and aminet_tools.py on the
# MicroPython AmigaOS m68k port (build 230+).
#
# Provides:
#   - SockReader:        buffered reader over a BSD socket (recv/send only)
#   - parse_url():       split a URL into (scheme, host, port, path)
#   - filename_from_url():  guess a sensible filename for downloads
#   - format_size(),
#     format_time(),
#     draw_progress():   helpers for the progress bar
#   - http_request():    low-level single-shot HTTP GET (no redirect handling)
#   - http_get_text():   high-level GET, follows redirects, returns body as str
#   - download_to_file():high-level download to disk with progress bar
#   - wrap_text():       word-boundary text wrap (MicroPython has no textwrap)
#
# Supports http:// and https:// (https requires AmiSSL).
#
# Author: written for Fabrice's MicroPython Amiga port.
#

import sys
import socket
import time

try:
    import ssl
    HAS_SSL = True
except ImportError:
    HAS_SSL = False

MAX_REDIRECTS = 5
BLOCK_SIZE = 4096
USER_AGENT = "MicroPython-http_util/1.0 (AmigaOS)"


# ---------------------------------------------------------------------------
# SockReader
# ---------------------------------------------------------------------------

class SockReader:
    """
    Tiny buffered reader over a BSD-style socket (send/recv only).
    Provides readline() and read(n) without relying on the stream API
    (which is not exposed by the Amiga socket module).

    Works transparently for both raw sockets and ssl-wrapped sockets that
    expose recv()/send().
    """

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self):
        chunk = self.sock.recv(BLOCK_SIZE)
        if not chunk:
            return False
        self.buf += chunk
        return True

    def readline(self):
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl + 1]
                self.buf = self.buf[nl + 1:]
                return line
            if not self._fill():
                line = self.buf
                self.buf = b""
                return line

    def read(self, n):
        # Return up to n bytes; first drain the buffer, otherwise one recv.
        if self.buf:
            if len(self.buf) >= n:
                out = self.buf[:n]
                self.buf = self.buf[n:]
                return out
            out = self.buf
            self.buf = b""
            return out
        try:
            return self.sock.recv(n)
        except OSError:
            return b""

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# URL / formatting helpers
# ---------------------------------------------------------------------------

def parse_url(url):
    """Return (scheme, host, port, path) for a http:// or https:// URL."""
    if url.startswith("http://"):
        scheme = "http"
        rest = url[7:]
        default_port = 80
    elif url.startswith("https://"):
        scheme = "https"
        rest = url[8:]
        default_port = 443
    else:
        raise ValueError("URL must start with http:// or https://")

    slash = rest.find("/")
    if slash < 0:
        hostport = rest
        path = "/"
    else:
        hostport = rest[:slash]
        path = rest[slash:]

    colon = hostport.find(":")
    if colon < 0:
        host = hostport
        port = default_port
    else:
        host = hostport[:colon]
        port = int(hostport[colon + 1:])

    return scheme, host, port, path


def filename_from_url(url):
    """Extract a sensible filename from a URL."""
    # Strip query string.
    q = url.find("?")
    if q >= 0:
        url = url[:q]
    # Take the last path segment.
    slash = url.rfind("/")
    name = url[slash + 1:] if slash >= 0 else url
    if not name or name in (".", ".."):
        name = "index.html"
    return name


def format_size(n):
    if n < 1024:
        return "%dB" % n
    if n < 1024 * 1024:
        return "%.1fK" % (n / 1024)
    if n < 1024 * 1024 * 1024:
        return "%.1fM" % (n / (1024 * 1024))
    return "%.2fG" % (n / (1024 * 1024 * 1024))


def format_time(secs):
    if secs < 0 or secs > 359999:
        return "--:--"
    s = int(secs)
    return "%02d:%02d" % (s // 60, s % 60)


def draw_progress(downloaded, total, start_time, bar_width=30):
    """Draw an in-place progress bar to stdout (no trailing newline)."""
    elapsed = time.time() - start_time
    if elapsed <= 0:
        elapsed = 0.001
    speed = downloaded / elapsed  # bytes per second

    if total > 0:
        frac = downloaded / total
        if frac > 1:
            frac = 1
        filled = int(bar_width * frac)
        bar = "=" * filled + ">" + " " * (bar_width - filled - 1)
        if filled >= bar_width:
            bar = "=" * bar_width
        pct = int(frac * 100)
        remaining = (total - downloaded) / speed if speed > 0 else -1
        line = "\r%3d%% [%s] %s  %s/s  ETA %s" % (
            pct, bar, format_size(downloaded),
            format_size(int(speed)), format_time(remaining),
        )
    else:
        # Unknown size: show a spinner-style indicator.
        spin = "|/-\\"[int(elapsed * 4) % 4]
        line = "\r %s  %s  %s/s  %ss" % (
            spin, format_size(downloaded),
            format_size(int(speed)), int(elapsed),
        )

    sys.stdout.write(line)


# ---------------------------------------------------------------------------
# Low-level HTTP request
# ---------------------------------------------------------------------------

def http_request(url):
    """
    Send an HTTP(S) GET request and return (status, headers_dict, reader).

    No redirect handling here - see _follow_redirects() or the higher-level
    http_get_text() / download_to_file() functions.

    The caller is responsible for closing the returned reader.
    """
    scheme, host, port, path = parse_url(url)

    # Resolve & connect.
    addr_info = socket.getaddrinfo(host, port)
    addr = addr_info[0][-1]
    sock = socket.socket()
    sock.connect(addr)

    if scheme == "https":
        if not HAS_SSL:
            sock.close()
            raise OSError("ssl module not available - cannot fetch https://")
        sock = ssl.wrap_socket(sock, server_hostname=host)

    # Build & send the request.
    req = (
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n"
    ) % (path, host, USER_AGENT)
    data = req.encode("ascii")
    # send() may return short writes - loop until everything is out.
    sent = 0
    while sent < len(data):
        n = sock.send(data[sent:])
        if n <= 0:
            sock.close()
            raise OSError("send() failed")
        sent += n

    reader = SockReader(sock)

    # Status line.
    status_line = reader.readline()
    if not status_line:
        reader.close()
        raise OSError("Empty response from server")
    parts = status_line.decode("ascii", "replace").split(None, 2)
    if len(parts) < 2:
        reader.close()
        raise OSError("Malformed status line: %r" % status_line)
    status = int(parts[1])

    # Headers.
    headers = {}
    while True:
        line = reader.readline()
        if not line or line == b"\r\n" or line == b"\n":
            break
        try:
            text = line.decode("ascii", "replace").rstrip("\r\n")
        except Exception:
            continue
        colon = text.find(":")
        if colon > 0:
            k = text[:colon].strip().lower()
            v = text[colon + 1:].strip()
            headers[k] = v

    return status, headers, reader


# ---------------------------------------------------------------------------
# Redirect handling
# ---------------------------------------------------------------------------

def _follow_redirects(url, max_redirects=MAX_REDIRECTS, verbose=False):
    """
    Internal helper: perform a GET, follow HTTP redirects up to
    max_redirects, and return (final_url, status, headers, reader) with
    a non-redirect status. The caller closes the reader.

    If verbose is True, prints the redirect chain to stdout.
    """
    current_url = url
    redirects = 0

    while True:
        if verbose:
            print("--> %s" % current_url)

        status, headers, reader = http_request(current_url)

        if status in (301, 302, 303, 307, 308):
            loc = headers.get("location")
            reader.close()
            if not loc:
                raise OSError("Redirect %d without Location header" % status)
            redirects += 1
            if redirects > max_redirects:
                raise OSError("Too many redirects (>%d)" % max_redirects)
            # Handle relative redirects.
            if loc.startswith("/"):
                scheme, host, port, _ = parse_url(current_url)
                if (scheme == "http" and port == 80) or \
                   (scheme == "https" and port == 443):
                    current_url = "%s://%s%s" % (scheme, host, loc)
                else:
                    current_url = "%s://%s:%d%s" % (scheme, host, port, loc)
            else:
                current_url = loc
            if verbose:
                print("    redirect -> %s" % current_url)
            continue

        return current_url, status, headers, reader


# ---------------------------------------------------------------------------
# High-level fetchers
# ---------------------------------------------------------------------------

def http_get_text(url, max_bytes=2000000, encoding="latin-1"):
    """
    Fetch a URL and return its body as a decoded str.

    Follows redirects up to MAX_REDIRECTS. Raises OSError on non-200 status
    or network errors.

    max_bytes: safety cap to avoid reading unbounded data into memory.
               Default 2,000,000 (2 MB) is generous for HTML pages.
    encoding:  defaults to latin-1 which matches the Aminet site and the
               AmigaOS native filesystem encoding.
    """
    _final, status, _headers, reader = _follow_redirects(url)
    try:
        if status != 200:
            raise OSError("HTTP %d" % status)

        data = b""
        while len(data) < max_bytes:
            chunk = reader.read(BLOCK_SIZE)
            if not chunk:
                break
            data += chunk

        return data.decode(encoding, "replace")
    finally:
        reader.close()


def download_to_file(url, output_path=None, show_progress=True):
    """
    Download a URL to a local file.

    If output_path is None, a filename is derived from the final URL
    (after redirects). If show_progress is True, the redirect chain, the
    'Saving to' line and a live progress bar are printed to stdout.

    Returns the number of bytes written. Raises OSError on HTTP errors,
    I/O errors, or too many redirects.
    """
    final_url, status, headers, reader = _follow_redirects(
        url, verbose=show_progress)

    try:
        if status != 200:
            raise OSError("HTTP %d" % status)

        if output_path is None:
            output_path = filename_from_url(final_url)

        total = 0
        cl = headers.get("content-length")
        if cl is not None:
            try:
                total = int(cl)
            except ValueError:
                total = 0

        if show_progress:
            print("Saving to: %s   (%s)" % (
                output_path,
                format_size(total) if total else "unknown size"))

        try:
            f = open(output_path, "wb")
        except OSError as e:
            raise OSError(
                "Cannot open '%s' for writing: %s" % (output_path, e))

        downloaded = 0
        start = time.time()
        last_draw = 0

        try:
            while True:
                chunk = reader.read(BLOCK_SIZE)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                if show_progress:
                    now = time.time()
                    if now - last_draw >= 0.2:
                        draw_progress(downloaded, total, start)
                        last_draw = now
        finally:
            f.close()

        if show_progress:
            # Final progress line + newline.
            draw_progress(downloaded, total, start)
            sys.stdout.write("\n")
            elapsed = time.time() - start
            if elapsed <= 0:
                elapsed = 0.001
            avg = downloaded / elapsed
            print("Done. %s in %.1fs (%s/s)" % (
                format_size(downloaded), elapsed,
                format_size(int(avg))))

        return downloaded
    finally:
        reader.close()


# ---------------------------------------------------------------------------
# Text wrapping (MicroPython has no textwrap module)
# ---------------------------------------------------------------------------

def wrap_text(text, width):
    """
    Word-boundary wrap of `text` to lines of at most `width` chars.

    Falls back to hard-split for single words longer than `width`.
    Returns a list of strings. Returns [""] for empty input.

    Used by aminet_tools.py for the desc column hanging-indent wrap.
    """
    if width <= 0:
        return [text]
    if not text:
        return [""]

    words = text.split()
    if not words:
        return [""]

    lines = []
    current = ""
    for w in words:
        # Hard-split any single word longer than the whole width.
        while len(w) > width:
            if current:
                lines.append(current)
                current = ""
            lines.append(w[:width])
            w = w[width:]
        if not current:
            current = w
        elif len(current) + 1 + len(w) <= width:
            current = current + " " + w
        else:
            lines.append(current)
            current = w

    if current:
        lines.append(current)

    return lines
