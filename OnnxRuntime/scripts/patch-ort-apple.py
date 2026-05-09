# patch-ort-apple.py
#
# Apple-platform Mach-O / framework patcher for the InoOnnx plugin.
#
# Two modes, selected via --mode:
#
#   --mode dylib
#       Patch a flat dylib's LC_ID_DYLIB install_name. Used on macOS to
#       turn Microsoft's `libonnxruntime.dylib` into our renamed
#       `libInoOnnxRuntime.dylib` with a unique @rpath install_name so
#       dyld can never alias us to a future Marketplace plugin's copy.
#
#   --mode framework
#       Rename a whole .framework end-to-end. Used on iOS to turn
#       Microsoft's `onnxruntime.framework` into our renamed
#       `InoOnnxRuntime.framework`. Steps:
#         1. Copy framework dir to output, renaming the framework dir
#            itself.
#         2. Rename the binary inside (e.g. `onnxruntime` -> `InoOnnxRuntime`).
#         3. Patch the binary's LC_ID_DYLIB to
#            `@rpath/<NewName>.framework/<NewName>` (works for both flat
#            iOS frameworks and any Versions/A/-style structures).
#         4. Patch Info.plist's CFBundleExecutable, CFBundleName, and
#            CFBundleIdentifier so the framework's recorded identity
#            matches the new on-disk layout (App Store signing rejects
#            a mismatch).
#
# WHY rename at all: Apple platforms don't have Windows' base-name DLL
# cache or Android's SONAME aliasing — dyld looks up by full @rpath +
# LC_ID_DYLIB install_name — so collision risk on macOS / iOS is much
# lower than on those two platforms. We still rename for
# defence-in-depth + consistency with the existing Win64 / Android
# rename pattern (`InoOnnxRuntime.dll` / `libInoOnnxRuntime.so`). If a
# future UE plugin ever ships its own ORT framework, our renamed copy
# is structurally distinguishable.
#
# Why lief and not install_name_tool: install_name_tool is a macOS-only
# Xcode tool; this setup runs on the Windows dev box (matching the
# existing setup-onnxruntime.ps1 host-stages-everything pattern). lief
# is pure Python, cross-platform, and we already require it for the
# Android SONAME patch (patch-ort-android-soname.py).
#
# Usage:
#   python patch-ort-apple.py --mode dylib \
#       --input <src.dylib> --output <dst.dylib> \
#       --install-name @rpath/libInoOnnxRuntime.dylib
#
#   python patch-ort-apple.py --mode framework \
#       --input <src.framework> --output <dst.framework> \
#       --new-name InoOnnxRuntime
#
# Dependencies:
#   lief    (pip install lief)        # Mach-O editor (also used for the
#                                       Android SONAME patch)
#   plistlib                          # stdlib; XML + binary plist support

import argparse
import os
import plistlib
import shutil
import sys

try:
    import lief  # noqa: F401
except ImportError:
    sys.exit(
        "ERROR: lief is not installed. Install with:\n"
        "  pip install lief\n"
        "This script needs lief to patch LC_ID_DYLIB across Mach-O fat "
        "and thin binaries (iOS simulator slice is fat: arm64 + x86_64)."
    )


# Microsoft's stock framework identifier — defensive sanity check before
# we rewrite. If this changes upstream, the patch will surface a clear
# mismatch error rather than silently corrupting the bundle.
EXPECTED_BUNDLE_ID_PREFIX = "com.microsoft.onnxruntime"


# ============================================================================
#  Mach-O LC_ID_DYLIB rewrite
# ============================================================================

