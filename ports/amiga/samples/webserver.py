#
# webserver.py - Minimal single-threaded HTTP file server for MicroPython
#                (AmigaOS m68k port).
#
# Usage:
#   micropython webserver.py <port> <path>
#
#   <path> may be either a directory (browsable index) or a single file
#   (served at /, with sibling files reachable via relative links).
#
# Author: written for Fabrice's MicroPython Amiga port (build 227+)
#

import sys
import socket
import os

BLOCK_SIZE = 4096
S_IFDIR = 0x4000   # POSIX directory bit in stat[0]

MIME_TYPES = {
    "html": "text/html; charset=utf-8",
    "htm":  "text/html; charset=utf-8",
    "txt":  "text/plain; charset=utf-8",
    "css":  "text/css; charset=utf-8",
    "js":   "application/javascript",
    "json": "application/json",
    "xml":  "application/xml",
    "png":  "image/png",
    "jpg":  "image/jpeg",
    "jpeg": "image/jpeg",
    "gif":  "image/gif",
    "bmp":  "image/bmp",
    "ico":  "image/x-icon",
    "svg":  "image/svg+xml",
    "pdf":  "application/pdf",
    "zip":  "application/zip",
    "lha":  "application/x-lzh-compressed",
    "lzh":  "application/x-lzh-compressed",
    "gz":   "application/gzip",
    "tar":  "application/x-tar",
    "mp3":  "audio/mpeg",
    "wav":  "audio/wav",
    "mod":  "audio/x-mod",
}

INLINE_TYPES = ("text/", "image/", "application/pdf",
                "application/json", "application/xml",
                "application/javascript")


# ---------------------------------------------------------------------------
# Tiny buffered reader over BSD socket (same idea as wget.py)
# ---------------------------------------------------------------------------
class SockReader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self):
        try:
            chunk = self.sock.recv(BLOCK_SIZE)
        except OSError:
            return False
        if not chunk:
            return False
        self.buf += chunk
        return True

    def readline(self, limit=4096):
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl + 1]
                self.buf = self.buf[nl + 1:]
                return line
            if len(self.buf) >= limit:
                line = self.buf
                self.buf = b""
                return line
            if not self._fill():
                line = self.buf
                self.buf = b""
                return line


def send_all(sock, data):
    """Send everything, handling short writes."""
    sent = 0
    n = len(data)
    while sent < n:
        try:
            k = sock.send(data[sent:])
        except OSError:
            return False
        if k <= 0:
            return False
        sent += k
    return True


# ---------------------------------------------------------------------------
# URL helpers
# ---------------------------------------------------------------------------
def url_decode(s):
    """Decode %xx and '+' in URL paths."""
    if "%" not in s and "+" not in s:
        return s
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "+":
            out.append(" ")
            i += 1
        elif c == "%" and i + 2 < n:
            try:
                out.append(chr(int(s[i + 1:i + 3], 16)))
                i += 3
            except ValueError:
                out.append(c)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def url_encode(s):
    """Percent-encode characters that aren't safe in a URL path segment."""
    safe = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789-_.~")
    out = []
    for ch in s:
        if ch in safe:
            out.append(ch)
        else:
            try:
                b = ch.encode("utf-8")
            except Exception:
                b = ch.encode("latin-1")
            for byte in b:
                out.append("%%%02X" % byte)
    return "".join(out)


def html_escape(s):
    return (s.replace("&", "&amp;")
             .replace("<", "&lt;")
             .replace(">", "&gt;")
             .replace('"', "&quot;"))


def split_url_segments(url_path):
    """
    Split a URL path like '/foo/bar/baz.txt' into ['foo','bar','baz.txt'].
    Rejects '..' and empty/'.' segments. Returns None on invalid path.
    """
    # Strip query string
    q = url_path.find("?")
    if q >= 0:
        url_path = url_path[:q]
    raw = url_path.split("/")
    out = []
    for seg in raw:
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            return None  # forbidden
        out.append(url_decode(seg))
    return out


# ---------------------------------------------------------------------------
# Filesystem helpers
# ---------------------------------------------------------------------------
def stat_or_none(path):
    try:
        return os.stat(path)
    except OSError:
        return None


def is_dir(st):
    return st is not None and (st[0] & S_IFDIR) != 0


