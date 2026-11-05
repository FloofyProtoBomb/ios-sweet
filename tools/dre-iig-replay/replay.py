#!/usr/bin/env python3
"""Replay the authenticated XNU 8796 DriverKit generator outputs."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import posixpath
import re
import sys
import tarfile
import tempfile
import zipfile


PROFILE = Path(__file__).with_name("profile.json")
DEFINITIONS = "xnu/iokit/DriverKit"
SEARCH_ROOTS = ("xnu/iokit", "xnu/osfmk", "xnu/bsd", "build/xnu.obj/bsd",
                "xnu/EXTERNAL_HEADERS")
ANCHORS = (
    "xnu/config/MasterVersion",
    f"{DEFINITIONS}/Makefile",
    f"{DEFINITIONS}/IORPC.h",
    "xnu/iokit/IOKit/IORPC.h",
    "xnu/bsd/sys/make_posix_availability.sh",
    "xnu/bsd/sys/make_symbol_aliasing.sh",
)
CPP_FLAGS = ("-x", "c++", "-std=gnu++1z", "-D__IIG=1", "-DDRIVERKIT_PRIVATE=1",
             "-DPRIVATE_WIFI_ONLY=1", "-DXNU_PLATFORM_MacOSX")
COMMENTS = re.compile(r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|/\*.*?\*/|//[^\n]*''', re.S)
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(include(?:_next)?|import|if|elif)\b(.*)$", re.M)
HEADER = re.compile(r'<([^<>"]+)>|"([^<>"]+)"')
HAS_INCLUDE = re.compile(r"\b__has_include(?:_next)?[ \t]*\(")


class Refusal(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Refusal(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def inventory_digest(inventory):
    return sha256(json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode())


def namespace(path):
    for prefix in ("xnu/", "build/xnu.obj/"):
        if path.startswith(prefix):
            return prefix, path[len(prefix):]
    raise Refusal(f"path outside the archived include namespace: {path}")


def literal_headers(data, source):
    # Follow both conditional branches. An unrecognized macro include may
    # resolve to a new source input and is therefore grounds for refusal.
    text = data.decode("utf-8").replace("\\\r\n", "").replace("\\\n", "")
    text = COMMENTS.sub(lambda token: token[1] or " " + "\n" * token[0].count("\n"), text)
    for kind, args in DIRECTIVE.findall(text):
        if kind in ("if", "elif"):
            matches = []
            for call in HAS_INCLUDE.finditer(args):
                tail = args[call.end():].lstrip()
                header = HEADER.match(tail)
                require(header and tail[header.end():].lstrip().startswith(")"),
                        f"nonliteral include probe in {source}")
                matches.append(header)
        else:
            header = HEADER.fullmatch(args.strip())
            require(header, f"nonliteral include in {source}")
            matches = [header]
        for header in matches:
            quoted = header[2] is not None
            name = header[2] if quoted else header[1]
            require(not name.startswith("/") and "\\" not in name,
                    f"nonportable include in {source}")
            yield quoted, name


def source_inventory(read, classes):
    """Conservatively include all possible repository candidates for literals."""
    pending = [f"{DEFINITIONS}/{name}.iig" for name in classes]
    inventory = {}
    for name in ANCHORS:
        data = read(name)
        require(data is not None, f"missing source anchor: {name}")
        inventory[name] = sha256(data)
    scanned = set()
    while pending:
        path = pending.pop()
        if path in scanned:
            continue
        scanned.add(path)
        data = read(path)
        require(data is not None, f"missing generator input: {path}")
        inventory[path] = sha256(data)
        for quoted, header in literal_headers(data, path):
            roots = (posixpath.dirname(path), *SEARCH_ROOTS) if quoted else SEARCH_ROOTS
            for root in roots:
                candidate = posixpath.normpath(f"{root}/{header}")
                namespace(candidate)
                if candidate not in scanned and read(candidate) is not None:
                    pending.append(candidate)
    return inventory


def load_profile():
    profile = json.loads(PROFILE.read_text(encoding="utf-8"))
    require(profile["schema"] == 1, "unknown replay profile schema")
    return profile


def checked_archive(path, profile):
    pin = profile["artifact"]
    require(path.stat().st_size == pin["zip_bytes"], "artifact ZIP size differs")
    data = path.read_bytes()
    require(sha256(data) == pin["zip_sha256"], "artifact ZIP digest differs")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        require(archive.namelist() == ["buildtree.tgz"], "unexpected ZIP member list")
        require(archive.getinfo("buildtree.tgz").file_size == pin["tgz_bytes"],
                "archive member size differs")
        compressed = archive.read("buildtree.tgz")
    require(sha256(compressed) == pin["tgz_sha256"], "buildtree digest differs")
    raw = gzip.decompress(compressed)
    require(len(raw) == pin["tar_bytes"], "buildtree tar size differs")
    return tarfile.open(fileobj=io.BytesIO(raw), mode="r:")


def prepare(artifact, cache, profile):
    require(not cache.exists() and not cache.is_symlink(), "replay cache must be a new path")
    with checked_archive(artifact, profile) as archive:
        members = {}
        for member in archive:
            if not member.isdir():
                require(member.name not in members, f"duplicate tar member: {member.name}")
                members[member.name] = member

        def read(path):
            member = members.get(path)
            if member is None:
                return None
            stream = archive.extractfile(member)
            require(stream is not None, f"invalid archived file: {path}")
            with stream:
                return stream.read()

        classes = profile["classes"]
        definitions = {posixpath.basename(name) for name in members
                       if posixpath.dirname(name) == DEFINITIONS and name.endswith(".iig")}
        require(definitions == {f"{name}.iig" for name in classes},
                "archived interface list differs")
        inventory = source_inventory(read, classes)
        require(len(inventory) == profile["source_count"] and
                inventory_digest(inventory) == profile["source_sha256"],
                "archived source closure differs")
        outputs = {}
        for name, pair in classes.items():
            for output in (f"{name}.h", f"{name}.iig.cpp"):
                data = read(f"{profile['artifact']['output_directory']}/{output}")
                require(data is not None and sha256(data) == pair[output],
                        f"archived output digest differs: {output}")
                outputs[output] = data

    cache.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".iig-replay-", dir=cache.parent) as temp:
        stage = Path(temp) / "verified"
        stage.mkdir()
        for name, data in outputs.items():
            (stage / name).write_bytes(data)
        (stage / "source-inventory.json").write_text(
            json.dumps(inventory, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        stage.rename(cache)
    print(f"Verified {len(classes)} DriverKit output pairs and {len(inventory)} source inputs")


def validate_recipe(args, src, obj, sdk, classes, cwd):
    require(len(args) == 16 + len(CPP_FLAGS) and args[0] == "--def",
            "unexpected IIG arguments")
    source = Path(args[1])
    name = source.stem
    require(name in classes and source.suffix == ".iig" and source.is_absolute() and
            source.resolve() == (src / "iokit" / "DriverKit" / f"{name}.iig").resolve(),
            "unrecognized IIG interface")
    output = (f"{name}.h", f"{name}.iig.cpp")
    require(tuple(args[2:9]) == ("--header", output[0], "--impl", output[1],
                                "--framework-name", "DriverKit", "--"),
            "unexpected IIG output or framework arguments")
    roots = (src / "iokit", src / "osfmk", src / "bsd", obj / "bsd", src / "EXTERNAL_HEADERS")
    require(all(arg.startswith("-I") and Path(arg[2:]).is_absolute() and
                Path(arg[2:]).resolve() == root.resolve()
                for arg, root in zip(args[9:14], roots, strict=True)),
            "IIG include roots or ordering differ")
    require(args[14] == "-isysroot" and Path(args[15]).is_absolute() and
            Path(args[15]).resolve() == sdk and tuple(args[16:]) == CPP_FLAGS,
            "IIG SDK path or preprocessor flags differ")
    require(cwd.is_relative_to(obj) and cwd.name == "DriverKit" and
            "iokit" in cwd.relative_to(obj).parts, "unexpected IIG output directory")
    return name, output


def live_inventory(src, obj, classes):
    roots = {"xnu/": src, "build/xnu.obj/": obj}
    seen = {}

    def read(path):
        if path not in seen:
            prefix, name = namespace(path)
            root = roots[prefix]
            candidate = root / name
            require(candidate.resolve().is_relative_to(root), f"source escapes build tree: {path}")
            seen[path] = candidate.read_bytes() if candidate.is_file() else None
        return seen[path]

    definitions = {p.name for p in (src / "iokit" / "DriverKit").glob("*.iig")}
    require(definitions == {f"{name}.iig" for name in classes}, "interface list differs")
    return source_inventory(read, classes)


def install_output(path, data):
    require(not path.is_symlink(), f"symlink output refused: {path.name}")
    if path.exists():
        require(path.is_file() and path.read_bytes() == data,
                f"existing IIG output differs: {path.name}")
        return
    # An exclusive link publishes a complete file without replacing a
    # concurrently created output or requiring a GNU make wrapper lock.
    with tempfile.TemporaryDirectory(prefix=".iig-output-", dir=path.parent) as temp:
        complete = Path(temp) / "output"
        complete.write_bytes(data)
        try:
            os.link(complete, path)
        except FileExistsError:
            require(not path.is_symlink() and path.is_file() and path.read_bytes() == data,
                    f"concurrent IIG output differs: {path.name}")


def replay(args, profile, env=os.environ, cwd=None):
    cache, src, obj, sdk = (
        Path(env[f"DRE_IIG_REPLAY_{key}"]).resolve(strict=True)
        for key in ("CACHE", "SRCROOT", "OBJROOT", "SDKROOT")
    )
    cwd = Path.cwd().resolve() if cwd is None else cwd.resolve()
    name, outputs = validate_recipe(args, src, obj, sdk, profile["classes"], cwd)
    settings = plistlib.loads((sdk / "SDKSettings.plist").read_bytes())
    require(settings.get("CanonicalName") == profile["consumer_sdk"],
            "compiler SDK identity differs from the replay profile")
    expected = json.loads((cache / "source-inventory.json").read_text(encoding="utf-8"))
    require(len(expected) == profile["source_count"] and
            inventory_digest(expected) == profile["source_sha256"],
            "prepared source inventory differs")
    actual = live_inventory(src, obj, profile["classes"])
    if actual != expected:
        changes = sorted(path for path in actual.keys() | expected.keys()
                         if actual.get(path) != expected.get(path))
        raise Refusal(f"generator source closure differs: {', '.join(changes[:8])}")

    pair = []
    for output in outputs:
        data = (cache / output).read_bytes()
        require(sha256(data) == profile["classes"][name][output],
                f"prepared output digest differs: {output}")
        target = cwd / output
        require(not target.is_symlink() and (not target.exists() or
                (target.is_file() and target.read_bytes() == data)),
                f"existing IIG output differs: {output}")
        pair.append((target, data))
    for target, data in pair:
        install_output(target, data)
    print(f"Replayed verified DriverKit-286 pair: {name}")


def main():
    profile = load_profile()
    if sys.argv[1:2] == ["prepare"]:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--artifact", type=Path, required=True)
        parser.add_argument("--cache", type=Path, required=True)
        args = parser.parse_args(sys.argv[2:])
        prepare(args.artifact.resolve(), args.cache.absolute(), profile)
    else:
        replay(sys.argv[1:], profile)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, tarfile.TarError, zipfile.BadZipFile,
            plistlib.InvalidFileException) as error:
        sys.exit(f"IIG replay refused: {error}")
