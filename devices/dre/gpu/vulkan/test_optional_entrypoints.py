"""Regression checks for the strict Darwin Turnip relink."""

from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import TestCase, main

from build_turnip_dre import ENTRYPOINT_TABLES, resolve_optional_entries


SOURCE = """#define VK_ENTRY_USE_WEAK 1
const struct vk_device_entrypoint_table tu_device_entrypoints_a6xx = {
    .CmdDraw = tu_CmdDraw<A6XX>,
    .CmdDispatch = tu_CmdDispatch<A6XX>,
};
"""
LINK_LOG = """Undefined symbols for architecture arm64:
  "void tu_CmdDraw<(chip)6>(VkCommandBuffer_T*, unsigned int)", referenced from:
      _tu_device_entrypoints_a6xx in meson-generated_.._tu_entrypoints.cc.o
ld: symbol(s) not found for architecture arm64
"""
RELATIVE = ENTRYPOINT_TABLES["tu_entrypoints.cc"]
GENERATOR = f"python vk_entrypoints_gen.py --weak --out-c {RELATIVE} --prefix tu"


class OptionalEntrypointsTest(TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.build_dir = Path(tmp.name)
        self.source = self.build_dir / RELATIVE
        self.source.parent.mkdir(parents=True)
        self.source.write_text(SOURCE, encoding="utf-8")

    def test_only_the_unresolved_slot_is_nulled(self):
        self.assertEqual(resolve_optional_entries(self.build_dir, LINK_LOG, GENERATOR), {RELATIVE: 1})
        self.assertEqual(self.source.read_text(encoding="utf-8"), SOURCE.replace("tu_CmdDraw<A6XX>", "0"))

    def test_a_strong_reference_refuses_the_relink(self):
        log = LINK_LOG.replace("ld: symbol", "      _real_call in driver.o\nld: symbol")
        with self.assertRaisesRegex(ValueError, "outside optional entrypoint tables"):
            resolve_optional_entries(self.build_dir, log, GENERATOR)
        self.assertEqual(self.source.read_text(encoding="utf-8"), SOURCE)

    def test_a_missing_exact_slot_refuses_the_relink(self):
        log = LINK_LOG.replace("tu_CmdDraw", "tu_CmdCopy")
        with self.assertRaisesRegex(ValueError, "do not match generated slots"):
            resolve_optional_entries(self.build_dir, log, GENERATOR)
        self.assertEqual(self.source.read_text(encoding="utf-8"), SOURCE)


if __name__ == "__main__":
    main()
