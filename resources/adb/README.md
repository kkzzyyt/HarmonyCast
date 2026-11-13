# Bundled Android tools

Google Android SDK Platform Tools 37.0.1, downloaded on 2026-11-12:

| Host | Official archive | Archive SHA-256 |
| --- | --- | --- |
| macOS (universal x86_64/arm64) | https://dl.google.com/android/repository/platform-tools-latest-darwin.zip | `ee39ad5967e95c2a07f04dbcbde96b1a0c916ba376096db5d2f498b7727a5d1d` |
| Windows 10+ | https://dl.google.com/android/repository/platform-tools-latest-windows.zip | `45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99` |

Only ADB and its Windows DLL dependencies are included. Each host directory retains Google's `NOTICE.txt` and `source.properties`. ADB source is available from https://android.googlesource.com/platform/packages/modules/adb/.

`checksums.json` records the original files in this source checkout, including the pinned scrcpy server. Host code signing during packaging may change executable hashes; the server JAR is verified against its pinned hash at runtime.

The build keeps only the packaged host's ADB directory. The Server 2012 compatibility build omits the current Google ADB and searches for a compatible installed tool instead. `HARMONYCAST_ADB` overrides automatic tool discovery.
