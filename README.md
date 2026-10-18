# ForzaInfinity

Forza Horizon 2's online services, back for the PC version running on **Azimuth 0.9.10**
(recommended) or **0.9.9**, on Windows, Linux and the Steam Deck.

This repository holds the source of **everything ForzaInfinity runs on your PC**, so you can
see exactly what you install:

| Folder | What it is | Where it runs |
|---|---|---|
| `hook/` | `azimuth-tls.dll`: Azimuth's TLS helper replaced by a fork of [ExtraOptions](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) that sends the game's own online requests to a ForzaInfinity server. C, Kernel32 only. | Inside the game, on your PC |
| `dedicated/` | `dedicated_server.exe`: the free roam world a player hosts on their own PC (a relay of the game's NerdCore protocol), with STUN, UPnP and hole punching. Rust, std only. | On your PC, while you host a world |
| `release/` | The installers, updaters, uninstallers and the report script. | On your PC, when you run them |

The ForzaInfinity server (`https://infinity.anarqz.dev`) is not part of this repository. What it
receives from your game is what the hook sends, which you can read in `hook/adapters/`.

## Play

Download **ForzaInfinity.zip** from the [latest release](https://github.com/anarqz/ForzaInfinity/releases/latest)
and run `Install.cmd` (Windows) or `install.sh` (Linux / Steam Deck), or in one line:

```sh
curl -fsSL https://github.com/anarqz/ForzaInfinity/releases/latest/download/install.sh | bash   # Linux / Steam Deck
```
```powershell
irm https://github.com/anarqz/ForzaInfinity/releases/latest/download/install.ps1 | iex          # Windows
```

Then start the game, open **Settings › INFINITY › CREATE PROFILE** and pick HARDCORE or NOOB.
Full guide: [docs/INSTALL.md](docs/INSTALL.md). Online free roam and races:
[docs/FREEROAM.md](docs/FREEROAM.md).

## Checking what you run

- Every release lists the SHA-256 of `azimuth-tls.dll` and `dedicated_server.exe`
  (`*.sha256`), and both are signed with the project's Ed25519 key (`*.sig`). The hook only
  installs an update whose signature checks out against the public key built into it
  (`hook/adapters/azimuth_sign.c`), and only runs the relay whose SHA-256 it carries
  (`hook/adapters/azimuth_relay_hash.h`).
- Build the hook yourself: see [hook/BUILD.md](hook/BUILD.md) (`python build.py <zig> --azimuth`).
- Build the relay yourself: `cd dedicated && ./build-windows.sh` (see
  [dedicated/README.md](dedicated/README.md)).

## Legal

ForzaInfinity is a fan project. It is not affiliated with, endorsed or sponsored by
Microsoft, Xbox, Playground Games or Turn 10 Studios. Forza Horizon and Xbox are trademarks of
their owners. You need your own copy of the game; nothing of the game is distributed here. The
hook is a fork of ExtraOptions and keeps its license ([hook/LICENSE](hook/LICENSE), GPL-3.0).
