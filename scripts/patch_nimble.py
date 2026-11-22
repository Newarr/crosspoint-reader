"""Let the generated sdkconfig set NimBLE-Arduino's HCI event pool sizes."""

import re
from pathlib import Path


def patch_nimble(env):
    header = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "NimBLE-Arduino/src/nimconfig.h"
    if not header.is_file():
        return

    with header.open(encoding="utf-8", newline="") as source:
        original = source.read()
    newline = "\r\n" if "\r\n" in original else "\n"
    content = original.replace("\r\n", "\n")
    for name, default in (
        ("CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT", 30),
        ("CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT", 8),
    ):
        define = f"#define {name} {default}"
        guarded = f"#ifndef {name}\n{define}\n#endif"
        if guarded in content:
            continue
        content, count = re.subn(rf"^{re.escape(define)}$", guarded, content, flags=re.MULTILINE)
        if count != 1:
            raise RuntimeError(f"Unexpected NimBLE-Arduino definition of {name} in {header}")

    patched = content.replace("\n", newline)
    if patched != original:
        with header.open("w", encoding="utf-8", newline="") as output:
            output.write(patched)


try:
    Import("env")  # noqa: F821
except NameError:
    pass
else:
    patch_nimble(env)  # noqa: F821
