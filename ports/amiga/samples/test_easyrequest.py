# Test suite for amiga.intuition.easy_request().
#
# Run under WinUAE or on real hardware:
#     micropython samples/test_easyrequest.py
#
# Each case pops a modal requester; click a button to advance to the next
# case. Case 7 is the Ctrl-C test -- press Ctrl-C while the requester is
# open instead of clicking a button.

import amiga.intuition as intuition


def case(n, label, **kw):
    print()
    print("--- case {}: {} ---".format(n, label))
    idx = intuition.easy_request(**kw)
    print("   -> index", idx)


# 1. Two plain buttons, left-to-right indexing.
case(
    1, "two buttons",
    title="Case 1",
    body="Two buttons -- click either.",
    buttons=["Left", "Right"],
)

# 2. Four buttons, confirm left=0 ... right=3.
case(
    2, "four buttons",
    title="Case 2",
    body="Four buttons -- click to verify index order (0..3, left to right).",
    buttons=["Zero", "One", "Two", "Three"],
)

# 3. Multi-line body via \n.
case(
    3, "multi-line body",
    title="Case 3",
    body="Line 1\nLine 2\nLine 3 -- three lines total.",
    buttons=["OK"],
)

# 4. printf-injection guard: body contains %s, %d, %n.
#    If the implementation naively passed body as es_TextFormat, the
#    requester would crash or print garbage. It must render the literal
#    characters instead.
case(
    4, "printf-injection guard",
    title="Case 4",
    body="Literal specifiers: %s %d %x %n %% -- must render as-is.",
    buttons=["OK"],
)

# 5. One-button case -- EasyRequest forces "OK" internally but our API
#    must still return 0 (the index of the single logical button).
case(
    5, "single button",
    title="Case 5",
    body="Single button. The API must return 0.",
    buttons=["Continue"],
)

# 6. Latin-1 round-trip: accented characters in title, body and labels.
case(
    6, "Latin-1 accents",
    title="Été à la mer -- éèàç",
    body="Accented body:\nà, â, é, è, ê, ë, î, ï, ô, ù, û, ü, ç.\n"
         "Title, body and labels should all render correctly.",
    buttons=["Validé", "Annulé", "Éteindre"],
)

# 7. Ctrl-C handling. Press Ctrl-C while the requester is open instead
#    of clicking; the call should raise KeyboardInterrupt cleanly.
print()
print("--- case 7: Ctrl-C -- press Ctrl-C in the requester window ---")
try:
    idx = intuition.easy_request(
        title="Case 7",
        body="Press Ctrl-C in this window (do not click).\n"
             "The call should raise KeyboardInterrupt.",
        buttons=["I clicked instead"],
    )
    print("   -> clicked, index", idx)
except KeyboardInterrupt:
    print("   -> KeyboardInterrupt caught (Ctrl-C worked as expected)")

print()
print("All cases done.")
