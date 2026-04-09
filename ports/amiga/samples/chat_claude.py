# chat_claude.py - Talk to Claude from your Amiga shell.
#
# Uses the Anthropic Messages API over HTTPS (via AmiSSL + urequests).
#
# Usage:
#   micropython chat_claude.py [-k API_KEY] [-m MODEL] [-s SYSTEM_PROMPT]
#
# Options:
#   -k KEY     Anthropic API key (default: env var ANTHROPIC_API_KEY)
#   -m MODEL   Model name (default: claude-haiku-4-5)
#   -s TEXT    Custom system prompt
#
# In-chat commands:
#   /quit                       exit
#   /clear                      reset the conversation history
#   /save <file>                save the conversation to a text file
#   /blocks                     list code blocks from the last reply
#   /save-code <N> [filename]   save code block N as a file
#   /tokens                     show cumulative token usage
#   /help                       list commands
#
# Author: written for Fabrice's MicroPython Amiga port (build 227+)

import sys
import os
import json

try:
    import urequests
except ImportError:
    print("Error: urequests module not available in this build.")
    sys.exit(1)

API_URL = "https://api.anthropic.com/v1/messages"
API_VERSION = "2023-06-01"
DEFAULT_MODEL = "claude-haiku-4-5"
DEFAULT_MAX_TOKENS = 1024

DEFAULT_SYSTEM = (
    "You are Claude, talking to a user from an AmigaOS shell via a "
    "text-only interface running MicroPython. Keep your replies concise "
    "and use plain text only - no markdown formatting (no bold, no "
    "headers, no bullet lists with asterisks). However, when the user "
    "asks for code, ALWAYS wrap it in standard triple-backtick code "
    "fences with the language name, like ```python ... ``` or ```c "
    "... ```, because the client detects these blocks and offers to "
    "save them as files. Wrap prose lines at about 75 characters when "
    "possible. The user is a retro-computing enthusiast running classic "
    "Amiga hardware or an emulator."
)


# Mapping from fence language tag to file extension for default filenames.
LANG_EXT = {
    "python": "py", "py": "py",
    "c": "c", "h": "h",
    "cpp": "cpp", "c++": "cpp", "cc": "cpp",
    "javascript": "js", "js": "js",
    "html": "html", "htm": "html",
    "css": "css",
    "json": "json",
    "xml": "xml",
    "markdown": "md", "md": "md",
    "bash": "sh", "sh": "sh", "shell": "sh",
    "rexx": "rexx", "arexx": "rexx",
    "asm": "s", "assembly": "s", "s": "s",
    "make": "mk", "makefile": "mk",
    "yaml": "yaml", "yml": "yaml",
    "sql": "sql",
    "lua": "lua",
    "text": "txt", "txt": "txt", "": "txt",
}


# ---------------------------------------------------------------------------
# Simple word-wrap (textwrap module is not available in MicroPython)
# ---------------------------------------------------------------------------
def wrap_text(text, width=78):
    out = []
    for paragraph in text.split("\n"):
        if not paragraph:
            out.append("")
            continue
        line = ""
        for word in paragraph.split(" "):
            if not line:
                line = word
            elif len(line) + 1 + len(word) <= width:
                line = line + " " + word
            else:
                out.append(line)
                line = word
        if line:
            out.append(line)
    return "\n".join(out)


def format_reply(text, width=75):
    """
    Format an assistant reply for display: word-wrap prose paragraphs,
    but leave code blocks (between ``` fences) untouched.
    """
    out = []
    in_code = False
    for line in text.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith("```"):
            # Toggle code mode and keep the fence line as-is
            out.append(line)
            in_code = not in_code
            continue
        if in_code:
            out.append(line)
        else:
            if line.strip() == "":
                out.append("")
            else:
                out.append(wrap_text(line, width))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Code block extraction
