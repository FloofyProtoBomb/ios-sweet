#!/usr/bin/env python3
"""Exercise the hook through real GNU make, including DXB's exported CC/CXX."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('enable_xnu_ccache', HERE / 'enable-xnu-ccache.py')
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)

# Exact default-origin guards from xnu-8796's makedefs/MakeInc.cmd. DXB's
# setup_xcode_toolchain exports compilers, bypassing these assignments.
DEFINITIONS = '''ifeq ($(origin CC),default)
\texport CC := $(shell $(XCRUN) -sdk $(SDKROOT) -find clang)
endif
ifeq ($(origin CXX),default)
\texport CXX := $(shell $(XCRUN) -sdk $(SDKROOT) -find clang++)
endif
'''
FIXTURE = '''XCRUN := $(CURDIR)/fake-xcrun
SDKROOT := fixture
HOSTCC := /host/clang -host-only
''' + DEFINITIONS + '''
.PHONY: all child
all:
\t@printf '%s\\n' 'CC=$(CC)' 'CXX=$(CXX)' 'HOSTCC=$(HOSTCC)'
\t@$(MAKE) --no-print-directory -f Makefile child
child:
\t@printf '%s\\n' 'CHILD_CC=$(CC)' 'CHILD_CXX=$(CXX)' 'CHILD_HOSTCC=$(HOSTCC)'
'''


class CompilerCacheHook(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.make = shutil.which(os.environ.get('DRE_TEST_MAKE', 'make'))
        if not cls.make:
            raise unittest.SkipTest('GNU make is required for real compiler-selection tests')
        banner = subprocess.check_output([cls.make, '--version'], text=True)
        if 'GNU Make' not in banner:
            raise unittest.SkipTest('test requires GNU make, matching XNU')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dre-ccache-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / 'Makefile'
        self.path.write_bytes(FIXTURE.encode())
        xcrun = self.root / 'fake-xcrun'
        xcrun.write_text('#!/bin/sh\nfor tool do :; done\nprintf "/default/%s\\n" "$tool"\n')
        xcrun.chmod(0o755)

    def run_make(self, environment=None, arguments=()):
        env = dict(os.environ)
        for name in ('CC','CXX','MIGCC','DRE_CCACHE','MAKEFLAGS','MFLAGS','MAKEOVERRIDES'):
            env.pop(name, None)
        env.update(environment or {})
        result = subprocess.run([self.make, '--no-print-directory', '-f', 'Makefile', *arguments],
                                cwd=self.root, env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def assert_compilers(self, output, cc, cxx):
        self.assertEqual(output, '\n'.join((f'CC={cc}', f'CXX={cxx}',
            'HOSTCC=/host/clang -host-only', f'CHILD_CC={cc}', f'CHILD_CXX={cxx}',
            'CHILD_HOSTCC=/host/clang -host-only')) + '\n')

    def test_unset_and_empty_cache_are_byte_equivalent_commands(self):
        for environment, arguments in (({}, ()),
            ({'CC':'/chosen/clang -fno-builtin','CXX':'/chosen/clang++ -fno-exceptions'}, ()),
            ({}, ('CC=/override/clang -O1','CXX=/override/clang++ -O2'))):
            with self.subTest(environment=environment, arguments=arguments):
                self.path.write_bytes(FIXTURE.encode())
                before = self.run_make(environment, arguments)
                hook.enable(self.path)
                self.assertEqual(self.run_make(environment, arguments), before)
                self.assertEqual(self.run_make(dict(environment, DRE_CCACHE=''), arguments), before)

    def test_default_compilers_and_recursive_make(self):
        hook.enable(self.path)
        self.assert_compilers(self.run_make({'DRE_CCACHE':'/cache/ccache'}),
            '/cache/ccache /default/clang', '/cache/ccache /default/clang++')

    def test_dxb_exported_compilers_and_flags(self):
        hook.enable(self.path)
        self.assert_compilers(self.run_make({'DRE_CCACHE':'/cache/ccache',
            'CC':'/selected/Xcode/clang -fno-builtin', 'CXX':'/selected/Xcode/clang++ -fno-exceptions'}),
            '/cache/ccache /selected/Xcode/clang -fno-builtin',
            '/cache/ccache /selected/Xcode/clang++ -fno-exceptions')

    def test_command_line_compilers_override_exported_compilers(self):
        hook.enable(self.path)
        self.assert_compilers(self.run_make({'DRE_CCACHE':'/cache/ccache',
            'CC':'/environment/clang', 'CXX':'/environment/clang++'},
            ('CC=/override/clang -O1','CXX=/override/clang++ -O2')),
            '/cache/ccache /override/clang -O1', '/cache/ccache /override/clang++ -O2')

    def test_existing_cache_prefix_is_not_duplicated(self):
        hook.enable(self.path)
        for existing in ('/cache/ccache', 'ccache'):
            with self.subTest(existing=existing):
                self.assert_compilers(self.run_make({'DRE_CCACHE':'/cache/ccache',
                    'CC':existing+' /selected/clang', 'CXX':existing+' /selected/clang++'}),
                    existing+' /selected/clang', existing+' /selected/clang++')

    def test_make_invokes_wrapper_and_preserves_compiler_argv(self):
        compiler = self.root / 'selected-compiler'
        compiler.write_text('#!/usr/bin/env python3\nimport json,sys\n'
                            'open("received.json","w").write(json.dumps(sys.argv[1:]))\n')
        compiler.chmod(0o755)
        cache = self.root / 'cache-wrapper'
        cache.write_text('#!/bin/sh\nprintf "invoked\\n" >> cache-invocations\nexec "$@"\n')
        cache.chmod(0o755)
        self.path.write_text('XCRUN := $(CURDIR)/fake-xcrun\nSDKROOT := fixture\n' +
            DEFINITIONS + "all:\n\t@$(CC) --mode='selected flag' -DNAME='two words' -c input.c -o output.o\n")
        self.run_make({'CC':str(compiler)})
        before = json.loads((self.root/'received.json').read_text())
        hook.enable(self.path)
        self.run_make({'CC':str(compiler), 'DRE_CCACHE':str(cache)})
        self.assertEqual(json.loads((self.root/'received.json').read_text()), before)
        self.assertEqual(before, ['--mode=selected flag','-DNAME=two words','-c','input.c','-o','output.o'])
        self.assertEqual((self.root/'cache-invocations').read_text(), 'invoked\n')

    def test_hook_is_idempotent_and_changes_no_original_bytes(self):
        original = self.path.read_bytes()
        hook.enable(self.path)
        once = self.path.read_bytes()
        self.assertEqual(once.replace(hook.BLOCK.encode(), b''), original)
        hook.enable(self.path)
        self.assertEqual(self.path.read_bytes(), once)

    def test_mig_executes_selected_preprocessor_as_one_path(self):
        compiler = self.root / 'selected-preprocessor'
        compiler.write_text('#!/usr/bin/env python3\nimport json,sys\n'
            'open("mig-received.json","w").write(json.dumps(sys.argv[1:]))\n')
        compiler.chmod(0o755)
        cache = self.root / 'cache-wrapper'
        cache.write_text('#!/bin/sh\nprintf "wrong\\n" >> mig-cache-calls\nexec "$@"\n')
        cache.chmod(0o755)
        mig = '''ifeq ($(origin MIGCC),undefined)
export MIGCC := $(CC)
endif
all:
\t@"$(MIGCC)" --mode='MIG preprocessing' -E input.defs
'''
        for selected in (str(compiler), str(cache)+' '+str(compiler)):
            with self.subTest(selected=selected):
                self.path.write_bytes(('XCRUN := $(CURDIR)/fake-xcrun\nSDKROOT := fixture\n'+
                    DEFINITIONS+mig).encode())
                hook.enable(self.path)
                self.run_make({'CC':selected,'DRE_CCACHE':str(cache)})
                self.assertEqual(json.loads((self.root/'mig-received.json').read_text()),
                    ['--mode=MIG preprocessing','-E','input.defs'])
                self.assertFalse((self.root/'mig-cache-calls').exists())

    def test_mig_explicit_preprocessor_override_is_preserved(self):
        self.path.write_bytes((FIXTURE+'\nshow-mig:\n\t@printf "%s\\n" "$(MIGCC)"\n').encode())
        hook.enable(self.path)
        self.assertEqual(self.run_make({'CC':'/chosen/clang','DRE_CCACHE':'/cache/ccache',
            'MIGCC':'/explicit/preprocessor'}, ('show-mig',)), '/explicit/preprocessor\n')

    def test_crlf_bytes_are_preserved(self):
        original = FIXTURE.replace('\n','\r\n').encode()
        self.path.write_bytes(original)
        hook.enable(self.path)
        once = self.path.read_bytes()
        self.assertEqual(once.replace(hook.BLOCK.replace('\n','\r\n').encode(), b''), original)
        hook.enable(self.path)
        self.assertEqual(self.path.read_bytes(), once)

    def test_legacy_default_only_prefix_is_migrated(self):
        self.path.write_text(FIXTURE.replace(':= $(shell $(XCRUN)', ':= $(DRE_CCACHE) $(shell $(XCRUN)'))
        hook.enable(self.path)
        self.assertEqual(self.path.read_bytes().replace(hook.BLOCK.encode(),b''), FIXTURE.encode())
        self.assert_compilers(self.run_make({'DRE_CCACHE':'/cache/ccache',
            'CC':'/selected/clang','CXX':'/selected/clang++'}),
            '/cache/ccache /selected/clang', '/cache/ccache /selected/clang++')

    def test_unsupported_layout_is_rejected_without_mutation(self):
        for content in (FIXTURE.replace('export CXX :=', 'export OTHER_CXX :='),
                        FIXTURE.replace('ifeq ($(origin CC),default)', 'ifeq ($(origin CC),environment)'),
                        FIXTURE.replace('export CC := $(shell', 'export CC := $(DRE_CCACHE) $(shell'),
                        FIXTURE + hook.BEGIN + '\n'):
            with self.subTest(content=content):
                self.path.write_bytes(content.encode())
                with self.assertRaises(ValueError):
                    hook.enable(self.path)
                self.assertEqual(self.path.read_bytes(), content.encode())


if __name__ == '__main__':
    unittest.main(verbosity=2)
