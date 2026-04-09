## Sample Scripts

A set of example scripts is provided in `ports/amiga/samples/` to demonstrate
what MicroPython on AmigaOS can do. They are designed both as ready-to-use
tools and as educational material showing how to combine the available modules.

### wget.py — HTTP/HTTPS file downloader

A minimal `wget` clone that downloads a file from an HTTP or HTTPS URL with a
text progress bar (percentage, speed, ETA). It follows redirects up to 5 levels
and uses AmiSSL transparently for HTTPS.

```
micropython samples/wget.py http://aminet.net/dev/lang/micropython.lha
micropython samples/wget.py https://micropython.org/ -O index.html
```

Demonstrates: `socket`, `ssl`, low-level HTTP/1.1 protocol, buffered stream
reading over BSD sockets. Runs fine with the default 128 KB heap for HTTP.
HTTPS uses more memory due to the TLS handshake — if you hit memory errors,
relaunch with `-m 256` or higher.

### webserver.py — Single-threaded HTTP file server

A minimal web server that serves either a directory (with a clickable HTML
index, like `ls -la` in your browser) or a single file (with sibling files
reachable via relative links). Path traversal (`..`) is rejected, directory
listings are streamed to keep memory usage low, and common MIME types are
recognized so HTML/text/images render inline while binaries trigger a download.

```
micropython -m 4096 samples/webserver.py 8080 DH0:Work/site
micropython -m 4096 samples/webserver.py 8080 DH0:doc/manual.html
```

Then open `http://<your-amiga-ip>:8080/` from any browser on the network.

Demonstrates: `socket` server-side, HTTP request parsing, streaming responses,
URL encoding/decoding, MIME type detection, AmigaOS path handling.
**A larger heap is required** (`-m 4096` recommended) because each request
allocates buffers and the GC needs headroom to keep up with concurrent
browser connections.

### launch_ibrowse.py — Drive IBrowse via ARexx

Launches IBrowse (if not already running), waits for its ARexx port to become
available, then asks it to open the Aminet MicroPython page. A small but
complete demonstration of inter-process scripting on AmigaOS.

```
micropython samples/launch_ibrowse.py Work:IBrowse/IBrowse
```

Demonstrates: the `arexx` module (`arexx.exists()`, `arexx.Port()`, `.send()`),
launching external programs with `os.system()` and `Run >NIL:`, polling
patterns. Runs fine with the default heap — no `-m` needed.

### diff_view.py — Side-by-side colored diff viewer

A WinMerge-style diff viewer that displays two text files side-by-side with
ANSI color highlighting (yellow = modified, red = deleted, green = added),
line numbers on both sides, and a paginated reader. Implements the classic
LCS (Longest Common Subsequence) algorithm via dynamic programming.

```
micropython -m 4096 samples/diff_view.py file1.txt file2.txt
micropython -m 4096 samples/diff_view.py -w 132 file1.py file2.py
micropython -m 4096 samples/diff_view.py -w 160 -l 30 a.txt b.txt
```

Navigation inside the viewer: `Enter` for next page, `b`+`Enter` to go back,
`q`+`Enter` to quit. Use `-w` to set the terminal width (default 80) and
`-l` to set the number of diff rows per page (default 18).

Demonstrates: dynamic programming, ANSI escape codes on the AmigaOS shell,
text layout, interactive paging.
**A larger heap is required** (`-m 4096` strongly recommended) because the
LCS table grows as N×M where N and M are the line counts of the two files.
For files of a few hundred lines this comfortably fits; for very large files
you may need `-m 8192` or more.

### chat_claude.py — Talk to Claude from your Amiga shell

An interactive chat client for Anthropic's Claude AI, running directly in
your AmigaOS shell. Sends your messages to the Anthropic Messages API over
HTTPS (via AmiSSL and `urequests`) and displays Claude's replies with proper
word-wrapping. Maintains the conversation history across turns, and
automatically detects code blocks in Claude's responses so you can save them
as files with a single command — perfect for asking Claude to write Amiga
scripts and getting them straight onto your disk.

```
setenv ANTHROPIC_API_KEY sk-ant-...
micropython -m 512 samples/chat_claude.py
micropython -m 512 samples/chat_claude.py -m claude-sonnet-4-6
micropython -m 512 samples/chat_claude.py -k sk-ant-... -s "You are a helpful AmigaOS expert."
```

Options: `-k` to pass the API key inline (overrides the environment variable),
`-m` to choose a model (default `claude-haiku-4-5`, fastest and cheapest),
`-s` to provide a custom system prompt.

In-chat commands:
- `/quit` — exit the chat
- `/clear` — reset conversation history
- `/save <file>` — save the full conversation to a text file
- `/blocks` — list code blocks from Claude's last reply
- `/save-code <N> [filename]` — save code block N as a file (default name
  based on the language: `code1.py`, `code2.c`, etc.)
- `/tokens` — show cumulative token usage
- `/help` — list commands

You will need an Anthropic API key, which you can create for free at
[console.anthropic.com](https://console.anthropic.com/). The key starts with
`sk-ant-...` and is set via the `ANTHROPIC_API_KEY` environment variable or
the `-k` option. API usage is billed per token directly to your Anthropic
account; with the default Haiku model, casual conversation costs fractions
of a cent per exchange.

Demonstrates: `urequests` HTTPS POST with JSON payload, the Anthropic Messages
API protocol, multi-turn conversation state management, parsing fenced code
blocks, environment variable access via `os.getenv()`. A modest heap bump
(`-m 512`) is recommended because the conversation history grows with each
turn and the JSON payload is sent in full on every request.