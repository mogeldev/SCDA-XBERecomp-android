# Manual device report (0.2.0-testing.2)

Launcher: Share device report → preview → Android Share or Review on GitHub. No upload client or background transmission. GitHub opens an editable issue draft; submission is the player's action. Warning explains public visibility and account association.

Automatically included: app version, manufacturer/model, Android/API, runtime ABI, selected driver, Turnip eligibility and the last recorded successful Vulkan-device initialization (GPU, Vulkan/driver version, loaded driver, timestamp). The local internal-storage snapshot is replaced atomically at initialization and is ignored if it predates the last app install/update. It does not prove game startup or gameplay. No identifiers, accounts, paths, logs, game files or saves are collected. Optional notes are user-provided, limited to 600 characters and not persisted by this UI.

Tests: `python -B -m unittest discover -s android/tests -p 'test_*.py'`. Static guards verify the manual-only intent boundary and allowlisted snapshot fields; these are not end-to-end UI tests.

Android 15 x86_64 emulator (pre-bump build): not-recorded state before first start, GPU and Manufacturer recorded after a game start, Cancel returns to launcher, Share opens the system chooser, Review on GitHub opens a browser draft (not submitted).

Manual checks still required on physical hardware:
- Before first game startup: report says not recorded, no guessed loaded driver.
- After game startup: GPU/loaded driver match log; selecting another driver without restarting changes only the selected field.
- Cancel does not open another app; Share opens chooser; GitHub opens editable draft with encoded body, no automatic submission.
- Verify notes/result, full preview readability/scrolling, large font and no-browser/share-target error.
- After an app update, stale startup data is not attributed to the new build.
