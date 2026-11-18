# The IPAs the store serves, and where each came from

The curated LAN source (`repo-hakios.json`, served beside these files at
`http://192.168.0.104:8000/`) carries only apps whose developer publishes an unsigned
build. Nothing here is a decrypted App Store image: every IPA below was checked for
FairPlay (`LC_ENCRYPTION_INFO cryptid == 0`), arm64 and a declared minimum OS before it
was put on the LAN.

The LAN directory is not in the repository (the IPAs are 5-62 MB). This page is the
record: what each app is, where it came from, what it weighs, and its digest at the time
of download. A scene that installs one trusts the LAN copy; re-check a digest with
`sha256sum` after replacing a file.

| App | Bundle id | Version | Source (developer's release) | SHA-256 of the IPA | Licence |
| --- | --- | --- | --- | --- | --- |
| 贵州天气 (weather) | `cn.guizhou.weathergz` | 3.2 | `https://github.com/wangvizi/WeatherGZ/releases/download/v3.2/WeatherGZ.ipa` | `e3777b8288f165e8843437087177b5187b971c373e22ea0bf8ba163000e71c25` | none declared in the repository (source published, no licence file) |
| Alpimaps (map) | `akylas.alpi.maps` | 3.0.4 build 242 | `https://github.com/Akylas/alpimaps/releases/download/ios/alpha/3.0.4/242/alpimaps.ipa` | `566c5f522d92c4e6ef8df053ae53aa6f5d436f01f9290bbcb8daa1a9232c4f94` | MIT (repository licence; the IPA's dependencies carry their own) |
| 知乎减减 (Zhihu client) | `com.huamu013.ZhihuMinusMinus` | 0.9.10 | `https://github.com/huamurui/zhihu-minus-minus` release, unsigned IPA | not recorded at download time (2026-11-18, earlier session) | to verify (repository licence not recorded) |
| Aidoku (manga reader) | `app.aidoku.Aidoku` | 0.9 | `https://github.com/Aidoku/Aidoku` release, IPA | not recorded at download time (2026-11-18, earlier session) | to verify (repository licence not recorded) |
| PPSSPP (PSP emulator) | `org.ppsspp.ppsspp` | 1.20.4 | `https://github.com/hrydgard/ppsspp` iOS release, IPA | not recorded at download time (2026-11-18) | GPL-2.0-or-later (PPSSPP) |
| LocProbe (this project's probe) | `dev.hakios.LocProbe` | 1.1 | built here (`tools/ipa.py`) | not recorded (rebuilt each time) | MIT (this project) |

Two facts about the store itself:

- TrollApps (the client) is installed from `TrollApps.ipa` on the same LAN directory,
  itself the official release (`TheResonanceTeam/TrollApps`); TrollStore's helper is
  installed from the app the port ships in `/Applications` (TrollStore 2.1.1).
- `repo-hakios.json` lists the five current apps with the row order the scenes tap:
  贵州天气, Alpimaps, 知乎减减, Aidoku, LocProbe.

## What a scene cannot check yet

`hk_download` now takes the expected digest as an optional third word: it hashes what it
fetched, prints the comparison, and returns 4 on a mismatch, so a scene stops before handing
a wrong or truncated file to TrollStore's helper. The store scenes pass TrollApps' digest
(`8eb32fb3…c01e`) and the Alpimaps scenes pass Alpimaps' (`566c5f52…4f94`). The two apps the
store downloads itself (贵州天气, the rest of the catalog) are not covered: TrollApps fetches
those, so their digests above are compared by an operator, or by a scene that downloads a
copy of its own first. A shell-level check is not possible on the phone -- the Procursus
bootstrap in `jb/debs/bootstrap.tar.zst` carries no `sha256sum`/`sha1sum`/`shasum` (checked
in the archive).
