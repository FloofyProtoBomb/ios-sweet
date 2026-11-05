#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""UNVALIDATED source connector; never run during the overnight source period.

Called only for a fresh build_frontend.py --dre-compiler-service staging tree.
Replaces the exact pinned frontend's compiler-service consumer with the real
selected306 connection. Does not modify previous SDK objects/source trees,
execute a compiler/service, build, install or publish any GPU/Metal device.
"""
from __future__ import annotations
import hashlib
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent


def connect(staged: Path) -> dict:
    path = staged / "NativeBridgeMetalFrontend.mm"
    source = path.read_text(encoding="utf-8")
    if '#include "compiler_service.h"' in source:
        raise ValueError("compiler service already connected; require fresh prepared source")
    original = source
    include = '#include "compiler_request.h"'
    if source.count(include) != 1:
        raise ValueError("selected request13 codec include changed")
    source = source.replace(include, include + '\n#include "compiler_service.h"', 1)
    begin = "// The system's own front end (MTLCompiler.framework"
    end = "// Metal source, request 13:"
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("selected compiler-consumer source boundaries changed")
    start, stop = source.index(begin), source.index(end)
    section = source[start:stop]
    for marker in ("MTLCodeGenServiceCreate", "MTLCodeGenServiceBuildRequest", "shaderCacheRead",
                   "Versions/Current", "static char gCompilerLoaded", "static NSData *codeGenRequest"):
        if marker not in section:
            raise ValueError("selected compiler consumer changed: " + marker)
    replacement = '''// UNVALIDATED DRE source connection: real selected20H392 compiler306 only.
// No latest-version scan, unqualified request14 or unchecked cache reply bypass.
static char gCompilerLoaded[16]="untried";
static std::atomic<uint32_t> gStitchMade,gStitchFailed;
static NSData *codeGenRequest(uint32_t type,const std::vector<uint8_t> &request,NSString **warnings,NSString **failure) {
 const uint64_t began=nowUs();
 NSError *error=nil;
 NSData *library=dre_metal306_compile_request(type,request.data(),request.size(),warnings,&error);
 gTimeCompile.add(began);
 strlcpy(gCompilerLoaded,library?"306-20H392":"306-refused",sizeof gCompilerLoaded);
 if(failure)*failure=error.localizedDescription;
 return library;
}
'''
    source = source[:start] + replacement + source[stop:]
    old = '''// Metal source, request 13: two little-endian u64 lengths, the source NUL-padded to a multiple of 4 bytes and the arguments
// NUL-terminated.'''
    new = '''// Metal306 request13: four little-endian u32 fields (source+NUL, metadata0,
// arguments+NUL, flags0), followed by4-byte-padded source and arguments.'''
    if source.count(old) != 1:
        raise ValueError("selected request13 comment context changed")
    source = source.replace(old, new, 1)
    stitched = '- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error {'
    after = '- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d destinationBinaryArchive:'
    if source.count(stitched) != 1 or source.count(after) != 1:
        raise ValueError("selected stitched-library method boundaries changed")
    start, stop = source.index(stitched), source.index(after)
    section = source[start:stop]
    if 'codeGenRequest(14,request' not in section or "macOS 27's compiler" not in section:
        raise ValueError("selected newer stitching request path changed")
    source = source[:start] + '''- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error {
 (void)d;gStitchFailed++;
 if(error)*error=[NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorUnsupported
  userInfo:@{NSLocalizedDescriptionKey:@"DRE Metal306 request14 stitching packet ABI is not qualified"}];
 return nil;
}
''' + source[stop:]
    # This decoder was used solely by the newer stitching request builder.
    # Remove it from this connected profile instead of suppressing unused-code
    # diagnostics or retaining an apparently active unqualified request path.
    bitcode = "// A function's wrapped bitcode in its library's metallib:"
    next_decoder = "// Each entry of a metallib's function list,"
    if source.count(bitcode) != 1 or source.count(next_decoder) != 1:
        raise ValueError("selected stitching-only bitcode decoder boundaries changed")
    start, stop = source.index(bitcode), source.index(next_decoder)
    if source[start:stop].count("static NSData *functionBitcode(") != 1:
        raise ValueError("selected stitching-only decoder changed")
    source = source[:start] + source[stop:]
    files = {}
    for name in ("compiler_service.h", "compiler_service.mm"):
        source_file = HERE / name
        if source_file.is_symlink() or not source_file.is_file():
            raise ValueError("ordinary compiler connection source missing: " + name)
        if (staged / name).exists():
            raise ValueError("compiler connection destination must be fresh: " + name)
        shutil.copyfile(source_file, staged / name)
        files[name] = hashlib.sha256((staged / name).read_bytes()).hexdigest()
    path.write_text(source, encoding="utf-8", newline="\n")
    return {"status": "UNVALIDATED_COMPILER306_SOURCE_CONNECTED", "source_recipe_qualified": False,
        "original_frontend_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "connected_frontend_sha256": hashlib.sha256(source.encode()).hexdigest(), "sources_sha256": files,
        "compiler_profile": "20H392/MTLCompiler306.6.6/0ACFC872-8027-3DFD-8F30-397C9CB25639",
        "accepted_request_types": [13], "request14_supported": False,
        "unchecked_shader_cache_bypass_removed": True, "highest_compiler_search_removed": True,
        "compiler_runtime_exercised": False, "metal_acceleration_pass": False}
