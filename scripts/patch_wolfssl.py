from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Arduino-wolfSSL turns on DEBUG_WOLFSSL, which compiles every WOLFSSL_MSG and
   WOLFSSL_ENTER trace string into flash. Keep it only for FREEINK_WOLFSSL_DEBUG. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
/* Ask servers for 2 KB TLS records (RFC 6066 max_fragment_length). SecureClient
   already calls wolfSSL_UseMaxFragment when this is defined, but
   Arduino-wolfSSL's user_settings.h never defines it, so the request was not
   being sent and every server used 16 KB records. Servers that do not support
   the extension ignore it. */
#ifndef HAVE_MAX_FRAGMENT
#define HAVE_MAX_FRAGMENT
#endif
"""


def patch_user_settings(path: Path) -> None:
    original_text = path.read_text()
    text = original_text
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    patched_text = text.rstrip() + OVERRIDES + "\n"
    if original_text == patched_text:
        return
    path.write_text(patched_text)
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


# wolfSSL grows its input buffer to fit each record larger than the static
# buffer, then frees it again as soon as the record is consumed. A server that
# ignores max_fragment_length sends 16 KB records, so a large download on a
# PSRAM-less C3 needs a fresh ~17 KB contiguous block for every record, and
# fails with MEMORY_E once fragmentation leaves none. Keep the grown buffer
# until the connection is freed: one allocation per connection, failing at the
# first record rather than megabytes into the transfer.
SHRINK_MARKER = "/* CrossInk: keep a grown input buffer until the connection is freed */"
SHRINK_ORIGINAL = """    if (!forcedFree && (usedLength > STATIC_BUFFER_LEN ||
            ssl->buffers.clearOutputBuffer.length > 0))
        return;
"""
# The source lives in PlatformIO's dependency cache. Keep the original branch
# available so reverting this script also disables retention in an already
# patched dependency, without requiring users to delete their package cache.
SHRINK_PATCHED = f"""    {SHRINK_MARKER}
#if defined(FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER)
    if (!forcedFree)
        return;