def join_path(root, segments):
    """Join the root with a list of URL segments using '/' separator."""
    if not segments:
        return root
    if root.endswith("/") or root.endswith(":"):
        return root + "/".join(segments)
    return root + "/" + "/".join(segments)


def mime_for(filename):
    dot = filename.rfind(".")
    if dot < 0:
        return "application/octet-stream"
    ext = filename[dot + 1:].lower()
    return MIME_TYPES.get(ext, "application/octet-stream")


def is_inline(mime):
    for prefix in INLINE_TYPES:
        if mime.startswith(prefix):
            return True
    return False


def format_size(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024)
    if n < 1024 * 1024 * 1024:
        return "%.1f MB" % (n / (1024 * 1024))
    return "%.2f GB" % (n / (1024 * 1024 * 1024))


# ---------------------------------------------------------------------------
# HTTP response helpers
# ---------------------------------------------------------------------------
def send_response(sock, status, status_text, body, content_type="text/html; charset=utf-8",
                  extra_headers=None, head_only=False):
    if isinstance(body, str):
        body = body.encode("utf-8")
    headers = [
        "HTTP/1.0 %d %s" % (status, status_text),
        "Content-Type: %s" % content_type,
        "Content-Length: %d" % len(body),
        "Connection: close",
        "Server: MicroPython-webserver/1.0 (AmigaOS)",
    ]
    if extra_headers:
        for h in extra_headers:
            headers.append(h)
    head = ("\r\n".join(headers) + "\r\n\r\n").encode("ascii")
    send_all(sock, head)
    if not head_only:
        send_all(sock, body)


def send_error(sock, status, status_text, message=None):
    if message is None:
        message = status_text
    body = ("<html><head><title>%d %s</title></head>"
            "<body><h1>%d %s</h1><p>%s</p></body></html>") % (
        status, status_text, status, status_text, html_escape(message))
    send_response(sock, status, status_text, body)


def send_file(sock, local_path, filename, head_only=False):
    st = stat_or_none(local_path)
    if st is None:
        send_error(sock, 404, "Not Found", "File not found: " + filename)
        return
    size = st[6]
    mime = mime_for(filename)
    extra = []
    if not is_inline(mime):
        extra.append('Content-Disposition: attachment; filename="%s"' % filename)

    headers = [
        "HTTP/1.0 200 OK",
        "Content-Type: %s" % mime,
        "Content-Length: %d" % size,
        "Connection: close",
        "Server: MicroPython-webserver/1.0 (AmigaOS)",
    ]
    headers.extend(extra)
    head = ("\r\n".join(headers) + "\r\n\r\n").encode("ascii")
    if not send_all(sock, head):
        return
    if head_only:
        return

    try:
        f = open(local_path, "rb")
    except OSError as e:
        # Headers already sent - just bail.
        print("  ! cannot open %s: %s" % (local_path, e))
        return
    try:
        while True:
            chunk = f.read(BLOCK_SIZE)
            if not chunk:
                break
            if not send_all(sock, chunk):
                break
    finally:
        f.close()