# Magic bytes for the various Apple binary container formats we care about.
# Used by _detect_apple_binary_kind to distinguish a Mach-O dylib (which has
# an LC_ID_DYLIB to patch) from a Unix `ar` static archive (which doesn't —
# Microsoft ships their iOS xcframework as static archives wrapped in fat
# headers, and patching them is a no-op).
MACHO_MAGICS = {
    b"\xCA\xFE\xBA\xBE",  # FAT_MAGIC (32-bit offsets, big-endian)
    b"\xCA\xFE\xBA\xBF",  # FAT_MAGIC_64 (64-bit offsets, big-endian)
    b"\xFE\xED\xFA\xCE",  # MH_MAGIC (32-bit Mach-O, big-endian)
    b"\xCE\xFA\xED\xFE",  # MH_CIGAM (32-bit Mach-O, little-endian)
    b"\xFE\xED\xFA\xCF",  # MH_MAGIC_64 (64-bit Mach-O, big-endian)
    b"\xCF\xFA\xED\xFE",  # MH_CIGAM_64 (64-bit Mach-O, little-endian)
}
AR_MAGIC = b"!<arch>\n"  # Unix `ar` archive (sometimes wrapped in fat header)


def _detect_apple_binary_kind(path: str) -> str:
    """Sniff the first ~64 bytes of `path` and return one of:
       'macho'        — Mach-O dylib / executable / fat-with-Mach-O slices.
                        LC_ID_DYLIB patching applies.
       'static_ar'    — Unix `ar` archive (with or without an outer fat
                        header). LC_ID_DYLIB patching is a no-op (no
                        install_name on static archives); caller should
                        skip it. Microsoft ships iOS xcframework slices as
                        these — iOS static frameworks per Apple convention.
       'unknown'     — neither; caller should error out.
    """
    with open(path, "rb") as f:
        head = f.read(64)
    if len(head) < 8:
        return "unknown"

    magic = bytes(head[:4])
    # Direct Mach-O — easy case.
    if magic in MACHO_MAGICS - {b"\xCA\xFE\xBA\xBE", b"\xCA\xFE\xBA\xBF"}:
        return "macho"
    # Direct Unix `ar` archive (no fat wrapper). Rare for Apple frameworks
    # but possible.
    if head.startswith(AR_MAGIC):
        return "static_ar"

    # Fat header — peek inside the first slice to see if it's Mach-O or `ar`.
    # Fat header layout (big-endian):
    #   uint32 magic
    #   uint32 nfat_arch
    # then nfat_arch entries of:
    #   uint32 cputype, cpusubtype, offset, size, align
    # FAT_MAGIC_64 has a 64-bit offset/size variant. We only need the
    # offset of slice 0 to peek at its magic.
    if magic == b"\xCA\xFE\xBA\xBE":
        # 32-bit fat. Slice 0 offset at byte 16 (after 8-byte fat_header +
        # 8 bytes of cputype/cpusubtype).
        if len(head) < 24:
            return "unknown"
        slice_off = int.from_bytes(head[16:20], "big")
    elif magic == b"\xCA\xFE\xBA\xBF":
        # 64-bit fat. Slice 0 offset at byte 16 (after 8-byte fat_header +
        # 8 bytes cputype/cpusubtype) but offset is 8 bytes wide.
        if len(head) < 32:
            return "unknown"
        slice_off = int.from_bytes(head[16:24], "big")
    else:
        return "unknown"

    # Read 8 bytes at the slice offset to identify it.
    with open(path, "rb") as f:
        f.seek(slice_off)
        slice_head = f.read(8)
    if len(slice_head) < 8:
        return "unknown"
    if slice_head.startswith(AR_MAGIC):
        return "static_ar"
    if bytes(slice_head[:4]) in MACHO_MAGICS:
        return "macho"
    return "unknown"


