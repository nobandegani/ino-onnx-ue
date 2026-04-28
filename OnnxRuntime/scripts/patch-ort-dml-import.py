# patch-ort-dml-import.py
#
# Patch an ONNX Runtime DLL's import table so its static dependency on
# "DirectML.dll" becomes a dependency on "InoDml.dll" instead. Run once
# by setup-onnxruntime.ps1 after the Microsoft.ML.OnnxRuntime.DirectML
# NuGet is extracted, before we stage the DLL.
#
# WHY: UE 5.7 ships its own DirectML.dll under
# Engine/Binaries/Win64/DML/x64/ (and marketplace plugins like
# RuntimeMetaHumanLipSync LoadLibrary it early during editor startup).
# Windows caches DLLs by base name — so when our InoOnnxRuntime.dll
# later delay-loads "DirectML.dll" on the first DirectML function
# call, it gets UE's copy, which is a different build than what our
# ORT 1.24.3 was compiled against. That version skew causes
# MultiHeadAttention / Slice kernel-validation failures (E_INVALIDARG)
# and fp16 silent numerical corruption.
#
# Note: DirectML.dll is a DELAY-LOAD dependency of
# Microsoft.ML.OnnxRuntime.DirectML's onnxruntime.dll (confirmed via
# pefile parse — DIRECTORY_ENTRY_DELAY_IMPORT, not
# DIRECTORY_ENTRY_IMPORT). The delay-load descriptor has the same
# RVA-to-name-string layout as a regular import descriptor (just a
# different struct field name: szName vs Name), so the byte-level
# patching technique is identical.
#
# Renaming the file on disk alone isn't enough — the import table
# inside InoOnnxRuntime.dll still says "DirectML.dll". This script
# patches that table to say "InoDml.dll" instead. The Microsoft-
# signed signature on InoOnnxRuntime.dll becomes invalid after this
# modification (minor AV concern documented in the plugin README).
#
# The new name must be the same length or SHORTER than the original
# (including null terminator) so we can overwrite in-place without
# rewriting the PE file layout. "DirectML.dll\0" = 13 bytes;
# "InoDml.dll\0" = 11 bytes — fits with 2 bytes of null padding at
# the end.
#
# Usage:
#   python patch-ort-dml-import.py <input.dll> <output.dll>
#
# Dependencies:
#   pefile  (pip install pefile)  — pure-Python PE manipulation library

import argparse
import os
import sys

try:
    import pefile  # noqa: F401
except ImportError:
    sys.exit(
        "ERROR: pefile is not installed. Install with:\n"
        "  pip install pefile\n"
        "This script needs the pure-Python pefile library to parse "
        "and patch the ORT DLL's PE import table."
    )

# Byte constants — operate on raw bytes (not str) so we don't accidentally
# hit Python's UTF-8 encoding mid-patch. PE DLL names are ASCII by spec.
OLD_NAME = b"DirectML.dll"
NEW_NAME = b"InoDml.dll"