# ---------------------------------------------------------------------------
# Directory listing (streaming - no big allocations)
# ---------------------------------------------------------------------------
def stream_listing(sock, local_path, url_segments, root_label):
    """
    Stream an HTML directory listing directly to the socket.
    Returns True on success, False if the connection broke.
    Uses HTTP/1.0 without Content-Length: end of body = connection close.
    """
    try:
        names = os.listdir(local_path)
    except OSError as e:
        send_error(sock, 500, "Internal Server Error", "listdir failed: %s" % e)
        return True

    # Send headers immediately
    head = (
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Server: MicroPython-webserver/1.0 (AmigaOS)\r\n"
        "\r\n"
    ).encode("ascii")
    if not send_all(sock, head):
        return False

    def w(s):
        if isinstance(s, str):
            s = s.encode("utf-8")
        return send_all(sock, s)

    if url_segments:
        crumb = "/" + "/".join(url_segments) + "/"
    else:
        crumb = "/"

    # --- HTML head + opening ---
    if not w("<!DOCTYPE html><html><head><meta charset='utf-8'><title>Index of "):
        return False
    if not w(html_escape(crumb)):
        return False
    if not w("</title><style>"
             "body{font-family:monospace;background:#fafafa;color:#222;margin:2em;}"
             "h1{font-size:1.1em;border-bottom:1px solid #888;padding-bottom:.3em;}"
             "table{border-collapse:collapse;width:100%;}"
             "th,td{text-align:left;padding:.2em .8em;}"
             "th{border-bottom:1px solid #888;}"
             "tr:hover{background:#eef;}"
             "a{text-decoration:none;color:#036;}a:hover{text-decoration:underline;}"
             ".dir{font-weight:bold;}.size{text-align:right;color:#555;}"
             ".foot{margin-top:1em;color:#888;font-size:.85em;}"
             "</style></head><body>"):
        return False
    if not w("<h1>Index of "):
        return False
    if not w(html_escape(crumb)):
        return False
    if not w("</h1><p>Serving: <code>"):
        return False
    if not w(html_escape(root_label)):
        return False
    if not w("</code></p><table>"
             "<tr><th>Name</th><th class='size'>Size</th></tr>"):
        return False

    # --- Parent link (only if not at root) ---
    if url_segments:
        parent = url_segments[:-1]
        if parent:
            parent_url = "/" + "/".join(url_encode(s) for s in parent) + "/"
        else:
            parent_url = "/"
        if not w("<tr><td class='dir'><a href='"):
            return False
        if not w(parent_url):
            return False
        if not w("'>../</a></td><td class='size'>-</td></tr>"):
            return False

    # --- Entries: split dirs and files in two passes, no full sort ---
    # We collect just names to keep memory low; then do two passes.
    # On big directories this trades a bit of CPU for much less RAM.
    dirs = []
    files = []
    for name in names:
        child = join_path(local_path, [name])
        st = stat_or_none(child)
        if is_dir(st):
            dirs.append(name)
        else:
            # Keep size inline with name to avoid a second stat() pass
            size = -1
            if st is not None:
                try:
                    size = st[6]
                except (IndexError, TypeError):
                    size = -1
            files.append((name, size))

    # Sort each list (cheap compared to building it)
    dirs.sort(key=lambda n: n.lower())
    files.sort(key=lambda e: e[0].lower())

    # Stream directories first
    base_url = "/" + "/".join(url_encode(s) for s in url_segments)
    if url_segments:
        base_url += "/"
    else:
        # base_url == "/" already
        pass

    for name in dirs:
        href = base_url + url_encode(name) + "/"
        row = ("<tr><td class='dir'><a href='" + href + "'>"
               + html_escape(name) + "/</a></td>"
               "<td class='size'>-</td></tr>")
        if not w(row):
            return False
        # Hint the GC every so often on large directories
        del row

    # Then files
    count = 0
    for name, size in files:
        href = base_url + url_encode(name)
        if size >= 0:
            size_str = format_size(size)
        else:
            size_str = "?"
        row = ("<tr><td><a href='" + href + "'>"
               + html_escape(name) + "</a></td>"
               "<td class='size'>" + size_str + "</td></tr>")
        if not w(row):
            return False
        del row
        count += 1
        # Periodic GC nudge to stay safe on huge directories
        if (count & 31) == 0:
            try:
                import gc
                gc.collect()
            except Exception:
                pass

    if not w("</table><p class='foot'>MicroPython webserver on AmigaOS</p>"
             "</body></html>"):
        return False
    return True