def _patch_dylib_id(input_path: str, output_path: str, new_install_name: str) -> None:
    """Rewrite the LC_ID_DYLIB install_name of every slice in a Mach-O
    binary to `new_install_name`. Handles both single-arch (arm64 only)
    and fat (arm64 + x86_64) Mach-O files transparently.

    For static-archive frameworks (Apple's iOS convention — a fat-wrapped
    Unix `ar` archive instead of a real dylib), this function copies the
    file unchanged: there is no install_name on a `.a` to patch.
    Microsoft's ORT iOS xcframework ships this way; their osx-arm64 macOS
    NuGet entry is a real dylib and gets the patch.

    LC_LOAD_DYLIB references inside the binary are NOT touched — those
    point at OS / framework dependencies (libSystem, CoreML, Metal, etc.)
    and renaming them would break loading.
    """
    if not os.path.exists(input_path):
        sys.exit(f"ERROR: input Mach-O not found: {input_path}")

    kind = _detect_apple_binary_kind(input_path)
    if kind == "unknown":
        sys.exit(
            f"ERROR: {input_path} is neither a Mach-O nor a Unix `ar` "
            f"archive — first 4 bytes don't match any known magic."
        )

    if kind == "static_ar":
        # iOS static framework — nothing to patch in the binary itself.
        # The framework rename (dir + binary file + Info.plist) is what
        # makes the static framework act under our renamed identity at
        # link time; the static archive's internal `.o` symbol table has
        # no recorded "install_name" or framework-name reference.
        print(
            f"  static `ar` archive detected — skipping LC_ID_DYLIB patch "
            f"(iOS static framework convention)."
        )
        # Copy unchanged so the caller's pipeline doesn't break on a
        # missing output.
        if os.path.abspath(input_path) != os.path.abspath(output_path):
            output_dir = os.path.dirname(os.path.abspath(output_path))
            if output_dir and not os.path.isdir(output_dir):
                os.makedirs(output_dir, exist_ok=True)
            shutil.copyfile(input_path, output_path)
            print(f"  copied static archive to {output_path}")
        return

    parsed = lief.MachO.parse(input_path)
    if parsed is None:
        sys.exit(f"ERROR: lief could not parse {input_path} as Mach-O.")

    # lief.MachO.parse returns either a FatBinary (for fat / multi-arch
    # binaries) or a single Binary. Iterating works for both: FatBinary
    # is iterable across slices, Binary iterates as a single-element list.
    # We use `parsed.size` if available to detect the FatBinary case for
    # nicer logging, but the patch loop is uniform.
    slices = list(parsed) if hasattr(parsed, "__iter__") else [parsed]
    if not slices:
        sys.exit(f"ERROR: no Mach-O slices found in {input_path}.")

    print(
        f"  parsed Mach-O: {len(slices)} slice(s) "
        f"({'fat' if len(slices) > 1 else 'thin'})"
    )

    for i, sl in enumerate(slices):
        # Find the LC_ID_DYLIB load command. lief 0.13+ doesn't expose a
        # `binary.dylib_id` shortcut — we iterate `sl.commands` looking
        # for the entry whose `.command` enum is `ID_DYLIB`. (LC_LOAD_DYLIB
        # entries reference dependency dylibs and we don't touch those.)
        dylib_id = None
        for cmd in sl.commands:
            if cmd.command == lief.MachO.LoadCommand.TYPE.ID_DYLIB:
                dylib_id = cmd
                break

        if dylib_id is None:
            sys.exit(
                f"ERROR: slice {i} of {input_path} has no LC_ID_DYLIB "
                f"command — input may be an executable or bundle, not a "
                f"dylib."
            )

        old_name = dylib_id.name
        if old_name == new_install_name:
            print(
                f"  slice {i}: LC_ID_DYLIB already '{new_install_name}' — no-op"
            )
            continue

        dylib_id.name = new_install_name

        # CPU-type label for nicer fat-binary diagnostics. lief exposes
        # this on the Mach-O header; the attr path has shifted between
        # versions, so guard against AttributeError.
        cpu_label = "?"
        try:
            cpu_label = str(sl.header.cpu_type).rsplit(".", 1)[-1]
        except Exception:  # noqa: BLE001
            pass
        print(
            f"  slice {i} ({cpu_label}): LC_ID_DYLIB '{old_name}' "
            f"-> '{new_install_name}'"
        )

    # Make sure the output dir exists, then write. lief's write() takes a
    # path and serialises the (possibly fat) binary; works for both cases.
    output_dir = os.path.dirname(os.path.abspath(output_path))
    if output_dir and not os.path.isdir(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    parsed.write(output_path)

    # lief sometimes does not preserve the executable bit on the output
    # file (depends on lief version + host OS). Mach-O dylibs in
    # frameworks must be executable for dyld to map them; chmod here so
    # downstream cook/staging doesn't trip on a 0644 binary.
    try:
        os.chmod(output_path, 0o755)
    except OSError:
        # On Windows-hosted runs, chmod is a no-op for the executable
        # bit; the bit isn't meaningful on NTFS anyway and Mac/iOS file
        # systems will pick up sane defaults at copy time.
        pass

    print(f"  wrote patched Mach-O to {output_path}")


# ============================================================================
#  Info.plist rewrite (framework mode only)
# ============================================================================

def _patch_info_plist(plist_path: str, new_name: str) -> None:
    """Rewrite CFBundleExecutable / CFBundleName / CFBundleIdentifier
    in `plist_path` to match the renamed framework.

    Handles both XML and binary plists transparently — Microsoft ships
    XML in the iOS xcframework's Info.plists as of ORT 1.24.x, but
    plistlib works with both formats out of the box.
    """
    if not os.path.exists(plist_path):
        sys.exit(f"ERROR: Info.plist not found: {plist_path}")

    with open(plist_path, "rb") as f:
        data = plistlib.load(f)

    # Sanity: confirm the upstream identifier is what we expect, so a
    # future xcframework restructure surfaces as a clear error instead
    # of silent corruption.
    old_id = data.get("CFBundleIdentifier", "")
    if not old_id.startswith(EXPECTED_BUNDLE_ID_PREFIX):
        print(
            f"  WARNING: Info.plist CFBundleIdentifier='{old_id}' does not "
            f"start with '{EXPECTED_BUNDLE_ID_PREFIX}' — input may not be "
            f"the upstream Microsoft xcframework. Patching anyway.",
            file=sys.stderr,
        )

    new_bundle_id = f"io.inoland.{new_name}"

    print(
        f"  Info.plist: CFBundleExecutable={data.get('CFBundleExecutable')!r} -> {new_name!r}"
    )
    print(
        f"  Info.plist: CFBundleName={data.get('CFBundleName')!r} -> {new_name!r}"
    )
    print(
        f"  Info.plist: CFBundleIdentifier={old_id!r} -> {new_bundle_id!r}"
    )

    data["CFBundleExecutable"] = new_name
    data["CFBundleName"] = new_name
    data["CFBundleIdentifier"] = new_bundle_id

    # Write back in the same format we read. plistlib.dump preserves
    # XML vs binary format if FMT_XML is omitted and an existing-format
    # detection isn't built in — but we want to keep the upstream's XML
    # form regardless (Apple's codesign accepts both, XML is grep-friendly).
    with open(plist_path, "wb") as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_XML)


