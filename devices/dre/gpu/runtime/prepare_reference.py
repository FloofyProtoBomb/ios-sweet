#!/usr/bin/env python3
"""Prepare a pinned, source-only Metal frontend reference for the 20H392 ABI.

The existing A740 publication wrapper, renderer patches, probe daemon and
publish marker are intentionally not inputs. This produces no runnable plugin.
The bounded compiler envelope, strict missing-method handling and 528-byte class
guard are only source adaptations; native GPU execution remains unqualified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from inspect_abi import PROFILE, verify_captures

HERE = Path(__file__).resolve().parent


def adapt_extent(source: str) -> str:
    original = 'if(!base || class_getInstanceSize(base)!=536){NBLOG(@"native-metal: unexpected _MTLDevice ABI");return nil;}'
    replacement = 'if(!base || class_getInstanceSize(base)!=528){NBLOG(@"DRE 20H392: unexpected _MTLDevice ABI");return nil;}'
    if source.count(original) != 1:
        raise ValueError("pinned frontend class-extent guard changed")
    return source.replace(original, replacement).replace(
        "// The target's base class ABI (iPadOS 17.7.11, 536 bytes): the dynamic subclass adds no instance variables and reads none of the base's,\n"
        " // so only 17's size is held to; the arm64e build (iPadOS 27, whose _MTLDevice is larger) needs the class to exist.",
        "// DRE source reference only: iOS 16.7.16/20H392 has a 528-byte base.\n"
        " // This guard changes no capabilities, resources, compiler, queue or publication policy.")


def adapt_frontend(source: str) -> str:
    source = adapt_extent(source)
    core = '''// MPSCore reads the GPU core count of every Apple GPU family (no count: no MPS device, and its image cache is nil); the Adreno's
// shader processors count as eight cores, which only sizes MPS's kernel choices.
- (NSUInteger)gpuCoreCount {return 8;}'''
    if source.count(core) != 1:
        raise ValueError("pinned A740 core-count policy changed")
    source = source.replace(core, '''// DRE: no qualified mapping to Apple's gpuCoreCount query exists. The
// method is deliberately absent; a required direct call fails as unsupported.''')
    caller = '''static BOOL (*gFamilyAnswer)(id,SEL,NSInteger);
static BOOL familyForCaller(id self,SEL _cmd,NSInteger family) {
 if(family>=1003 && family<2000) {   // MTLGPUFamilyApple3 and up
  Dl_info caller;
  if(dladdr(__builtin_return_address(0),&caller) && caller.dli_fname && strstr(caller.dli_fname,"/RenderBox.framework/"))return NO;
 }
 return gFamilyAnswer(self,_cmd,family);
}
'''
    install = ''' if(Method family=class_getInstanceMethod(cls,@selector(supportsFamily:))) {
  gFamilyAnswer=(BOOL (*)(id,SEL,NSInteger))method_getImplementation(family);
  method_setImplementation(family,(IMP)familyForCaller);
 }
'''
    for original in (caller, install):
        if source.count(original) != 1:
            raise ValueError("pinned caller-specific family policy changed")
        source = source.replace(original, "")
    xr = 'if(!strcmp(name,"supportsExtendedXR10Formats") && (!strstr(getprogname(),"Poster") || !strcmp(getprogname(),"PosterBoard")))value=0;'
    if source.count(xr) != 1:
        raise ValueError("pinned process-specific XR policy changed")
    source = source.replace(xr, 'if(!strcmp(name,"supportsExtendedXR10Formats"))value=0; // DRE scanout is 8-bit; no per-process capability policy')
    if source.count("MTLLanguageVersion3_1") != 1:
        raise ValueError("pinned default language selection changed")
    source = source.replace("MTLLanguageVersion3_1", "MTLLanguageVersion3_0")
    signature = 'return [NSMethodSignature signatureWithObjCTypes:"@@:"];'
    if source.count(signature) != 2:
        raise ValueError("pinned unknown-selector fallback changed")
    source = source.replace(signature, "return nil;")
    original = '''static void answerMissing(NSInvocation *invocation) {
 const NSUInteger length=invocation.methodSignature.methodReturnLength;
 if(!length)return;
 alignas(16) unsigned char zero[256]={};
 if(length<=sizeof zero)[invocation setReturnValue:zero];
}'''
    if source.count(original) != 1:
        raise ValueError("pinned zero-result fallback changed")
    source = source.replace(original, '''static void answerMissing(NSInvocation *invocation) {
 [invocation.target doesNotRecognizeSelector:invocation.selector];
}''').replace('(answered nil)', '(refused)')
    include = '#include "NativeBridgeMetalFrontend.h"'
    if source.count(include) != 1:
        raise ValueError("pinned frontend include changed")
    source = source.replace(include, include + '\n#include "compiler_request.h"')
    original = ''' const uint64_t sourceBytes=(text.length+1+3)&~uint64_t(3),optionBytes=strlen(options)+1;
 std::vector<uint8_t> request(16+sourceBytes+optionBytes,0);
 memcpy(request.data(),&sourceBytes,8);memcpy(request.data()+8,&optionBytes,8);
 memcpy(request.data()+16,text.bytes,text.length);memcpy(request.data()+16+sourceBytes,options,optionBytes);'''
    if source.count(original) != 1:
        raise ValueError("pinned request encoder changed")
    source = source.replace(original, ''' size_t needed=0,used=0;
 if(!options || !dre_metal306_request13_size(text.length,strlen(options),&needed)) {
  *failure=@"DRE Metal306: source or arguments exceed the bounded compiler profile";return nil;
 }
 std::vector<uint8_t> request(needed);
 if(!dre_metal306_request13(text.bytes,text.length,options,strlen(options),request.data(),request.size(),&used) || used!=needed) {
  *failure=@"DRE Metal306: invalid source or argument bytes";return nil;
 }''')
    original = '''   uint32_t header=0,warning=0;
   if(reply && length>=16){memcpy(&header,(const uint8_t *)reply+8,4);memcpy(&warning,(const uint8_t *)reply+12,4);}
   if(status || !reply || length<16 || (uint64_t)header+warning>length) {
    message=why && *why?@(why):[NSString stringWithFormat:@"the Metal compiler failed (status %d)",status];
    return;
   }
   if(warning)warned=[[NSString alloc] initWithBytes:(const uint8_t *)reply+header length:warning encoding:NSUTF8StringEncoding];
   metallib=[NSData dataWithBytes:(const uint8_t *)reply+header+warning length:length-header-warning];'''
    if source.count(original) != 1:
        raise ValueError("pinned compiler reply consumer changed")
    source = source.replace(original, '''   struct dre_metal306_reply view={};
   if(!dre_metal306_reply(status,reply,length,&view)) {
    message=why && *why?@(why):[NSString stringWithFormat:@"DRE Metal306: compiler error or unqualified archive envelope (status %d)",status];
    return;
   }
   if(view.warning_bytes)warned=[[NSString alloc] initWithBytes:view.warnings length:view.warning_bytes encoding:NSUTF8StringEncoding];
   metallib=[NSData dataWithBytes:view.metallib length:view.metallib_bytes];''')
    return source


def adapt_bridge(source: str) -> str:
    forced = '(d.supports_family(arg as i64) || TARGET_FAMILIES.iter().any(|f| f.raw() == arg as i64)) as u64'
    if source.count(forced) != 1:
        raise ValueError("pinned forced-family answer changed")
    source = source.replace(forced, 'd.supports_family(arg as i64) as u64')
    start = source.index('/// The GPU families of the target hardware')
    end = source.index('const TARGET_FAMILIES:', start)
    source = source[:start] + '/// Audit-only family rows. No family is forced true for this port.\n' + source[end:]
    source = source.replace('TARGET_FAMILIES', 'AUDIT_FAMILIES')
    return source


def prepare(source: Path, capture: Path | None, out: Path, *, profile_only=False) -> dict:
    pin = json.loads((HERE / "upstream.json").read_text(encoding="utf-8"))
    if capture is not None:
        observed = verify_captures(capture)
    elif profile_only:
        # CI compiles public source from the already qualified, committed ABI
        # profile. It does not receive or re-inspect any Apple private capture.
        observed = {"class": {"instance_size": PROFILE["class_instance_size"]}}
    else:
        raise ValueError("provide a private capture or explicit source-profile-only mode")
    if observed["class"]["instance_size"] != PROFILE["class_instance_size"]:
        raise ValueError("unexpected qualified ABI extent")
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                     text=True, timeout=20).strip()
    if commit != pin["commit"]:
        raise ValueError("upstream commit differs from the audited reference")
    contents = {}
    for name, digest in pin["files"].items():
        raw = (source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError(f"upstream source hash mismatch: {name}")
        contents[name] = raw
    frontend = adapt_frontend(contents["NativeBridgeMetalFrontend.mm"].decode("utf-8"))
    contents["NativeBridgeMetalFrontend.mm"] = frontend.encode("utf-8")
    contents["bridge/NativeMetalObjects.rs"] = adapt_bridge(contents["bridge/NativeMetalObjects.rs"].decode("utf-8")).encode("utf-8")
    for name in ("compiler_request.h", "compiler_request.c"):
        contents[name] = (HERE / name).read_bytes()
    # No automatic feature table, gpuCoreCount, transport, ownership or display
    # policy substitution is justified by a class-size observation.
    out.mkdir(parents=True, exist_ok=False)
    for name, raw in contents.items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    result = {
        "schema": 1, "status": "REFERENCE_SOURCE_ABI_ADAPTED_ONLY",
        "upstream": {"repository": pin["repository"], "commit": commit, "license": pin["license"]},
        "profile": PROFILE,
        "private_cache_rechecked": capture is not None,
        "changes": ["Qualified _MTLDevice class extent guard: 536 -> 528 bytes",
            "Default MSL language 3.1 -> selected compiler's 3.0 cap",
            "Unknown selectors fail instead of receiving a generic object signature and zero/no-op result",
            "Bounded request13 encoder and 104-byte archive-envelope validator",
            "Unqualified A740 core-count query removed",
            "No forced Apple/common GPU-family answers or caller-specific family overrides",
            "XR10 scanout capability withheld for all clients until a matching display path exists"],
        "output_sha256": {n: hashlib.sha256(b).hexdigest() for n, b in contents.items()},
        "executable_built": False, "bundle_created": False, "publication_ready": False,
        "metal_acceleration_pass": False,
        "blockers": [
            "No A619 Darwin Vulkan BO, mapping, queue, fence or presentation provider is linked.",
            "No native XNU Vulkan 1.2 device has been observed; Android's driver is a separate implementation.",
            "No qualified gpuCoreCount query exists; required consumers will see an unsupported method, not the old A740 value8.",
            "Inherited private methods and strict runtime feature tables require an iOS 16.7 consumer audit.",
            "The upstream Rust bridge requires its matching translator/Turnip patch set, not arbitrary stock versions.",
            "Cross-process shared events and the IOSurface resource/fence ABI must work for system UI clients.",
            "No iPhoneOS SDK compile, device factory run or GPU workload has validated this adapted reference."
        ]
    }
    (out / "reference-readiness.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--capture", type=Path)
    mode.add_argument("--source-profile-only", action="store_true",
                      help="compile public sources from committed profile; no private-cache recheck")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = prepare(args.source, args.capture, args.out, profile_only=args.source_profile_only)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"FAIL: {exc}\n")
    print(json.dumps({"status": result["status"], "publication_ready": False}))


if __name__ == "__main__":
    main()
