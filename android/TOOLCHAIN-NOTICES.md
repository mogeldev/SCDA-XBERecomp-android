# Bundled on-device tools

Production ARM64 builds include the compiler and import tools used during first-time setup. No game executable, media, generated game sources/objects or compiled game library is included. Native tools run as the app UID; they are not a sandbox for hostile inputs.

- LLVM/Clang/LLD 21.1.8: Apache-2.0 with LLVM exceptions; pinned Termux packages and SHA-256 values are in `tools/on-device-probe/packages-arm64-v8a.json`.
- NDK LLVM libunwind: Apache-2.0 with LLVM exceptions; the ARM64 static archive is bundled for linking player-built game libraries on Android 10. The packaged LLVM notices cover it.
- Termux libc++ runtime: renamed `libc++_compil.so` to avoid colliding with the game's NDK `libc++_shared.so`.
- libffi, libxml2, zlib, zstd: their upstream notices are extracted from the pinned packages into `assets/licenses/`.
- GNU libiconv: LGPL; this is a separately linked shared library, not incorporated into project source. Preserve the bundled copyright/license texts. Corresponding source and Termux packaging/patches for the shipped version are provided with the testing release. Upstream: https://www.gnu.org/software/libiconv/ ; package recipe: https://github.com/termux/termux-packages/tree/master/packages/libiconv .
- extract-xiso: copyright in@fishtank.com, modified BSD license. This product includes software developed by in <in@fishtank.com>.
- libarchive, XZ/liblzma and bzip2: pinned sources in `tools/on-device-setup/extract-xiso/CMakeLists.txt`; bundled upstream notices retain their respective licenses. Only liblzma is linked, not XZ's GPL shell scripts/tools.
- The XBE translator also uses Zydis/Zycore (MIT) and tomlplusplus (MIT).

The setup screen opens the packaged notices. Corresponding source/build materials for the bundled compiler/toolchain and LGPL components are supplied with the testing release. This is not a completed security audit or a stable release; production upgrade/manual-picker validation, low-storage stress and testing on 16-KiB page devices remain outstanding.
