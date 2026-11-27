# Upstream policy

## Remotes

| Remote | Repository | Role |
|---|---|---|
| `origin` | `printlndarling/hakios-thyme` | this integrated fork; all THYME work is pushed here |
| `upstream` | `oosexgang/hakios-xnu-kernel` | official common development |

## Branches and compatibility

Last checked on 2026-11-27.

| Upstream branch | Inspected tip | Use in this fork |
|---|---|---|
| [`main`](https://github.com/oosexgang/hakios-xnu-kernel/tree/63fc2b4633bac3af8e57dadab9f5a8d4cb940438) | `63fc2b46` | Ancestor of this fork through `d0c234cd`. Uses the XNU 8796.121.2 baseline. |
| [`common`](https://github.com/oosexgang/hakios-xnu-kernel/tree/62e09f850f1f713192228cb9031da256033ede93) (default) | `62e09f85` | Uses XNU 12377.121.6, Darwin 26/27 and a reorganized tree. Review individual implementations against the thyme XNU and loader interfaces. |

Check the related [`hakios-uefi`](https://github.com/oosexgang/hakios-uefi/tree/93e98ee8f8b9ca42f43dbbfd91b10566ba8d328c)
and [`hakios-sm7325`](https://github.com/oosexgang/hakios-sm7325/tree/817ec5a0a19e8ddafc91c47b8311fc300d8d7150)
ports for reference. Their boot images and board settings target their own
platforms. The `common` tip also changes the loader UFS service. Compare its
shared-memory ABI and block-geometry assumptions with the thyme loader before
considering a port.

## Reviewing updates

```sh
git fetch upstream main common
git log --oneline HEAD..upstream/main
git log -10 --oneline upstream/common
```

Keep this fork on XNU 8796.121.2 and the iOS 16.5 userspace. Review a candidate
against the local interfaces and the documented thyme fixes before applying
it. Record the upstream commit in the change, and check the board configuration
and build before testing on a phone.

```sh
tools/hakios-device.py check thyme
make thyme_defconfig
make config
make kernel JOBS=8
```

A new upstream revision requires successful RAM Boot on thyme before merging.
For a rendering change, exercise both the established OpenGL ES session and
the affected Metal path, and check UI, touch, charging, networking, audio
and thermal behavior. Use the [checklist](CHECKLIST.md) for feature-specific
hardware acceptance.

## Apple services compatibility review

The public [`common` tree at `62e09f85`](https://github.com/oosexgang/hakios-xnu-kernel/tree/62e09f850f1f713192228cb9031da256033ede93)
contains the account and store experiments introduced in [`7e43fbbf`](https://github.com/oosexgang/hakios-xnu-kernel/commit/7e43fbbf5381ce55f26bcd4f2215efda784af2fe).
The main experimental daemon integrations target Darwin 26/27. The
`components/apple-id/first-generation/` experiments target Darwin 16.5,
the generation used by thyme. The [upstream FAQ](https://hackios-web.vercel.app/en/faq)
still describes Apple account and App Store support as unavailable.

| Feature | Thyme status | Evidence needed |
|---|---|---|
| Apple ID | BLOCKED | Successful sign-in, 2FA, and token retention across reboot on thyme. |
| App Store | BLOCKED | Catalog, download, installation, and launch of an acquired app on thyme. |
| FairPlay | BLOCKED | Authorized installation and playback or execution of protected content. The stock Apple Secure Enclave is absent on thyme. |
| Stock WeatherKit | BLOCKED | Live Apple forecast and widget refresh on thyme. Apple's [public REST API](https://developer.apple.com/documentation/weatherkitrestapi/request-authentication-for-weatherkit-rest-api) requires a signed developer token and is available to apps that integrate it explicitly. |

The fork's optional Open-Meteo forecast screen is documented in the
[Weather bring-up notes](docs/weather-data-shim.md). Stock Weather and its
widget have their own verification gates. The upstream experiments are
not part of the daily RAM scene.