# ---------------------------------------------------------------------------
def extract_code_blocks(text):
    """
    Find ```lang ... ``` blocks in the text.
    Returns a list of dicts: [{"lang": str, "code": str}, ...].
    Tolerant: matches fences at the start of a line, with or without a
    language tag, and accepts the closing fence either at start of line
    or after some whitespace.
    """
    blocks = []
    n = len(text)
    i = 0
    while i < n:
        # Look for an opening fence at start of a line (or start of text)
        idx = text.find("```", i)
        if idx < 0:
            break
        # Verify it's at start of line
        if idx > 0 and text[idx - 1] != "\n":
            i = idx + 3
            continue

        # Read the language tag (rest of the line after ```)
        lang_end = text.find("\n", idx + 3)
        if lang_end < 0:
            break
        lang = text[idx + 3:lang_end].strip().lower()

        # Find closing fence
        body_start = lang_end + 1
        close = text.find("\n```", body_start - 1)
        if close < 0:
            # No closing fence: take the rest as code
            code = text[body_start:]
            blocks.append({"lang": lang, "code": code})
            break

        code = text[body_start:close + 1]  # include final newline before fence
        # Strip the trailing newline we included for cleanliness
        if code.endswith("\n"):
            code = code[:-1]
        blocks.append({"lang": lang, "code": code})

        # Move past the closing fence (may be followed by language tag noise)
        after = close + 4
        nl = text.find("\n", after)
        i = nl + 1 if nl >= 0 else n
    return blocks


def default_filename(index, lang):
    ext = LANG_EXT.get(lang, "txt")
    return "code%d.%s" % (index, ext)


# ---------------------------------------------------------------------------
# API call
# ---------------------------------------------------------------------------
def call_claude(api_key, model, system, history, max_tokens=DEFAULT_MAX_TOKENS):
    """
    Send the conversation history to the API and return (assistant_text, usage).
    Raises an exception with a readable message on failure.
    """
    payload = {
        "model": model,
        "max_tokens": max_tokens,
        "system": system,
        "messages": history,
    }
    headers = {
        "x-api-key": api_key,
        "anthropic-version": API_VERSION,
        "content-type": "application/json",
    }

    body = json.dumps(payload)
    try:
        resp = urequests.post(API_URL, data=body, headers=headers)
    except Exception as e:
        raise OSError("network error: %s" % e)

    try:
        if resp.status_code != 200:
            # Try to surface the API error message
            try:
                err = resp.json()
                msg = err.get("error", {}).get("message", "(no message)")
            except Exception:
                msg = resp.text[:200] if hasattr(resp, "text") else "(unparseable)"
            raise OSError("HTTP %d: %s" % (resp.status_code, msg))

        data = resp.json()
    finally:
        try:
            resp.close()
        except Exception:
            pass

    # Extract the assistant's text from the content blocks.
    # The API returns content as a list of blocks: [{"type":"text","text":"..."}]
    text_parts = []
    for block in data.get("content", []):
        if block.get("type") == "text":
            text_parts.append(block.get("text", ""))
    text = "".join(text_parts)

    usage = data.get("usage", {})
    return text, usage


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def save_conversation(path, history):
    try:
        f = open(path, "w")
    except OSError as e:
        return "Cannot open '%s' for writing: %s" % (path, e)
    try:
        for msg in history:
            role = msg["role"].upper()
            content = msg["content"]
            f.write("=== %s ===\n" % role)
            f.write(content)
            f.write("\n\n")
    finally:
        f.close()
    return "Saved %d messages to %s" % (len(history), path)


def save_code_block(path, code):
    try:
        f = open(path, "w")
    except OSError as e:
        return "Cannot open '%s' for writing: %s" % (path, e)
    try:
        f.write(code)
        if not code.endswith("\n"):
            f.write("\n")
    finally:
        f.close()
    return "Saved %d bytes to %s" % (len(code), path)


