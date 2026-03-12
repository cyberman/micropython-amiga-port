# os.path for AmigaOS
# AmigaOS path conventions:
#   ":" separates volume from path (DH0:work/file)
#   "/" separates subdirectories
#   A path is absolute if it contains ":"
#   join("DH0:", "work") -> "DH0:work" (no slash after colon)

import os

sep = "/"


def isabs(path):
    return ":" in path


def join(*paths):
    result = ""
    for p in paths:
        if not p:
            continue
        if ":" in p:
            # Absolute path resets
            result = p
        elif not result:
            result = p
        elif result.endswith(":") or result.endswith("/"):
            result += p
        else:
            result += "/" + p
    return result


def split(path):
    # Split into (head, tail) where tail is the last component
    i = path.rfind("/")
    j = path.rfind(":")
    if i >= 0:
        return (path[: i + 1].rstrip("/") if i > j else path[: i + 1], path[i + 1 :])
    if j >= 0:
        return (path[: j + 1], path[j + 1 :])
    return ("", path)


def basename(path):
    return split(path)[1]


def dirname(path):
    return split(path)[0]


def splitext(path):
    name = basename(path)
    i = name.rfind(".")
    if i <= 0:
        return (path, "")
    ext = name[i:]
    return (path[: len(path) - len(ext)], ext)


def normpath(path):
    # Split volume prefix if present
    colon = path.find(":")
    if colon >= 0:
        prefix = path[: colon + 1]
        rest = path[colon + 1 :]
    else:
        prefix = ""
        rest = path

    parts = rest.split("/")
    result = []
    for p in parts:
        if p == "." or p == "":
            continue
        elif p == "..":
            if result:
                result.pop()
        else:
            result.append(p)

    joined = "/".join(result)
    if prefix:
        return prefix + joined
    if not joined:
        return ""
    return joined


def abspath(path):
    if isabs(path):
        return normpath(path)
    cwd = os.getcwd()
    if cwd.endswith(":") or cwd.endswith("/"):
        return normpath(cwd + path)
    return normpath(cwd + "/" + path)


def exists(path):
    return os._stat_type(path) != 0


def isdir(path):
    return os._stat_type(path) == 1


def isfile(path):
    return os._stat_type(path) == 2
