#!/usr/bin/env python3
"""Small synthetic archive and source tree tests for the replay guard."""
import gzip
import io
import json
from pathlib import Path
import plistlib
import tarfile
import tempfile
import unittest
import zipfile

import replay


class ReplayTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        base = Path(temp.name).resolve()
        self.src = base / "xnu"
        self.obj = base / "build/xnu.obj"
        self.sdk = base / "MacOSX15.5.sdk"
        self.cwd = self.obj / "DEVELOPMENT_ARM64_DRE/iokit/DriverKit"
        self.cache = base / "prepared"
        self.archive = base / "artifact.zip"
        self.cwd.mkdir(parents=True)
        self.sdk.mkdir()
        self.sdk_settings("macosx15.5")
        self.sources = {name: b"pinned anchor\n" for name in replay.ANCHORS}
        self.sources.update({
            "xnu/iokit/DriverKit/Foo.iig": b"#include <mach/port.h>\n",
            "xnu/osfmk/mach/port.h": (
                b"#include <sys/generated.h>\n"
                b"#if __has_include(<optional.h>)\n#define HAS_OPTIONAL 1\n#endif\n"
            ),
            "build/xnu.obj/bsd/sys/generated.h": b"#define FOO 1\n",
        })
        for path, data in self.sources.items():
            self.write_source(path, data)
        self.outputs = {"Foo.h": b"fixture header\n", "Foo.iig.cpp": b"fixture implementation\n"}
        classes = {"Foo": {name: replay.sha256(data) for name, data in self.outputs.items()}}
        source_inventory = replay.source_inventory(self.sources.get, classes)
        output_directory = "build/xnu.obj/DEVELOPMENT_ARM64_DRE/iokit/DriverKit"
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as writer:
            contents = dict(self.sources)
            contents.update({f"{output_directory}/{n}": d for n, d in self.outputs.items()})
            for name, data in contents.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                writer.addfile(member, io.BytesIO(data))
        raw = archive.getvalue()
        compressed = gzip.compress(raw, mtime=0)
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("buildtree.tgz", compressed)
        zipped = self.archive.read_bytes()
        self.profile = {
            "schema": 1,
            "artifact": {
                "zip_bytes": len(zipped), "zip_sha256": replay.sha256(zipped),
                "tgz_bytes": len(compressed), "tgz_sha256": replay.sha256(compressed),
                "tar_bytes": len(raw), "output_directory": output_directory,
            },
            "consumer_sdk": "macosx15.5",
            "source_count": len(source_inventory),
            "source_sha256": replay.inventory_digest(source_inventory),
            "classes": classes,
        }
        self.env = {f"DRE_IIG_REPLAY_{name}": str(value) for name, value in {
            "CACHE": self.cache, "SRCROOT": self.src, "OBJROOT": self.obj, "SDKROOT": self.sdk,
        }.items()}
        self.recipe = [
            "--def", str(self.src / "iokit/DriverKit/Foo.iig"),
            "--header", "Foo.h", "--impl", "Foo.iig.cpp", "--framework-name", "DriverKit", "--",
            *(f"-I{base / path}" for path in replay.SEARCH_ROOTS),
            "-isysroot", str(self.sdk), *replay.CPP_FLAGS,
        ]
        replay.prepare(self.archive, self.cache, self.profile)

    def write_source(self, name, data):
        prefix, relative = replay.namespace(name)
        root = self.src if prefix == "xnu/" else self.obj
        file = root / relative
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)

    def sdk_settings(self, name):
        (self.sdk / "SDKSettings.plist").write_bytes(plistlib.dumps({"CanonicalName": name}))

    def run_replay(self, recipe=None):
        replay.replay(self.recipe if recipe is None else recipe,
                      self.profile, env=self.env, cwd=self.cwd)

    def assert_refused(self, reason, recipe=None):
        with self.assertRaisesRegex(replay.Refusal, reason):
            self.run_replay(recipe)
        self.assertEqual(list(self.cwd.iterdir()), [])

    def test_replays_exact_pair_and_allows_matching_outputs_on_retry(self):
        self.run_replay()
        self.run_replay()
        self.assertEqual({p.name: p.read_bytes() for p in self.cwd.iterdir()}, self.outputs)

    def test_bootstrap_replays_before_kernel_object_directory_exists(self):
        bootstrap = self.obj.with_name("xnu-hdrs.obj")
        relative = self.cwd.relative_to(self.obj)
        self.obj.rename(bootstrap)
        self.cwd = bootstrap / relative
        self.assertFalse(self.obj.exists())
        recipe = self.recipe.copy()
        recipe[12] = f"-I{bootstrap / 'bsd'}"
        self.run_replay(recipe)
        self.assertEqual({p.name: p.read_bytes() for p in self.cwd.iterdir()}, self.outputs)

    def test_refuses_unrecognized_object_root(self):
        different = self.obj.with_name("other.obj")
        relative = self.cwd.relative_to(self.obj)
        self.obj.rename(different)
        self.cwd = different / relative
        recipe = self.recipe.copy()
        recipe[12] = f"-I{different / 'bsd'}"
        self.assert_refused("unexpected IIG object root", recipe)

    def test_refuses_object_root_outside_configured_build_directory(self):
        outside = self.src.parent / "outside" / "xnu-hdrs.obj"
        outside.parent.mkdir()
        relative = self.cwd.relative_to(self.obj)
        self.obj.rename(outside)
        self.cwd = outside / relative
        recipe = self.recipe.copy()
        recipe[12] = f"-I{outside / 'bsd'}"
        self.assert_refused("IIG output is outside the build directory", recipe)

    def test_refuses_changed_definition(self):
        self.write_source("xnu/iokit/DriverKit/Foo.iig", b"changed\n")
        self.assert_refused(r"source closure differs.*Foo\.iig")

    def test_refuses_changed_generated_dependency(self):
        self.write_source("build/xnu.obj/bsd/sys/generated.h", b"#define FOO 2\n")
        self.assert_refused(r"source closure differs.*generated\.h")

    def test_refuses_new_source_shadow_and_optional_include(self):
        for path in ("xnu/iokit/mach/port.h", "xnu/EXTERNAL_HEADERS/optional.h"):
            with self.subTest(path=path):
                self.write_source(path, b"new include candidate\n")
                self.assert_refused("source closure differs")
                (self.src / replay.namespace(path)[1]).unlink()

    def test_refuses_unknown_class_output_or_flags(self):
        for offset, value, reason in (
            (1, str(self.src / "iokit/DriverKit/New.iig"), "unrecognized IIG interface"),
            (3, "../Foo.h", "IIG output or framework arguments"),
            (9, f"-I{self.src / 'bsd'}", "include roots or ordering"),
            (-1, "-DXNU_PLATFORM_DriverKit", "preprocessor flags"),
        ):
            with self.subTest(offset=offset):
                recipe = self.recipe.copy()
                recipe[offset] = value
                self.assert_refused(reason, recipe)

    def test_refuses_wrong_compiler_sdk(self):
        self.sdk_settings("macosx15.4")
        self.assert_refused("compiler SDK identity differs")

    def test_refuses_corrupt_second_output_before_writing_first(self):
        (self.cache / "Foo.iig.cpp").write_bytes(b"corrupt implementation\n")
        self.assert_refused(r"output digest differs.*Foo\.iig\.cpp")

    def test_refuses_tampered_inventory(self):
        state = self.cache / "source-inventory.json"
        inventory = json.loads(state.read_text(encoding="utf-8"))
        inventory["xnu/iokit/DriverKit/Foo.iig"] = "0" * 64
        state.write_text(json.dumps(inventory), encoding="utf-8")
        self.assert_refused("prepared source inventory differs")

    def test_refuses_existing_different_output_without_modifying_it(self):
        other = self.cwd / "Foo.iig.cpp"
        other.write_bytes(b"existing different implementation\n")
        with self.assertRaisesRegex(replay.Refusal, "existing IIG output differs"):
            self.run_replay()
        self.assertEqual(list(self.cwd.iterdir()), [other])
        self.assertEqual(other.read_bytes(), b"existing different implementation\n")

    def test_refuses_altered_archive_before_creating_cache(self):
        data = bytearray(self.archive.read_bytes())
        data[0] ^= 1
        self.archive.write_bytes(data)
        cache = self.cache.with_name("rejected")
        with self.assertRaisesRegex(replay.Refusal, "artifact ZIP digest differs"):
            replay.prepare(self.archive, cache, self.profile)
        self.assertFalse(cache.exists())

    def saved_copy(self, members):
        wrapper = self.archive.with_name("saved-copy.zip")
        with zipfile.ZipFile(wrapper, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in members:
                archive.writestr(name, data)
        return wrapper

    def assert_bad_saved_copy(self, wrapper, reason):
        cache = self.cache.with_name("rejected-copy")
        with self.assertRaisesRegex(replay.Refusal, reason):
            replay.prepare(wrapper, cache, self.profile)
        self.assertFalse(cache.exists())

    def test_replays_saved_copy_with_exact_original_zip(self):
        wrapper = self.saved_copy([(replay.SAVED_ORIGIN_MEMBER, self.archive.read_bytes())])
        cache = self.cache.with_name("prepared-copy")
        replay.prepare(wrapper, cache, self.profile)
        self.assertEqual({p.name: p.read_bytes() for p in cache.iterdir()},
                         {p.name: p.read_bytes() for p in self.cache.iterdir()})

    def test_refuses_wrong_saved_copy_member_or_extra_member(self):
        data = self.archive.read_bytes()
        for members in (
            [("../dre-iig-original.zip", data)],
            [("different.zip", data)],
            [(replay.SAVED_ORIGIN_MEMBER, data), ("extra", b"extra")],
        ):
            with self.subTest(names=[name for name, _ in members]):
                self.assert_bad_saved_copy(self.saved_copy(members), "must contain only")

    def test_refuses_wrong_inner_digest_or_size(self):
        data = self.archive.read_bytes()
        corrupt = bytes([data[0] ^ 1]) + data[1:]
        for inner, reason in (
            (corrupt, "original ZIP digest differs"),
            (data + b"x", "member size differs"),
        ):
            with self.subTest(reason=reason):
                wrapper = self.saved_copy([(replay.SAVED_ORIGIN_MEMBER, inner)])
                self.assert_bad_saved_copy(wrapper, reason)

    def test_refuses_oversize_outer_archive(self):
        wrapper = self.archive.with_name("oversize.zip")
        limit = self.profile["artifact"]["zip_bytes"] + replay.MAX_WRAPPER_OVERHEAD
        wrapper.write_bytes(bytes(limit + 1))
        self.assert_bad_saved_copy(wrapper, "exceeds the saved-copy size bound")


if __name__ == "__main__":
    unittest.main()
