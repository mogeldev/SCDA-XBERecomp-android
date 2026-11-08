# Turnip v4.5 source / notice record

The unmodified ARM64 universal binary is pinned and verified by `stage.py`. The upstream binary contains `Mesa 27.0.0-devel (git-02f1337523)`, consistent with its release notes. This is upstream-provided binary provenance, **not an independently reproduced byte-identical build**.

The 0.2.0-testing.1 corresponding-source ZIP adds these full archives to the existing compiler/libiconv source bundle:

| Archive | Revision | SHA-256 |
|---|---|---|
| mesa-source.tar.gz | 02f13375231bf0a9ed20b09cb21171351053be52 | ef58b99e11c7d67bcc2d3faf12a5711742a14450c292515c2890559ef957e070 |
| vauzi-source.tar.gz | dd75952b6d3df5c31d9bef3adbda0577697a16b2 (4.5) | 7cb2544d38ce7d73fdef00b55d68cde232e18cbc910772eb82049bc8e1911524 |
| libadrenotools-source.tar.gz | 8fae8ce254dfc1344527e05301e43f37dea2df80 | ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb |
| liblinkernsbypass-source.tar.gz | aa3975893d83ef1bc84c321ec60c65fbf1287887 | da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638 |
| volk-source.tar.gz | 0b17a763ba5643e32da1b2152f8140461b3b7345 | 4befdf3b194bf39ad325391611e29da51bb0dfbe7fda028d3dfa1d837d48c2fb |

Mesa archive retrieved from the `chaotic-cx/mesa-mirror` GitHub mirror at the upstream SHA. GPU-table bytes match the independently pinned file hash in `gen_supported.py`.

## Reconstructing sources / build

Extract the Mesa and Vauzi archives. The universal Android variant applies only `patches/01-u_gralloc-always-use-ubwc-detection.patch`. Do not apply glibc or SW/WIN variant patches. The archived `turnip_builder.sh` is the upstream build recipe. On a Linux build host with its documented dependencies, select `BUILD_VERSION=4.5`, `MESA_REF=02f13375231bf0a9ed20b09cb21171351053be52`, `BUILD_TARGET=android`, `BUILD_VARIANTS=base`; NDK r29. Alternatively initialize the unpacked Mesa source as a local Git repository and point `MESA_REPO` and `MESA_REF` to it. The recipe fetches Mesa and applies the patch itself; do not pre-apply it when using the recipe.

The source archive retains Meson wraps/build files. Driver binary has only Android system NEEDED dependencies: libhardware.so, liblog.so, libnativewindow.so, libsync.so, libm.so, libz.so, libdl.so, libc.so. No libstdc++, private game library or host library is embedded in this package. Build-machine/tool versions beyond the upstream metadata are not asserted to be identical.

For loaders, place the liblinkernsbypass source inside libadrenotools `lib/linkernsbypass/`. Repository CMake pins all sources; standard CMake FetchContent source overrides can point to these extracted copies. Project source and Android build recipes are supplied by the immutable release tag and included project-source archive in the bundle.

## Notices

Mesa is per-file licensed, primarily MIT. All upstream `licenses/` texts plus source-header notices (a conservative superset, not a claim every component is linked) are packaged in APK assets. The Vauzi recipe repository is GPL-3.0; its license and complete source/patch archive accompany distribution. libadrenotools/linkernsbypass are BSD-2-Clause; volk is MIT. Runtime SDL/glslang/Opus/ImGui/DirectX-Headers notices are also included. This record does not claim an independent legal opinion or full binary reproducibility.
