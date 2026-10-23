# benilla patches

WOW-BG3's patches to [benilla](https://github.com/samwhosung/benilla), a World of Warcraft 1.12.1
client written in Rust, at commit `d3ac9f75bf946bf9896a01deceb1b6dd539ace64`.

They are applied in name order; later patches build on earlier ones. The numbers have gaps (0010,
0024, 0029, 0032, 0058): those patches were folded into others or dropped during development. Together they
add what WOW-BG3 needs from the WoW side: the camera and frame streams to BG3, world mode (BG3 leads,
WoW follows offscreen), BG3's character and effects drawn inside WoW, fights and stand-ins, the
party's bodies, minimap and interface changes, character creation, chat commands and the sniper
player build.

Build: `build.sh` (clones the pinned commit into `$BENILLA_SRC` when missing, applies the patches,
builds the player build for Steam Runtime 3). See [docs/build.md](../../docs/build.md).

## Licence

benilla is MIT OR Apache-2.0; these patches are under the same terms. `build.sh` is WOW-BG3's (MIT).
