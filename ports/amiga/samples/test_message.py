# Test suite for amiga.intuition.message().
#
# Run under WinUAE or on real hardware:
#     micropython samples/test_message.py
#
# Each case pops a modal requester with a single button. Clicking advances
# to the next case. message() returns None (verified in case 6).

import amiga.intuition as intuition


def case(n, label, **kw):
    print()
    print("--- case {}: {} ---".format(n, label))
    result = intuition.message(**kw)
    print("   -> result", result)


# 1. Default button "OK".
case(
    1, "default button 'OK'",
    body="Default button label. Click OK to continue.",
)

# 2. Customised single button.
case(
    2, "customised button 'Continue'",
    body="Button label overridden.",
    button="Continue",
)

# 3. Spanish button + accented body. Source must be UTF-8 encoded.
case(
    3, "Spanish button + accents",
    body="¡Listo! Pulsa el botón para continuar.",
    button="Vale",
)

# 4. Multi-line body via \n.
case(
    4, "multi-line body",
    body="Ligne 1\nLigne 2\nLigne 3 -- trois lignes.",
)

# 5. Long body (>200 chars) to check rendering / wrapping by Intuition.
long_body = (
    "Ceci est un texte volontairement long pour verifier comment le "
    "requester se comporte lorsque le body depasse la largeur habituelle. "
    "Intuition applique son propre rendu multi-lignes et doit afficher ce "
    "contenu sans tronquer."
)
case(
    5, "long body (>200 chars)",
    body=long_body,
)

# 6. Return value check: message() must return None.
print()
print("--- case 6: return value is None ---")
r = intuition.message(body="Check that this call returns None.")
assert r is None, "message() must return None, got %r" % (r,)
print("   -> assertion passed, returned None")

print()
print("All cases done.")
