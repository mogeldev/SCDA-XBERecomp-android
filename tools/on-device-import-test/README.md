# Isolated emulator import tests

Package **`io.github.mogeldev.scda.importtest`** exercises the exact local-setup `GameSource`, `GameImport` and installed native extraction tools. No game runtime, compiler, game source, XBE or media is embedded in this APK. No phone installation is permitted by `run.py`.

Supported local-setup inputs: retail original-Xbox ISO, XISO, extracted folder, ZIP, 7z, RAR/RAR5 and TAR (plain/gzip/bzip2/xz). Archives may contain a game folder or one ISO. Encrypted/split archives and archive-in-archive chains are outside scope. This product includes software developed by in <in@fishtank.com>.

Build and test (Windows; pass your SDK directory explicitly):

```text
python tools/on-device-import-test/build.py --sdk SDK
python tools/on-device-import-test/fixtures.py --xbe assets/default.xbe
python tools/on-device-import-test/matrix.py --sdk SDK --serial emulator-5554
python tools/on-device-import-test/run.py --sdk SDK --serial emulator-5554 --source PRIVATE.iso --reference PRIVATE_EXTRACTED_GAME
```

`fixtures.py` uses the supported private XBE, synthetic media/data, a tiny hand-generated XDVDFS image and stored RAR4 writer; fixtures remain ignored in `out/`. The host's installed 7-Zip creates 7z fixtures. Nothing from the user's downloads is committed. Use a disposable emulator with enough data storage (40 GiB was used for full retail/7z samples). Input transfer uses `adb push` to a unique temporary path, a private copy and SHA-256 verification, then removes the temporary path. Never run matrix/full-sample tests concurrently against the same package.

`run.py --cancel` interrupts when extraction begins, checks that no incomplete game is published and staging is removed. `--expect-failure` checks malformed/unsafe sources. `--reference` compares every imported campaign file SHA-256 against a private extracted folder. `--reuse-input` is for resuming an interrupted host harness only; hash verification still runs. Native subprocesses execute as the normal application UID from installed native-library paths, not ADB shell permissions.

## Results so far

- **30 emulator cases passed** in the expanded run (25 distinct cases plus five repeated unsafe-path fixtures): small XISO/retail-offset ISO, ZIP, 7z, stored RAR4, TAR, tar.gz, tar.bz2, tar.xz, ZIP containing ISO; invalid source, symlink, case collision, unsafe paths, malformed ISO names/NUL/extent, ambiguous game roots, unsupported XBE, encrypted 7z, and extraction-start cancellation. All rejected/cancelled inputs leave neither `game/` nor staging. Fixture generation now removes old unsafe fixtures for a repeatable 25-case matrix.
- Full private **4,055,040,000-byte XISO** imports successfully; only campaign assets are published. The **7,825,162,240-byte retail ISO**, **5,843,228,322-byte iso.7z** and **2,976,846,503-byte xiso.7z** each passed import and all **1,317 campaign file hashes matched** the private extracted reference. The 7z archives contain an ISO plus an ignored text file. Both image representations carry the same validated XBE; no broader game-build support is implied.
- Pinned upstream stored and compressed **RAR5** reader fixtures extracted under the app UID and matched 7-Zip output byte-for-byte. This supplements stored RAR4 full-import coverage; it is not an exhaustive RAR codec test.
- Shared importer: 132 synthetic ZIP checks still pass. `GameSourceTest`: 22 plain-Java checks. Two pinned-source hardening checks pass.
- ARM64 local-setup APK builds with both tools and import-license UI. The initial phone installation was blocked by `INSTALL_FAILED_USER_RESTRICTED`, but the authorized retry succeeded. The separate ARM64 local-setup app passed a full direct-XISO import, fresh 129-object build, load/dispatch check and automatic title-screen launch. **The user subsequently confirmed gameplay works after this direct-XISO import/build** (smoke confirmation, not exhaustive campaign/save-load testing). Its compiled/internal-loaded library matched the earlier gameplay-tested SHA-256 exactly; all 134 translator hashes matched Windows references. Compilation through publication took 381,659 ms, excluding import/translation. No phone archive-input or manual picker test ran, no phone campaign-file hash comparison was possible with the OEM external-storage restriction, and no phone settings were changed. Manual DocumentsProvider file/folder picker, large-game cancellation, process death during extraction, dictionary-memory exhaustion, low-storage stress, broader compressed RAR coverage and fuzzing remain outstanding.

A 1-GiB `RLIMIT_AS` experiment was removed: Android Scudo reserves very large virtual regions and aborted even a small ZIP. An early full-XISO attempt hit a transient Java directory-walk failure; no game was published. The retry passed; preservation of the original exception alongside cleanup errors was added. Neither event is evidence of successful low-storage/corruption testing.
