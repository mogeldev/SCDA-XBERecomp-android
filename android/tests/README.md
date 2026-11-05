# ZIP importer test

This is a plain-Java test for the path, CRC, ABI, cancellation, storage-preservation and atomic-publish checks:

```bat
mkdir out\import-tests
javac -d out\import-tests android\src\main\java\io\github\mogeldev\scda\GameImport.java android\tests\GameImportTest.java
java -cp out\import-tests io.github.mogeldev.scda.GameImportTest
```

It creates only temporary synthetic files; no game assets are included. To also extract and validate a private full game ZIP, append its path to the `java` command. Temporary extracted files are deleted afterward.

Test the PowerShell ZIP packager (including campaign-only filtering and failure preservation):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File android/tests/package-game-test.ps1
```

On a clean Android install, also manually test picker cancellation, import progress, Back/cancellation, a valid full import, automatic game launch and a second launch that skips setup. Do not clear an existing install's data without backing up saves/settings.