def print_help():
    print("Commands:")
    print("  /quit                       exit the chat")
    print("  /clear                      reset conversation history")
    print("  /save <file>                save the conversation to a text file")
    print("  /blocks                     list code blocks from the last reply")
    print("  /save-code <N> [filename]   save code block N to a file")
    print("  /tokens                     show cumulative token usage")
    print("  /help                       show this help")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def main():
    argv = sys.argv[1:]

    api_key = None
    model = DEFAULT_MODEL
    system = DEFAULT_SYSTEM

    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print("Usage: chat_claude.py [-k API_KEY] [-m MODEL] [-s SYSTEM_PROMPT]")
            return
        elif a == "-k":
            i += 1
            if i >= len(argv):
                print("Error: -k requires a value"); return
            api_key = argv[i]
        elif a == "-m":
            i += 1
            if i >= len(argv):
                print("Error: -m requires a value"); return
            model = argv[i]
        elif a == "-s":
            i += 1
            if i >= len(argv):
                print("Error: -s requires a value"); return
            system = argv[i]
        else:
            print("Error: unknown option %s" % a); return
        i += 1

    if api_key is None:
        try:
            api_key = os.getenv("ANTHROPIC_API_KEY")
        except Exception:
            api_key = None
    if not api_key:
        print("Error: no API key.")
        print("Set ANTHROPIC_API_KEY in the environment, or pass -k <key>.")
        print("On AmigaOS:  setenv ANTHROPIC_API_KEY sk-ant-...")
        return

    print("Claude chat for AmigaOS (model: %s)" % model)
    print("Type your message and press Enter. /help for commands, /quit to exit.")
    print()

    history = []
    total_in = 0
    total_out = 0
    last_blocks = []   # code blocks from the most recent assistant reply

    while True:
        try:
            user = input("You> ")
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print("\nInterrupted.")
            break

        user = user.strip()
        if not user:
            continue

        # Meta commands
        if user.startswith("/"):
            parts = user.split(None, 1)
            cmd = parts[0]
            arg = parts[1] if len(parts) > 1 else ""

            if cmd == "/quit":
                break
            elif cmd == "/clear":
                history = []
                print("(history cleared)")
                continue
            elif cmd == "/help":
                print_help()
                continue
            elif cmd == "/tokens":
                print("Tokens used: %d input, %d output (total %d)" % (
                    total_in, total_out, total_in + total_out))
                continue
            elif cmd == "/save":
                if not arg:
                    print("Usage: /save <filename>")
                    continue
                print(save_conversation(arg, history))
                continue
            elif cmd == "/blocks":
                if not last_blocks:
                    print("(no code blocks in the last reply)")
                    continue
                print("Code blocks from the last reply:")
                for n, b in enumerate(last_blocks, 1):
                    lines = b["code"].count("\n") + 1
                    lang = b["lang"] or "(no lang)"
                    print("  [%d] %s, %d lines  -> default: %s" % (
                        n, lang, lines, default_filename(n, b["lang"])))
                continue
            elif cmd == "/save-code":
                if not last_blocks:
                    print("(no code blocks to save)")
                    continue
                parts2 = arg.split(None, 1)
                if not parts2:
                    print("Usage: /save-code <N> [filename]")
                    continue
                try:
                    idx = int(parts2[0])
                except ValueError:
                    print("Error: block number must be an integer")
                    continue
                if idx < 1 or idx > len(last_blocks):
                    print("Error: block number out of range (1..%d)" % len(last_blocks))
                    continue
                block = last_blocks[idx - 1]
                if len(parts2) > 1 and parts2[1].strip():
                    filename = parts2[1].strip()
                else:
                    filename = default_filename(idx, block["lang"])
                print(save_code_block(filename, block["code"]))
                continue
            else:
                print("Unknown command. Type /help for the list.")
                continue

        # Send to Claude
        history.append({"role": "user", "content": user})
        print("(thinking...)")
        try:
            reply, usage = call_claude(api_key, model, system, history)
        except Exception as e:
            print("Error: %s" % e)
            # Pop the user message so the next attempt starts fresh
            history.pop()
            continue

        history.append({"role": "assistant", "content": reply})

        in_tok = usage.get("input_tokens", 0)
        out_tok = usage.get("output_tokens", 0)
        total_in += in_tok
        total_out += out_tok

        # Extract code blocks for the /save-code command
        last_blocks = extract_code_blocks(reply)

        print()
        print("Claude> " + format_reply(reply, 75))
        print()

        if last_blocks:
            n = len(last_blocks)
            if n == 1:
                print("[1 code block detected. Type /blocks to list, "
                      "/save-code 1 to save.]")
            else:
                print("[%d code blocks detected. Type /blocks to list, "
                      "/save-code N to save.]" % n)
            print()

    print("Goodbye.")


main()