# ============================================================================
#  Mode handlers
# ============================================================================

def run_dylib(input_path: str, output_path: str, install_name: str) -> None:
    """Mode: patch a flat dylib's LC_ID_DYLIB."""
    print(
        f"=== dylib mode: {input_path} -> {output_path} (install_name={install_name})"
    )
    _patch_dylib_id(input_path, output_path, install_name)


def run_framework(input_dir: str, output_dir: str, new_name: str) -> None:
    """Mode: copy framework dir, rename binary, patch LC_ID_DYLIB,
    patch Info.plist."""
    if not os.path.isdir(input_dir):
        sys.exit(f"ERROR: input framework not found (or not a directory): {input_dir}")

    # Locate the original binary inside the input framework. iOS
    # frameworks are flat (binary lives at the root of the .framework),
    # but Versioned macOS frameworks have it under Versions/A/. We try
    # the flat layout first because Microsoft's iOS xcframework slice is
    # already flat.
    candidate_root = os.listdir(input_dir)
    binary_candidates = [
        n for n in candidate_root
        if (os.path.isfile(os.path.join(input_dir, n))
            and not n.startswith(".")
            and not n.endswith(".plist")
            and n != "Headers"
            and n != "Modules"
            and n != "PrivateHeaders"
            and n != "Resources"
            and n != "Versions"
            and "_CodeSignature" not in n)
    ]
    if not binary_candidates:
        sys.exit(
            f"ERROR: could not locate framework binary inside {input_dir}. "
            f"Top-level entries: {candidate_root!r}"
        )
    if len(binary_candidates) > 1:
        sys.exit(
            f"ERROR: multiple binary candidates inside {input_dir}: "
            f"{binary_candidates!r}. Expected exactly one."
        )
    old_binary_name = binary_candidates[0]
    print(f"=== framework mode: {input_dir} -> {output_dir}")
    print(f"  detected binary inside framework: {old_binary_name!r}")

    # Wipe any previous staging at the output path so removed-upstream
    # files don't linger. The PowerShell script also does this; doing it
    # here makes the script safely runnable standalone too.
    if os.path.exists(output_dir):
        shutil.rmtree(output_dir)

    # Copy the framework tree to the output. shutil.copytree preserves
    # file modes (which matters for the binary's exec bit) and
    # propagates symlinks if any (none expected in iOS frameworks; macOS
    # versioned frameworks are handled by the PowerShell flatten step
    # before this script runs).
    shutil.copytree(input_dir, output_dir, symlinks=True)

    # Rename the binary inside the copied tree. Windows shutil.move
    # handles the case-only-rename correctly when the target name is in
    # a different casing.
    src_binary = os.path.join(output_dir, old_binary_name)
    dst_binary = os.path.join(output_dir, new_name)
    if os.path.abspath(src_binary) != os.path.abspath(dst_binary):
        # Use os.replace for cross-platform atomic rename. On Windows
        # this fails if dst already exists, so guard.
        if os.path.exists(dst_binary):
            os.remove(dst_binary)
        os.replace(src_binary, dst_binary)
        print(f"  renamed binary: {old_binary_name!r} -> {new_name!r}")

    # Patch LC_ID_DYLIB. Frameworks reference themselves via
    # @rpath/<FwName>.framework/<BinaryName> at link time — that's the
    # convention dyld expects when the framework is embedded under
    # @executable_path/Frameworks/.
    install_name = f"@rpath/{new_name}.framework/{new_name}"
    _patch_dylib_id(dst_binary, dst_binary, install_name)

    # Patch Info.plist. Search common locations: framework root (iOS
    # flat layout), Resources/ (older macOS layout). The framework was
    # already flattened upstream/by setup script before reaching here,
    # so root is the expected location.
    plist_candidates = [
        os.path.join(output_dir, "Info.plist"),
        os.path.join(output_dir, "Resources", "Info.plist"),
    ]
    plist_path = next((p for p in plist_candidates if os.path.exists(p)), None)
    if plist_path is None:
        sys.exit(
            f"ERROR: Info.plist not found inside {output_dir}. Tried: "
            f"{plist_candidates!r}"
        )
    print(f"  Info.plist: {plist_path}")
    _patch_info_plist(plist_path, new_name)


