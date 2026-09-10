# Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.
# patch-ort-android-soname.py
#
# Patch the SONAME (DT_SONAME) inside an ONNX Runtime .so for Android so
# it matches the renamed filename. Run once by setup-onnxruntime.ps1
# after the AAR is unpacked, before staging the .so into
# Source/ThirdParty/Android/<arch>/.
#
# WHY: Microsoft's Android ORT AAR ships libonnxruntime.so with
# DT_SONAME = "libonnxruntime.so". The setup script renames the FILE
# to libInoOnnxRuntime.so, but a file rename does not change the
# SONAME embedded in the ELF — and Android's dynamic linker dedupes
# loaded libraries by SONAME, NOT by filename. So if any other plugin
# in the same APK also ships a libonnxruntime.so (e.g. the
# RuntimeMetaHumanLipSync marketplace plugin at ORT 1.19.2), the
# linker's loaded-libraries cache aliases both files under the same
# SONAME, and dlopen("libInoOnnxRuntime.so") returns the older ORT's
# handle. Our OrtApi::GetApi(ORT_API_VERSION=24) then returns NULL
# because the older ORT doesn't implement API 24.
#
# Patching the SONAME here closes that hole — both filename AND
# SONAME become unique to our copy, so the marketplace plugin's
# libonnxruntime.so sits in a separate slot of the linker's cache
# and our calls only see our 1.24.3 vtable.
#
# The new SONAME must match what we rename the file to (currently
# "libInoOnnxRuntime.so") and is longer than the original
# "libonnxruntime.so" (20 chars > 17 chars), so we need a real ELF
# editor that can extend the dynamic string table — lief handles
# this. (A simple in-place byte overwrite, like the Windows DML
# patcher does, only works when the new name fits in the old slot.)
#
# Usage:
#   python patch-ort-android-soname.py <input.so> <output.so> <new-soname>
#
# Dependencies:
#   lief  (pip install lief)  — cross-platform ELF/PE/Mach-O editor

import argparse
import os
import sys

try:
    import lief  # noqa: F401
except ImportError:
    sys.exit(
        "ERROR: lief is not installed. Install with:\n"
        "  pip install lief\n"
        "This script needs lief to rewrite the ELF dynamic string table "
        "when changing the SONAME to a longer string."
    )


# Original SONAME shipped by Microsoft's onnxruntime-android AAR. Sanity-
# checked against the input before patching so we fail loudly if the AAR
# format changes.
EXPECTED_OLD_SONAME = "libonnxruntime.so"


def _find_dt_soname(elf):
    """Return the DT_SONAME dynamic entry, or None if not present."""
    for entry in elf.dynamic_entries:
        if entry.tag == lief.ELF.DynamicEntry.TAG.SONAME:
            return entry
    return None


def patch(input_path: str, output_path: str, new_soname: str) -> None:
    """Rewrite the DT_SONAME of `input_path` to `new_soname` and write
    the result to `output_path`. Raises SystemExit on any failure."""

    if not os.path.exists(input_path):
        sys.exit(f"ERROR: input .so not found: {input_path}")

    elf = lief.parse(input_path)
    if elf is None:
        sys.exit(f"ERROR: lief could not parse {input_path} as ELF.")

    soname_entry = _find_dt_soname(elf)
    if soname_entry is None:
        sys.exit(
            f"ERROR: no DT_SONAME entry in {input_path}. The Microsoft "
            f"onnxruntime-android AAR is expected to ship one — input "
            f"may be a different binary."
        )

    current_soname = soname_entry.name
    if current_soname == new_soname:
        # Idempotent — re-running the script after a successful patch
        # is a no-op rather than an error.
        print(
            f"SONAME already '{new_soname}' in {input_path}; nothing to do."
        )
        # Still copy the file to the output path so the caller's pipeline
        # doesn't break on the missing destination.
        if os.path.abspath(input_path) != os.path.abspath(output_path):
            with open(input_path, "rb") as src, open(output_path, "wb") as dst:
                dst.write(src.read())
        return

    if current_soname != EXPECTED_OLD_SONAME:
        sys.exit(
            f"ERROR: expected DT_SONAME '{EXPECTED_OLD_SONAME}' in "
            f"{input_path} but found '{current_soname}'. Either Microsoft "
            f"changed the AAR's SONAME convention or the input has already "
            f"been patched to something else."
        )

    soname_entry.name = new_soname

    # Make sure the output dir exists so we can write there.
    output_dir = os.path.dirname(os.path.abspath(output_path))
    if output_dir and not os.path.isdir(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    elf.write(output_path)
    print(
        f"Patched DT_SONAME '{current_soname}' -> '{new_soname}' in "
        f"{output_path}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Patch the SONAME of an ONNX Runtime Android .so so it "
            "matches the renamed filename, isolating our copy from any "
            "marketplace plugin that ships its own libonnxruntime.so."
        )
    )
    parser.add_argument(
        "input",
        help="Source .so (libonnxruntime.so as unpacked from MS AAR)")
    parser.add_argument(
        "output",
        help="Destination .so (patched, ready to stage)")
    parser.add_argument(
        "new_soname",
        help="New DT_SONAME string (e.g. libInoOnnxRuntime.so)")
    args = parser.parse_args()

    patch(args.input, args.output, args.new_soname)


if __name__ == "__main__":
    main()
