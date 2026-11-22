"""Apply the caller-owned scanline and short-read fixes to PNGdec 1.1.6."""

import argparse
import hashlib
from pathlib import Path


def replace(source, old, new, count=1):
    if source.count(old) != count:
        raise RuntimeError("PNGdec patch target does not match version 1.1.6")
    return source.replace(old, new)


def patch_header(source):
    source = replace(
        source,
        "    uint8_t ucPixels[PNG_MAX_BUFFERED_PIXELS];",
        "    uint8_t *ucPixels;\n    int iPixelsSize;",
    )
    return replace(
        source,
        "    void setBuffer(uint8_t *pBuffer);",
        "    void setBuffer(uint8_t *pBuffer);\n"
        "#define PNGDEC_CALLER_ROW_BUFFER 1\n"
        "    void setRowBuffer(uint8_t *pBuffer, int iSize);",
    )


def patch_wrapper(source):
    return replace(
        source,
        "int PNG::decode(void *pUser, int iOptions)",
        "void PNG::setRowBuffer(uint8_t *pBuffer, int iSize)\n"
        "{\n"
        "    _png.ucPixels = pBuffer;\n"
        "    _png.iPixelsSize = iSize;\n"
        "}\n\n"
        "int PNG::decode(void *pUser, int iOptions)",
    )


def patch_decoder(source):
    source = replace(
        source,
        "    // Use internal buffer to maintain the current and previous lines",
        "    const int rowBytes = (pPage->iPitch + 1) * 2 + 32;\n"
        "    if (pPage->iPitch <= 0 || pPage->ucPixels == NULL ||\n"
        "        pPage->iPixelsSize < rowBytes + ((iOptions & PNG_FAST_PALETTE) ? 512 : 0)) {\n"
        "        pPage->iError = PNG_NO_BUFFER;\n"
        "        return PNG_NO_BUFFER;\n"
        "    }",
    )
    source = replace(source, "sizeof(pPage->ucPixels)-512", "pPage->iPixelsSize-512", count=2)
    source = replace(
        source,
        "        pPage->iError = PNG_NO_BUFFER;\n        return 0;",
        "        pPage->iError = PNG_NO_BUFFER;\n        return PNG_NO_BUFFER;",
    )
    source = replace(
        source,
        "        pPage->ucBpp = s[24];",
        "        if (pPage->iWidth <= 0 || pPage->iWidth > 32767 || pPage->iHeight <= 0) {\n"
        "            pPage->iError = PNG_TOO_BIG;\n"
        "            return pPage->iError;\n"
        "        }\n"
        "        pPage->ucBpp = s[24];",
    )
    source = replace(
        source,
        "            case 0x504c5445: //'PLTE' palette colors\n"
        "                memset(&pPage->ucPalette[768], 0xff, 256); // assume all colors are opaque unless specified\n"
        "                memcpy(pPage->ucPalette, &s[iOffset], iLen);",
        "            case 0x504c5445: //'PLTE' palette colors\n"
        "                if (iLen > 768 || (iLen % 3) != 0) {\n"
        "                    pPage->iError = PNG_DECODE_ERROR;\n"
        "                    return 1;\n"
        "                }\n"
        "                if (iOffset + iLen > iBytesRead) {\n"
        "                    iFileOffset += (iOffset - iBytesRead);\n"
        "                    (*pPage->pfnSeek)(&pPage->PNGFile, iFileOffset);\n"
        "                    iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, s, PNG_FILE_BUF_SIZE);\n"
        "                    if (iBytesRead < iLen) {\n"
        "                        pPage->iError = PNG_DECODE_ERROR;\n"
        "                        return 1;\n"
        "                    }\n"
        "                    iFileOffset += iBytesRead;\n"
        "                    iOffset = 0;\n"
        "                }\n"
        "                memset(&pPage->ucPalette[768], 0xff, 256); // assume all colors are opaque unless specified\n"
        "                memcpy(pPage->ucPalette, &s[iOffset], iLen);",
    )
    source = replace(
        source,
        "            case 0x74524e53: //'tRNS' transparency info\n"
        "                if (pPage->ucPixelType == PNG_PIXEL_INDEXED) // if palette exists\n"
        "                {\n"
        "                    memcpy(&pPage->ucPalette[768], &s[iOffset], iLen);\n"
        "                    pPage->iHasAlpha = 1;\n"
        "                }",
        "            case 0x74524e53: //'tRNS' transparency info\n"
        "                if (pPage->ucPixelType == PNG_PIXEL_INDEXED || iLen == 2 || iLen == 6) {\n"
        "                    if (pPage->ucPixelType == PNG_PIXEL_INDEXED && iLen > 256) {\n"
        "                        pPage->iError = PNG_DECODE_ERROR;\n"
        "                        return 1;\n"
        "                    }\n"
        "                    if (iOffset + iLen > iBytesRead) {\n"
        "                        iFileOffset += (iOffset - iBytesRead);\n"
        "                        (*pPage->pfnSeek)(&pPage->PNGFile, iFileOffset);\n"
        "                        iBytesRead = (*pPage->pfnRead)(&pPage->PNGFile, s, PNG_FILE_BUF_SIZE);\n"
        "                        if (iBytesRead < iLen) {\n"
        "                            pPage->iError = PNG_DECODE_ERROR;\n"
        "                            return 1;\n"
        "                        }\n"
        "                        iFileOffset += iBytesRead;\n"
        "                        iOffset = 0;\n"
        "                    }\n"
        "                }\n"
        "                if (pPage->ucPixelType == PNG_PIXEL_INDEXED) // if palette exists\n"
        "                {\n"
        "                    memcpy(&pPage->ucPalette[768], &s[iOffset], iLen);\n"
        "                    pPage->iHasAlpha = 1;\n"
        "                }",
    )
    return replace(
        source,
        "                        iFileOffset += iBytesRead;\n"
        "                        iOffset = 0;\n"
        "                    } else {",
        "                        if (iBytesRead <= 0) {\n"
        "                            pPage->iError = PNG_DECODE_ERROR;\n"
        "                            iBytesRead = 0;\n"
        "                            iLen = 0;\n"
        "                            bDone = TRUE;\n"
        "                            break;\n"
        "                        }\n"
        "                        iFileOffset += iBytesRead;\n"
        "                        iOffset = 0;\n"
        "                    } else {",
    )


