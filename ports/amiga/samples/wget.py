#
# wget.py - Minimal wget clone for MicroPython (AmigaOS m68k port)
#
# Usage:
#   micropython wget.py <url> [-O output_file]
#
# Supports http:// and https:// (https requires AmiSSL).
# Follows redirects (max 5), shows a text progress bar with speed & ETA.
#
# Author: written for Fabrice's MicroPython Amiga port (build 227+)
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
USER_AGENT = "MicroPython-wget/1.0 (AmigaOS)"


class SockReader:
    """
    Tiny buffered reader over a BSD-style socket (send/recv only).
    Provides readline() and read(n) without relying on stream API.
    Works for both raw sockets and ssl-wrapped sockets that expose recv/send.
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
        # Return up to n bytes; first drain buffer, otherwise one recv.
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


def parse_url(url):
    """Return (scheme, host, port, path)."""
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
    # Strip query string
    q = url.find("?")
    if q >= 0:
        url = url[:q]
    # Take last path segment
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
    """Draw an in-place progress bar to stdout."""
    elapsed = time.time() - start_time
    if elapsed <= 0:
        elapsed = 0.001
    speed = downloaded / elapsed  # bytes/sec

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
        # Unknown size: show spinner-style indicator
        spin = "|/-\\"[int(elapsed * 4) % 4]
        line = "\r %s  %s  %s/s  %ss" % (
            spin, format_size(downloaded),
            format_size(int(speed)), int(elapsed),
        )

    sys.stdout.write(line)


def http_request(url):
    """
    Send an HTTP(S) GET request and return (status, headers_dict, sock_file).
    The caller reads the body from sock_file and must close it.
    """
    scheme, host, port, path = parse_url(url)

    # Resolve & connect
    addr_info = socket.getaddrinfo(host, port)
    addr = addr_info[0][-1]
    sock = socket.socket()
    sock.connect(addr)

    if scheme == "https":
        if not HAS_SSL:
            sock.close()
            raise OSError("ssl module not available - cannot fetch https://")
        sock = ssl.wrap_socket(sock, server_hostname=host)

    # Build & send request
    req = (
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n"
    ) % (path, host, USER_AGENT)
    data = req.encode("ascii")
    # send() may return short writes - loop until everything is out
    sent = 0
    while sent < len(data):
        n = sock.send(data[sent:])
        if n <= 0:
            sock.close()
            raise OSError("send() failed")
        sent += n

    reader = SockReader(sock)

    status_line = reader.readline()
    if not status_line:
        reader.close()
        raise OSError("Empty response from server")
    parts = status_line.decode("ascii", "replace").split(None, 2)
    if len(parts) < 2:
        reader.close()
        raise OSError("Malformed status line: %r" % status_line)
    status = int(parts[1])

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


def download(url, output=None):
    redirects = 0
    current_url = url

    while True:
        print("--> %s" % current_url)
        status, headers, sock = http_request(current_url)

        if status in (301, 302, 303, 307, 308):
            loc = headers.get("location")
            sock.close()
            if not loc:
                raise OSError("Redirect %d without Location header" % status)
            redirects += 1
            if redirects > MAX_REDIRECTS:
                raise OSError("Too many redirects (>%d)" % MAX_REDIRECTS)
            # Handle relative redirects
            if loc.startswith("/"):
                scheme, host, port, _ = parse_url(current_url)
                if (scheme == "http" and port == 80) or \
                   (scheme == "https" and port == 443):
                    current_url = "%s://%s%s" % (scheme, host, loc)
                else:
                    current_url = "%s://%s:%d%s" % (scheme, host, port, loc)
            else:
                current_url = loc
            print("    redirect -> %s" % current_url)
            continue

        if status != 200:
            sock.close()
            raise OSError("HTTP %d" % status)

        # Determine output filename
        if output is None:
            output = filename_from_url(current_url)

        total = 0
        cl = headers.get("content-length")
        if cl is not None:
            try:
                total = int(cl)
            except ValueError:
                total = 0

        print("Saving to: %s   (%s)" % (
            output, format_size(total) if total else "unknown size"))

        downloaded = 0
        start = time.time()
        last_draw = 0

        try:
            f = open(output, "wb")
        except OSError as e:
            sock.close()
            raise OSError("Cannot open '%s' for writing: %s" % (output, e))

        try:
            while True:
                chunk = sock.read(BLOCK_SIZE)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                now = time.time()
                if now - last_draw >= 0.2:
                    draw_progress(downloaded, total, start)
                    last_draw = now
        finally:
            f.close()
            sock.close()

        # Final progress line + newline
        draw_progress(downloaded, total, start)
        sys.stdout.write("\n")

        elapsed = time.time() - start
        if elapsed <= 0:
            elapsed = 0.001
        avg = downloaded / elapsed
        print("Done. %s in %.1fs (%s/s)" % (
            format_size(downloaded), elapsed, format_size(int(avg))))
        return


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help"):
        print("Usage: wget.py <url> [-O output_file]")
        print("  Supports http:// and https:// (https needs AmiSSL).")
        return

    url = None
    output = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-O":
            i += 1
            if i >= len(argv):
                print("wget: -O requires a filename")
                return
            output = argv[i]
        elif a.startswith("-"):
            print("wget: unknown option %s" % a)
            return
        else:
            url = a
        i += 1

    if url is None:
        print("wget: missing URL")
        return

    try:
        download(url, output)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    except Exception as e:
        print("\nError: %s" % e)


main()
