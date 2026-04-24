# Test suite for amiga.asl.file_request().
#
# Run under WinUAE or on real hardware:
#     micropython samples/test_asl.py
#
# Menu-driven: pick a scenario, interact with the requester, observe the
# returned value. Ctrl-C at the menu prompt exits cleanly. Note that, as
# with amiga.intuition, Ctrl-C pressed while an ASL requester has focus
# is NOT propagated -- asl.library does not surface SIGBREAKF_CTRL_C
# until the requester is dismissed.

import amiga.asl as asl


def run(label, **kw):
    print()
    print("--- {} ---".format(label))
    print("args:", kw)
    result = asl.file_request(**kw)
    if isinstance(result, list):
        print("result: list of {} item(s)".format(len(result)))
        for p in result:
            print("  ", repr(p))
    else:
        print("result:", repr(result))


def case_simple_open():
    run("1. simple open (title='Open file', initial_drawer='PROGDIR:')",
        title="Open file",
        initial_drawer="PROGDIR:")


def case_pattern_filter():
    run("2. pattern filter (PROGDIR:samples, #?.py)",
        title="Python files",
        initial_drawer="PROGDIR:samples",
        initial_pattern="#?.py")


def case_save_mode():
    run("3. save mode (initial_file='untitled.txt')",
        title="Save as",
        initial_file="untitled.txt",
        save_mode=True)


def case_multi_select():
    run("4. multi-select (pick several files)",
        title="Pick multiple files",
        multi_select=True)


def case_drawers_only():
    run("5. drawers only (pick a directory)",
        title="Select directory",
        drawers_only=True)


def case_cancel():
    print()
    print("--- 6. cancel probe -- click Cancel to check None is returned ---")
    result = asl.file_request(title="Click Cancel please")
    print("result:", repr(result))
    assert result is None, "expected None on cancel, got {!r}".format(result)
    print("   -> assertion passed")


def case_latin1():
    run("7. Latin-1 title 'Cafe Francais' with accents",
        title="Café Français",
        initial_drawer="PROGDIR:")


CASES = [
    ("simple open",      case_simple_open),
    ("pattern filter",   case_pattern_filter),
    ("save mode",        case_save_mode),
    ("multi-select",     case_multi_select),
    ("drawers only",     case_drawers_only),
    ("cancel probe",     case_cancel),
    ("Latin-1 accents",  case_latin1),
]


def menu():
    while True:
        print()
        print("amiga.asl.file_request() test menu")
        for i, (name, _) in enumerate(CASES, 1):
            print("  {}. {}".format(i, name))
        print("  q. quit")
        try:
            choice = input("choice> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if choice in ("q", "quit", "exit"):
            return
        if not choice.isdigit():
            print("?? unknown choice:", choice)
            continue
        n = int(choice)
        if not (1 <= n <= len(CASES)):
            print("?? out of range:", n)
            continue
        try:
            CASES[n - 1][1]()
        except KeyboardInterrupt:
            print()
            print("(KeyboardInterrupt during scenario)")


if __name__ == "__main__":
    menu()
    print("bye.")
