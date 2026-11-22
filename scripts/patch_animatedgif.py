"""
Apply the pinned AnimatedGIF width, scanline, palette, and cancellation fixes.
"""

import argparse
import os
import re
import sys


OLD_SNIPPET = """#define MAX_COLORS 256
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
"""

NEW_SNIPPET = """#define MAX_COLORS 256
#ifndef MAX_WIDTH
#ifdef __LINUX__
#define MAX_WIDTH 2048
#else
#define MAX_WIDTH 480
#endif // __LINUX__
#endif // MAX_WIDTH
"""

MAX_WIDTH_DEFINE_RE = re.compile(r"^\s*#\s*define\s+MAX_WIDTH\b", re.MULTILINE)
MAX_WIDTH_GUARD_RE = re.compile(
    r"^\s*#\s*(?:ifndef\s+MAX_WIDTH\b|if\s+!\s*defined\s*\(?\s*MAX_WIDTH\s*\)?)",
    re.MULTILINE,
)

PALETTE_OLD = """            // Read enough additional data for the color table
            i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            iBytesRead += i;
            if (iColorTableBits != 1 && i < 3*(1<<iColorTableBits)) { // file too small
                pPage->iError = GIF_BAD_FILE;
                return 0;
            }
"""

PALETTE_NEW = """            // The initial read may already contain the complete color table.
            // Keep the existing read-ahead for the following block parser.
            i = (*pPage->pfnRead)(&pPage->GIFFile, &pPage->ucFileBuf[iBytesRead], 3*(1<<iColorTableBits));
            iBytesRead += i;
            if (iBytesRead < iOffset + 3*(1<<iColorTableBits)) {
                pPage->iError = GIF_BAD_FILE;
                return 0;
            }
"""

COPY_OLD = """            while (buf < pEnd) {
#ifdef ALLOWS_UNALIGNED
// This is a significant perf improvement compared to copying 1 byte at a time
// even though it will often copy too many bytes. Since we're not at the end of
// the line, it's okay to copy a few extra pixels.
                BIGUINT tmp = *(BIGUINT *) s;
                s += sizeof(BIGUINT);
                *(BIGUINT *)buf = tmp;
                buf += sizeof(BIGUINT);
#else
                *buf++ = *s++;
#endif
            }
"""

COPY_NEW = """#ifdef ALLOWS_UNALIGNED
            while (pEnd - buf >= (int)sizeof(BIGUINT)) {
                BIGUINT tmp = *(BIGUINT *) s;
                s += sizeof(BIGUINT);
                *(BIGUINT *)buf = tmp;
                buf += sizeof(BIGUINT);
            }
#endif
            while (buf < pEnd) {
                *buf++ = *s++;
            }
"""

DRAW_END = "} GIFDRAW;"
DRAW_END_WITH_ABORT = "    uint8_t ucAbort;\n" + DRAW_END

DRAW_HELPER = """static int GIFDrawLine(GIFIMAGE *pPage, GIFDRAW *pDraw)
{
    pDraw->ucAbort = 0;
    (*pPage->pfnDraw)(pDraw);
    if (pDraw->ucAbort) {
        pPage->iError = GIF_DECODE_ERROR;
        return 1;
    }
    return 0;
}

"""

def _patch_header(header_path):
    with open(header_path, "r", encoding="utf-8") as file:
        text = file.read()

    max_width_define = MAX_WIDTH_DEFINE_RE.search(text)
    max_width_guard = MAX_WIDTH_GUARD_RE.search(text)
    if NEW_SNIPPET in text or (
        max_width_define
        and max_width_guard
        and max_width_guard.start() < max_width_define.start()
    ):
        patched = text
    elif OLD_SNIPPET in text:
        patched = text.replace(OLD_SNIPPET, NEW_SNIPPET, 1)
    else:
        if not max_width_define:
            sys.stderr.write(
                "WARNING: AnimatedGIF no longer defines MAX_WIDTH in %s\n"
                % header_path
            )
            patched = text
        else:
            raise RuntimeError("AnimatedGIF has an unguarded MAX_WIDTH definition in %s" % header_path)

    if DRAW_END_WITH_ABORT not in patched:
        if patched.count(DRAW_END) != 1:
            raise RuntimeError("AnimatedGIF draw structure patch does not match %s" % header_path)
        patched = patched.replace(DRAW_END, DRAW_END_WITH_ABORT, 1)

    if patched != text:
        with open(header_path, "w", encoding="utf-8") as file:
            file.write(patched)
        print("Patched AnimatedGIF header")

    return True