def patch(input_path: str, output_path: str) -> None:
    """Rename the DirectML.dll import in `input_path` to InoDml.dll and
    write to `output_path`. Raises SystemExit on any failure."""

    if not os.path.exists(input_path):
        sys.exit(f"ERROR: input DLL not found: {input_path}")

    # Ensure the new name + null fits in the original slot. Import table
    # name strings live inline in the PE, and moving them requires
    # rewriting the whole imports directory. Stick to same-or-shorter
    # names so in-place overwrite works.
    slot_size = len(OLD_NAME) + 1          # original + null terminator
    new_with_null = NEW_NAME + b"\x00"
    if len(new_with_null) > slot_size:
        sys.exit(
            f"ERROR: new name '{NEW_NAME.decode()}' is longer than original "
            f"'{OLD_NAME.decode()}' slot ({len(new_with_null)} > {slot_size} bytes). "
            f"Pick a shorter name or extend the script to handle PE layout "
            f"rewrites via lief."
        )

    pe = pefile.PE(input_path, fast_load=False)

    # pefile's __data__ may be bytes or bytearray depending on version —
    # force bytearray so we can assign into slices.
    data = bytearray(pe.__data__)

    # DirectML.dll is a DELAY-LOAD import in Microsoft.ML.OnnxRuntime.DirectML,
    # not a static import. We ALSO check static imports in case a future
    # ORT build changes the linkage type.
    #
    # Delay-load descriptor (IMAGE_DELAYLOAD_DESCRIPTOR) has an szName field
    # that is an RVA to the DLL-name string — same layout semantics as the
    # regular IMAGE_IMPORT_DESCRIPTOR::Name. pefile exposes both as an
    # .struct with a Name / szName attribute.

    def find_and_patch(entries, name_attr: str, section_label: str) -> bool:
        """Iterate `entries` looking for the target DLL; patch in place
        and return True if found, False otherwise."""
        for entry in entries:
            if entry.dll.lower() != OLD_NAME.lower():
                continue

            name_rva = getattr(entry.struct, name_attr)
            file_offset = pe.get_offset_from_rva(name_rva)

            # Sanity check: verify the bytes we're about to overwrite
            # actually say "DirectML.dll\0". Prevents re-patch / bad parse.
            original = bytes(data[file_offset:file_offset + slot_size])
            expected = OLD_NAME + b"\x00"
            if original != expected:
                sys.exit(
                    f"ERROR: expected {expected!r} at file offset 0x{file_offset:x} "
                    f"but found {original!r}. Input is not a fresh Microsoft.ML."
                    f"OnnxRuntime.DirectML DLL, or pefile parsed it wrong."
                )

            # Write new name + null + null padding to preserve slot length.
            padded = new_with_null + b"\x00" * (slot_size - len(new_with_null))
            data[file_offset:file_offset + slot_size] = padded

            print(
                f"Patched {section_label} '{OLD_NAME.decode()}' -> '{NEW_NAME.decode()}' "
                f"at file offset 0x{file_offset:x} ({slot_size} bytes, "
                f"{slot_size - len(new_with_null)} bytes of null padding)"
            )
            return True
        return False

    patched = False

    # Check delay-load imports first — that's where DirectML lives in
    # MS's DML-flavoured ORT build.
    if hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"):
        patched = find_and_patch(
            pe.DIRECTORY_ENTRY_DELAY_IMPORT, "szName", "delay-import")

    # Fall back to static imports for future-proofing in case MS changes
    # linkage.
    if not patched and hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        patched = find_and_patch(
            pe.DIRECTORY_ENTRY_IMPORT, "Name", "static-import")

    if not patched:
        sys.exit(
            f"ERROR: '{OLD_NAME.decode()}' was not found in {input_path}'s "
            f"import tables (neither static nor delay-load). Either the "
            f"input isn't the DML-flavoured ORT build, or pefile parsed "
            f"it in an unexpected way."
        )

    # Make sure the output dir exists so we can write there.
    output_dir = os.path.dirname(os.path.abspath(output_path))
    if output_dir and not os.path.isdir(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    # Write the mutated bytes directly — pefile.write() re-serialises the
    # parsed PE representation, which can drop modifications to raw data.
    # Going direct with a straight `f.write(data)` is both simpler and
    # guaranteed to preserve everything except the bytes we explicitly
    # changed.
    with open(output_path, "wb") as f:
        f.write(bytes(data))

    print(f"Wrote patched DLL to {output_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Patch an ORT DLL's import table to rename its DirectML.dll "
            "dependency to InoDml.dll, isolating our copy from UE's "
            "bundled Engine/Binaries/Win64/DML/x64/DirectML.dll."
        )
    )
    parser.add_argument("input", help="Source DLL (e.g. onnxruntime.dll from MS NuGet)")
    parser.add_argument("output", help="Destination DLL (patched, ready to stage)")
    args = parser.parse_args()

    patch(args.input, args.output)


if __name__ == "__main__":
    main()
