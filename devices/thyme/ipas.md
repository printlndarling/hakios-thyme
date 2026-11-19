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
| 知乎减减 (Zhihu client) | `com.huamu013.ZhihuMinusMinus` | 0.9.10 | `https://github.com/huamurui/zhihu-minus-minus` release, unsigned IPA | not recorded at download time (2026-11-18, earlier session) | to verify (repository licence not recorded) |
| Aidoku (manga reader) | `app.aidoku.Aidoku` | 0.9 | `https://github.com/Aidoku/Aidoku` release, IPA | not recorded at download time (2026-11-18, earlier session) | to verify (repository licence not recorded) |
| PPSSPP (PSP emulator) | `org.ppsspp.ppsspp` | 1.20.4 | `https://github.com/hrydgard/ppsspp` iOS release, IPA | not recorded at download time (2026-11-18) | GPL-2.0-or-later (PPSSPP) |
| iTorrent (torrent client) | `com.xitrix.iTorrent2` | 2.2.0 | `https://github.com/XITRIX/iTorrent/releases/download/v2.2.0-1/iTorrent.ipa` | `50457a89299bd62836ca1eb1ae0a5adbe097454c58ce67605bbe6710c37193d7` | MIT (repository) |
| LocProbe (this project's probe) | `dev.hakios.LocProbe` | 1.1 | built here (`tools/ipa.py`) | not recorded (rebuilt each time) | MIT (this project) |

**One app was rejected before staging**: Yattee 2.0.0 beta 278 (`stream.yattee.app`, the open-source
YouTube frontend, GitHub IPA) declares `minOSVersion` **18.0**; this port's system is iOS 16.x, so it
cannot be installed or started here, and it was not put on the LAN. It is a candidate again only if the
port moves to a newer system image.

Two facts about the store itself:

- TrollApps (the client) is installed from `TrollApps.ipa` on the same LAN directory,
  itself the official release (`TheResonanceTeam/TrollApps`); TrollStore's helper is
  installed from the app the port ships in `/Applications` (TrollStore 2.1.1).
- `repo-hakios.json` lists the four current apps with the row order the scenes tap:
  贵州天气, 知乎减减, Aidoku, LocProbe.

## What a scene cannot check yet

`hk_download` now takes the expected digest as an optional third word: it hashes what it
fetched, prints the comparison, and returns 4 on a mismatch, so a scene stops before handing
a wrong or truncated file to TrollStore's helper. The store scenes pass TrollApps' digest
(`8eb32fb3…c01e`). The two apps the
store downloads itself (贵州天气, the rest of the catalog) are not covered: TrollApps fetches
those, so their digests above are compared by an operator, or by a scene that downloads a
copy of its own first. A shell-level check is not possible on the phone -- the Procursus
bootstrap in `jb/debs/bootstrap.tar.zst` carries no `sha256sum`/`sha1sum`/`shasum` (checked
in the archive).

## The map intake (`rc.jobs.mapdiag`, 2026-11-20)

Alpimaps was removed from the image, the LAN directory and the scene set at the operator's
request on 2026-11-20; its evidence stays in `CHECKLIST.md` as the port's record, but no
build or run installs it any more. In its place the map slot is an intake that installs
whatever the operator stages:

- `tools/map-run.sh APP.ipa` reads `Payload/*.app/Info.plist` and the executable's
  `LC_ENCRYPTION_INFO` itself. A **cryptid 1** executable is refused on the host: it is a
  FairPlay App Store image, and this port has no Apple key to decrypt it (an installation of
  one would prove only that the helper copies files). A **cryptid 0** IPA -- self-built,
  unsigned or lawfully decrypted by its owner -- is staged as `target.ipa`, with
  `target.txt` carrying name, bundle id, version, the .app directory name, SHA-256 and the
  source note.
- `rc.jobs.mapdiag` + `userland/bin/hk-mapipa.sh` fetch both over the LAN, verify the digest
  with `hk_download` (mismatch returns 4 and nothing is installed), install through
  TrollStore's helper, `uicache -a`, `uiopen --bundleid`, then print the new container and
  the crash-report directory to `/private/var/tmp/mapipa.txt`.
- The LAN server runs on the Windows side (`tools/ipas-server.ps1`), because the phone
  reaches `192.168.0.104` and not WSL2's `172.x` address.

This is the path 高德地图 (`com.autonavi.amap`) and 百度地图 (`com.baidu.map`) take.
Neither is on the LAN as a download: both are App Store-only, their store images are
FairPlay-encrypted, and no decrypted copy is downloaded here.

**百度地图 was staged from the operator on 2026-11-20** ("百度地图-路线规划出行必备.ipa",
22.2.20, `com.baidu.map`, app directory `IphoneCom.app`, 207,202,418 bytes, SHA-256
`299ea64e50d1841499b64822eae236ea0fed7323fea88010fe7a6ea20363ac43`, `cryptid 0` -- the
operator's own copy, lawfully decrypted; this project neither downloaded it from a
distribution site nor asks for an Apple ID). It installs, registers and launches on the port
(image `65abdfef`): the screen shows its full UI -- search bar, layer and feedback buttons,
tab bar, ads, the location dot -- with the map canvas black and flickering. The log shows one
refusal repeated, its 1125x2436 `Depth32Float` depth texture asking for usage `0x7`
(read, write, render target) and being turned down ("not stored"); the map renderer treats
the failed attachment as the frame's, so the map never draws. 高德地图 remains untested:
no lawfully decrypted copy of it was provided.