def _patch_decoder(source_path):
    with open(source_path, "r", encoding="utf-8") as file:
        original = file.read()
    text = original
    for name, before, after in (
        ("global palette", PALETTE_OLD, PALETTE_NEW),
        ("bounded scanline copy", COPY_OLD, COPY_NEW),
        (
            "draw callback helper",
            "//\n// DecodeLZWTurbo\n",
            DRAW_HELPER + "//\n// DecodeLZWTurbo\n",
        ),
        (
            "turbo draw abort",
            "            (*pImage->pfnDraw)(&gd); // callback to handle this line",
            "            if (GIFDrawLine(pImage, &gd)) return 1;",
        ),
        (
            "scanline declaration",
            "static void GIFMakePels(GIFIMAGE *pPage, unsigned int code);",
            "static int GIFMakePels(GIFIMAGE *pPage, unsigned int code);",
        ),
        (
            "scanline definition",
            "static void GIFMakePels(GIFIMAGE *pPage, unsigned int code)\n{",
            "static int GIFMakePels(GIFIMAGE *pPage, unsigned int code)\n{",
        ),
        (
            "scanline overflow return",
            "            return; /* Exit with error */\n        }\n        *(--s) = gifpels[code];",
            "            return 0;\n        }\n        *(--s) = gifpels[code];",
        ),
        (
            "partial scanline return",
            "            return;\n        }\n        else  /* Pixels cross into next line */",
            "            return 0;\n        }\n        else  /* Pixels cross into next line */",
        ),
        (
            "scanline draw abort",
            "            if (pPage->pfnDraw) {\n"
            "                (*pPage->pfnDraw)(&gd); // callback to handle this line\n"
            "            }",
            "            if (pPage->pfnDraw && GIFDrawLine(pPage, &gd)) return 1;",
        ),
        (
            "scanline completion",
            "    return;\n} /* GIFMakePels() */",
            "    return 0;\n} /* GIFMakePels() */",
        ),
        (
            "first LZW code abort",
            "    GIFMakePels(pImage, code); // first code is output as the first pixel",
            "    if (GIFMakePels(pImage, code)) return 1;",
        ),
        (
            "LZW loop abort",
            "            GIFMakePels(pImage, code);",
            "            if (GIFMakePels(pImage, code)) return 1;",
        ),
    ):
        if after in text:
            continue
        if text.count(before) != 1:
            raise RuntimeError("AnimatedGIF %s patch does not match %s" % (name, source_path))
        text = text.replace(before, after, 1)
    if text != original:
        with open(source_path, "w", encoding="utf-8") as file:
            file.write(text)
        print("Patched AnimatedGIF palette, scanline bounds, and cancellation")
    return True


def patch_library(library_path):
    return _patch_header(os.path.join(library_path, "src", "AnimatedGIF.h")) and _patch_decoder(
        os.path.join(library_path, "src", "gif.inl")
    )


def patch_animatedgif(env, require_found=False):
    library_path = os.path.join(
        env.subst("$PROJECT_LIBDEPS_DIR"),
        env.subst("$PIOENV"),
        "AnimatedGIF",
    )
    patched_any = os.path.isdir(library_path) and patch_library(library_path)

    if require_found and not patched_any:
        sys.stderr.write("ERROR: AnimatedGIF dependency was not found to patch\n")
        raise SystemExit(1)

    return patched_any


def patch_animatedgif_before_link(source, target, env):
    patch_animatedgif(env, require_found=True)


if "Import" in globals():
    Import("env")  # noqa: F821 (SCons-injected global)
    patch_animatedgif(env, require_found=False)  # noqa: F821
    env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", patch_animatedgif_before_link)  # noqa: F821
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library_path")
    patch_library(parser.parse_args().library_path)