# ============================================================================
#  CLI
# ============================================================================

def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Apple-platform Mach-O / framework patcher for InoOnnx. Renames "
            "Microsoft's libonnxruntime.dylib / onnxruntime.framework to our "
            "Ino-prefixed identity for cache-collision isolation."
        )
    )
    parser.add_argument(
        "--mode",
        choices=("dylib", "framework"),
        required=True,
        help="dylib = patch a flat .dylib's LC_ID_DYLIB. "
             "framework = rename a .framework end-to-end.",
    )
    parser.add_argument("--input", required=True, help="Source dylib or .framework dir")
    parser.add_argument("--output", required=True, help="Destination dylib or .framework dir")
    parser.add_argument(
        "--install-name",
        help="(dylib mode) New LC_ID_DYLIB install_name, "
             "e.g. '@rpath/libInoOnnxRuntime.dylib'",
    )
    parser.add_argument(
        "--new-name",
        help="(framework mode) New framework name (without .framework suffix), "
             "e.g. 'InoOnnxRuntime'",
    )
    args = parser.parse_args()

    if args.mode == "dylib":
        if not args.install_name:
            sys.exit("ERROR: --install-name is required in dylib mode.")
        run_dylib(args.input, args.output, args.install_name)
    elif args.mode == "framework":
        if not args.new_name:
            sys.exit("ERROR: --new-name is required in framework mode.")
        run_framework(args.input, args.output, args.new_name)


if __name__ == "__main__":
    main()