# ---------------------------------------------------------------------------
# Request handling
# ---------------------------------------------------------------------------
def handle_request(client, addr, mode, root, single_file_name):
    """
    mode: "dir" or "file"
    root: in dir mode = root directory; in file mode = parent directory
    single_file_name: in file mode = the basename to serve at "/"
    """
    reader = SockReader(client)

    # Parse request line
    request_line = reader.readline()
    if not request_line:
        return
    try:
        line = request_line.decode("ascii", "replace").rstrip("\r\n")
    except Exception:
        send_error(client, 400, "Bad Request")
        return
    parts = line.split(" ")
    if len(parts) < 2:
        send_error(client, 400, "Bad Request")
        return
    method = parts[0]
    url_path = parts[1]

    # Drain headers (we don't care about them)
    while True:
        h = reader.readline()
        if not h or h == b"\r\n" or h == b"\n":
            break

    print("%s:%d  %s %s" % (addr[0], addr[1], method, url_path))

    if method not in ("GET", "HEAD"):
        send_error(client, 405, "Method Not Allowed")
        return
    head_only = (method == "HEAD")

    segments = split_url_segments(url_path)
    if segments is None:
        send_error(client, 403, "Forbidden", "Path traversal not allowed")
        return

    # ----- File mode -----
    if mode == "file":
        if not segments:
            # Serve the original file at "/"
            local = join_path(root, [single_file_name])
            send_file(client, local, single_file_name, head_only=head_only)
            return
        # Otherwise serve sibling files from the same root directory
        local = join_path(root, segments)
        st = stat_or_none(local)
        if st is None:
            send_error(client, 404, "Not Found", url_path)
            return
        if is_dir(st):
            # We don't list directories in file mode
            send_error(client, 403, "Forbidden", "Directory listing disabled")
            return
        send_file(client, local, segments[-1], head_only=head_only)
        return

    # ----- Directory mode -----
    local = join_path(root, segments)
    st = stat_or_none(local)
    if st is None:
        send_error(client, 404, "Not Found", url_path)
        return

    if is_dir(st):
        # If URL doesn't end with '/', redirect so relative links work right
        if not url_path.endswith("/") and "?" not in url_path:
            location = url_path + "/"
            body = "<html><body>Redirecting to <a href='%s'>%s</a></body></html>" % (
                location, location)
            send_response(client, 301, "Moved Permanently", body,
                          extra_headers=["Location: " + location],
                          head_only=head_only)
            return
        if head_only:
            # For HEAD on a directory, just send minimal headers.
            head = (
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Connection: close\r\n"
                "Server: MicroPython-webserver/1.0 (AmigaOS)\r\n"
                "\r\n"
            ).encode("ascii")
            send_all(client, head)
            return
        if not stream_listing(client, local, segments, root):
            return
        return

    # Regular file
    send_file(client, local, segments[-1], head_only=head_only)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def usage():
    print("Usage: webserver.py <port> <path>")
    print("  <path> may be a directory (browsable) or a single file.")


def main():
    argv = sys.argv[1:]
    if len(argv) != 2 or argv[0] in ("-h", "--help"):
        usage()
        return

    try:
        port = int(argv[0])
    except ValueError:
        print("Error: port must be an integer")
        return
    if port < 1 or port > 65535:
        print("Error: port out of range")
        return

    target = argv[1]
    st = stat_or_none(target)
    if st is None:
        print("Error: path not found: %s" % target)
        return

    if is_dir(st):
        mode = "dir"
        root = target
        single_file_name = None
        print("Mode: directory")
        print("Root: %s" % root)
    else:
        mode = "file"
        # Find parent directory and basename.
        # AmigaOS paths can use ':' (volume) or '/' (subdir) as separators.
        slash = target.rfind("/")
        colon = target.rfind(":")
        sep = max(slash, colon)
        if sep < 0:
            root = ""
            single_file_name = target
        elif target[sep] == ":":
            root = target[:sep + 1]   # keep the colon: "Work:"
            single_file_name = target[sep + 1:]
        else:
            root = target[:sep]
            single_file_name = target[sep + 1:]
        if not single_file_name:
            print("Error: cannot determine filename from %s" % target)
            return
        print("Mode: single file")
        print("Root: %s" % (root if root else "(current dir)"))
        print("File: %s" % single_file_name)

    # Create listening socket
    addr_info = socket.getaddrinfo("0.0.0.0", port)
    addr = addr_info[0][-1]
    srv = socket.socket()
    try:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except (OSError, AttributeError):
        pass  # not all ports support this
    srv.bind(addr)
    srv.listen(1)
    print("Listening on http://0.0.0.0:%d/" % port)
    print("Press Ctrl-C to stop.")

    try:
        while True:
            try:
                client, caddr = srv.accept()
            except OSError as e:
                # errno 4 = EINTR: accept() was interrupted by a signal
                # (typically Ctrl-C). Exit the loop cleanly.
                errno = e.args[0] if e.args else 0
                if errno == 4:
                    print("\nInterrupted.")
                    break
                print("accept() failed: %s" % e)
                continue
            try:
                handle_request(client, caddr, mode, root, single_file_name)
            except Exception as e:
                print("  ! handler error: %s" % e)
                try:
                    send_error(client, 500, "Internal Server Error", str(e))
                except Exception:
                    pass
            finally:
                try:
                    client.close()
                except Exception:
                    pass
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        try:
            srv.close()
        except Exception:
            pass


main()