#else
{SHRINK_ORIGINAL}#endif
"""
env.Append(CPPDEFINES=["FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER"])


# A TLS client must buffer a whole record before decrypting it, and servers that
# ignore max_fragment_length send 16 KB records. On PSRAM-less C3 boards the
# ~16.7 KB contiguous block that needs is usually gone by the time a large OPDS
# page or book arrives, and the transfer fails with MEMORY_E (-125). Let the
# input buffer borrow the block src/network/TlsRecordReserve.cpp set aside before
# the heap fragmented, recording its full capacity so the buffer never has to
# grow again, and tell CrossInk when a server agrees to small records so the
# block can be returned. Without a reserved block the original allocation runs.
RESERVE_MARKER = "/* CrossInk: TLS record reserve */"
RESERVE_DECL_ORIGINAL = """int GrowInputBuffer(WOLFSSL* ssl, int size, int usedLength)
{
    byte* tmp;
"""
RESERVE_DECL_PATCHED = f"""{RESERVE_MARKER}
#if defined(FREEINK_TLS_RECORD_RESERVE)
void* FreeInkTlsRecordReserveTake(unsigned int want, unsigned int* capacity);
#endif
int GrowInputBuffer(WOLFSSL* ssl, int size, int usedLength)
{{
    byte* tmp;
    unsigned int freeinkCapacity = 0;
"""
RESERVE_ALLOC_ORIGINAL = """    tmp = (byte*)XMALLOC(size + usedLength + align,
                             ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
"""
RESERVE_ALLOC_PATCHED = """    tmp = NULL;
#if defined(FREEINK_TLS_RECORD_RESERVE)
    tmp = (byte*)FreeInkTlsRecordReserveTake((unsigned int)(size + usedLength + align), &freeinkCapacity);
#endif
    if (tmp == NULL) {
        freeinkCapacity = (unsigned int)(size + usedLength + align);
        tmp = (byte*)XMALLOC(size + usedLength + align,
                             ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
    }
"""
RESERVE_SIZE_ORIGINAL = """    ssl->buffers.inputBuffer.buffer = tmp;
    ssl->buffers.inputBuffer.bufferSize = size + usedLength;
"""
RESERVE_SIZE_PATCHED = """    ssl->buffers.inputBuffer.buffer = tmp;
    ssl->buffers.inputBuffer.bufferSize = freeinkCapacity - align;
"""

MFL_MARKER = "/* CrossInk: report accepted max_fragment_length */"
MFL_ORIGINAL = """    if (ssl->session != NULL) {
        ssl->session->mfl = *input;
    }
"""
MFL_PATCHED = f"""    if (ssl->session != NULL) {{
        ssl->session->mfl = *input;
    }}
    {MFL_MARKER}
#if defined(FREEINK_TLS_RECORD_RESERVE)
    if (!isRequest)
        FreeInkTlsRecordReserveSmallRecords(ssl->max_fragment);
#endif
"""
MFL_DECL_ORIGINAL = """static int TLSX_MFL_Parse(WOLFSSL* ssl, const byte* input, word16 length,
                          byte isRequest)
"""
MFL_DECL_PATCHED = """#if defined(FREEINK_TLS_RECORD_RESERVE)
void FreeInkTlsRecordReserveSmallRecords(unsigned int maxFragment);
#endif
static int TLSX_MFL_Parse(WOLFSSL* ssl, const byte* input, word16 length,
                          byte isRequest)
"""
env.Append(CPPDEFINES=["FREEINK_TLS_RECORD_RESERVE"])


def apply_exact(text: str, original: str, patched: str, path: Path, what: str) -> str:
    if text.count(original) != 1:
        raise RuntimeError(f"Unsupported wolfSSL {what} in {path}; refusing an unverified patch")
    return text.replace(original, patched, 1)


def patch_record_reserve(path: Path) -> None:
    text = path.read_text()
    if RESERVE_MARKER in text:
        if text.count(RESERVE_ALLOC_PATCHED) != 1 or text.count(RESERVE_SIZE_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL record reserve patch in {path}")
        return
    text = apply_exact(text, RESERVE_DECL_ORIGINAL, RESERVE_DECL_PATCHED, path, "GrowInputBuffer")
    text = apply_exact(text, RESERVE_ALLOC_ORIGINAL, RESERVE_ALLOC_PATCHED, path, "input buffer allocation")
    text = apply_exact(text, RESERVE_SIZE_ORIGINAL, RESERVE_SIZE_PATCHED, path, "input buffer size")
    path.write_text(text)
    print(f"Patched wolfSSL record reserve: {path.relative_to(PROJECT_DIR)}")


def patch_mfl_report(path: Path) -> None:
    text = path.read_text()
    if MFL_MARKER in text:
        if text.count(MFL_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL max_fragment_length patch in {path}")
        return
    text = apply_exact(text, MFL_DECL_ORIGINAL, MFL_DECL_PATCHED, path, "TLSX_MFL_Parse")
    text = apply_exact(text, MFL_ORIGINAL, MFL_PATCHED, path, "max_fragment_length parse")
    path.write_text(text)
    print(f"Patched wolfSSL max_fragment_length report: {path.relative_to(PROJECT_DIR)}")


def patch_input_buffer_shrink(path: Path) -> None:
    text = path.read_text()
    if SHRINK_MARKER in text:
        if text.count(SHRINK_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL input buffer patch in {path}")
        return
    if text.count(SHRINK_ORIGINAL) != 1:
        raise RuntimeError(f"Unsupported wolfSSL ShrinkInputBuffer in {path}; refusing an unverified patch")
    path.write_text(text.replace(SHRINK_ORIGINAL, SHRINK_PATCHED, 1))
    print(f"Patched wolfSSL input buffer: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

for internal in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/internal.c"):
    patch_input_buffer_shrink(internal)
    patch_record_reserve(internal)

for tls in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/tls.c"):
    patch_mfl_report(tls)