TARGETS = (
    ("PNGdec.h", patch_header,
     "f01b12dea5d14df02aa0ae7b9e24c415b7d40879221e90ab644d7215a3c2a37f",
     "b1d24cc345143b414c8f18f34f6fb0e57bcad1ee7b868da1f4372e1e0cb400a0"),
    ("PNGdec.cpp", patch_wrapper,
     "2b534fbac2684848104f5eecb21ebf99402efc47659c686ecaf79e3b604a9f5c",
     "d9deb1e586738384f2b996acb2fa80219670ba1ef3a5138ed8559ca59837e5c1"),
    ("png.inl", patch_decoder,
     "8008f43fb74e578df3651791da4fb4ac74cb87f946ec6910aeab703fa5b5ea65",
     "e4d4448c2ac3db721fb885bc7a55d73f4c61a53932e802f7ed05c320fb48fd27"),
)


def apply_patch(dependency_dir):
    dependency_dir = Path(dependency_dir).resolve()
    properties = dependency_dir / "library.properties"
    if properties.is_symlink() or "version=1.1.6" not in properties.read_text().splitlines():
        raise RuntimeError("PNGdec patch requires version 1.1.6")

    updates = {}
    for filename, transform, upstream, patched in TARGETS:
        path = dependency_dir / "src" / filename
        if path.resolve() != path:
            raise RuntimeError(f"PNGdec patch target is a symlink: {path}")
        contents = path.read_bytes()
        digest = hashlib.sha256(contents).hexdigest()
        if digest == patched:
            continue
        if digest != upstream:
            raise RuntimeError(f"Unrecognized PNGdec 1.1.6 source: {path}")
        result = transform(contents.decode()).encode()
        if hashlib.sha256(result).hexdigest() != patched:
            raise RuntimeError(f"Unexpected PNGdec patch output: {path}")
        updates[path] = result

    for path, result in updates.items():
        path.write_bytes(result)
    if updates:
        print("Patched PNGdec scanline storage and IDAT reads")


if "Import" in globals():
    Import("env")  # noqa: F821
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")).resolve() / env["PIOENV"]  # noqa: F821
    dependency = libdeps / "PNGdec"
    if dependency.is_dir():
        if dependency.resolve() != dependency:
            raise RuntimeError("PNGdec must be a local dependency")
        apply_patch(dependency)
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dependency_dir", type=Path)
    apply_patch(parser.parse_args().dependency_dir)
