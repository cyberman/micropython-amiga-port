# Test suite for amiga.intuition.auto_request().
#
# Run under WinUAE or on real hardware:
#     micropython samples/test_auto_request.py
#
# Each case pops a modal requester; click the requested button to advance.
# auto_request() returns True for the left (yes) button, False for the
# right (no) button.

import amiga.intuition as intuition


def case(n, label, **kw):
    print()
    print("--- case {}: {} ---".format(n, label))
    result = intuition.auto_request(**kw)
    print("   -> result", result)


# 1. Default Yes/No labels, click Yes -> expect True.
case(
    1, "default labels, click Yes",
    body="Click 'Yes' to continue. Expected result: True.",
)

# 2. Default Yes/No labels, click No -> expect False.
case(
    2, "default labels, click No",
    body="Click 'No' to continue. Expected result: False.",
)

# 3. Custom Spanish labels, exercises configurability + accents.
case(
    3, "Spanish labels (Sí / No)",
    body="Labels configurables avec accents.\nCliquez l'un ou l'autre.",
    yes="Sí", no="No",
)

# 4. Multi-line body via \n.
case(
    4, "multi-line body",
    body="Ligne 1\nLigne 2\nLigne 3 -- trois lignes au total.",
)

# 5. printf-injection guard: body contains %s, %d, %%.
case(
    5, "printf-injection guard",
    body="Literal specifiers: %s %d %x %n %% -- must render as-is.",
)

# 6. Ctrl-C probe. Intuition does not surface SIGBREAKF_CTRL_C while an
#    EasyRequest is open, so pressing Ctrl-C in the launching shell does
#    NOT abort this call -- the try/except is documentary.
print()
print("--- case 6: Ctrl-C probe (click a button; Ctrl-C not catchable) ---")
try:
    result = intuition.auto_request(
        body="Press Ctrl-C in the launching shell (nothing will happen).\n"
             "Then click Yes or No to continue.",
    )
    print("   -> clicked, result", result)
except KeyboardInterrupt:
    print("   -> KeyboardInterrupt caught (unexpected on current Intuition)")

print()
print("All cases done.")
