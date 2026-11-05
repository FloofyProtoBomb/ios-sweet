# XNU 8796 DriverKit output replay

The pinned DRE kernel uses the DriverKit interfaces produced by Xcode 15.0.1
for XNU 8796.141.3. Newer Apple source uses a different kernel `IORPC` envelope
with a content pointer supplied by the request and trap entry paths. The
replay tool preserves the old generated C++ and headers so a newer compiler
can build them with the matching old kernel sources.

`profile.json` pins the original GitHub Actions archive, the tar member inside
it, the 17 generated pairs, and a digest of the generator input inventory.
The archive comes from run 42090341731 at source 2201e807 and contains output
from DriverKit-286 with macOS SDK 14.0. Keep the downloaded ZIP and extracted
files in build storage. The ZIP digest must match even if the artifact must
be recovered from a saved copy after Actions retention expires.

Prepare a fresh cache after verifying the pinned archive. Point XNU's existing
`IIG` override at the executable replay script and export the four absolute
paths for the actual build.

```sh
python3 tools/dre-iig-replay/replay.py prepare \
    --artifact "$DRE_IIG_ARTIFACT" --cache "$RUNNER_TEMP/dre-iig"
export DRE_IIG_REPLAY_CACHE="$RUNNER_TEMP/dre-iig"
export DRE_IIG_REPLAY_SRCROOT="$PWD/dxb/xnu"
export DRE_IIG_REPLAY_OBJROOT="$PWD/dxb/build/xnu.obj"
export DRE_IIG_REPLAY_SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export IIG="$PWD/tools/dre-iig-replay/replay.py"
```

Each call checks the interface, output basenames, ordered include roots,
compiler SDK identity, and historical preprocessor arguments. The source
inventory includes the interface definitions, ABI and generation anchors,
and all repository candidates for recursively discovered literal includes
and include probes across the original search roots. It follows both
conditional branches and catches a newly introduced shadow header. The two
early generated BSD headers are also checked by digest. This conservative
inventory may reject a change in an inactive branch. An unknown include
form is a refusal.

The original SDK's full preprocessed input is unavailable in the archive.
The old SDK and generator identify the archived outputs. The macOS 15.5
SDK is a separate, explicit compiler input for the new build. The script
never asks either SDK to regenerate the interfaces. Both requested output
digests and any existing destinations must match before the pair is written.

Run the local synthetic guard tests with
`python3 tools/dre-iig-replay/test_replay.py`. Qualification on Xcode 16.4
still requires compilation of the generated sources and their kernel
consumers, followed by the full link and VM checks.
