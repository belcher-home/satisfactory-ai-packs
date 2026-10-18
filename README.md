# Satisfactory MKI Mods

<p align="center"><img src="media/midknight-industries-logo.png" alt="MidKnight Industries" width="240"></p>

Satisfactory mods by **MidKnight Industries**. Each release of a mod is one commit here, tagged with the mod and its version (for example `MKIRoads-v0.8.0`).

| Mod | Version | What it is | Source |
| --- | --- | --- | --- |
| **MKI Roads** (`MKIRoads`) | 0.8.0 | Asphalt roads laid like railway track: curves, grades, intersections, a turnaround, truck depots, road supports, lane markings and vehicle paths. Usable on its own. | [mods/MKIRoads](mods/MKIRoads/README.md) |
| **MKI Bridge** (`MKIBridge`) | 0.4.5 | A local, token-protected HTTP bridge that lets an AI assistant observe and build in your world. It requires MKI Roads. | [mods/MKIBridge](mods/MKIBridge/README.md) |

Players install the mods through [Satisfactory Mod Manager](https://ficsit.app). This repository is for people who want to read or build the code.

## Building

Both mods are SML 3.12 plugins for Satisfactory CL502094, built with Coffee Stain's Unreal Engine 5.6.1-CSS on Windows (Visual Studio 2022, MSVC 14.38). Set up the SML development checkout and its engine as the [SML modding documentation](https://docs.ficsit.app/) describes, then, from this repository:

```sh
npm install
npm run road-pack -- sync        # mirror both plugins into <SML_ROOT>/Mods
npm run road-pack -- editor      # compile both native modules for the SDK editor
npm run road-pack -- import      # generate and verify MKI Roads' Unreal content in a headless editor
npm run road-pack -- package     # Shipping archives for both plugins, verified
```

Set `SML_ROOT` and `UE_ROOT` when your checkout and engine are not at the defaults (`D:/SatisfactoryModding/SatisfactoryModLoader`, `D:/DevTools/UnrealEngine-CSS`). `package` refuses a dirty tree. `npm run road-pack:install -- install --game <Satisfactory root>` installs the packages it just built, with a backup, and `restore` undoes it. The [MKI Roads README](mods/MKIRoads/README.md#workflow) has every step in detail.

MKI Roads' Unreal content is a build output: the import step generates every asset from `mods/MKIRoads/modules.json` and the deterministic icons, so the repository holds no `.uasset` files and no game files.

The bridge also needs two small editor-created assets this repository does not hold yet (its GameFeatureData and an input mapping config). On a fresh SML checkout, create them in the editor before packaging the bridge.

## Tests and versions

`npm test` runs the tooling tests, which also check the plugin sources, descriptors and content manifest; CI runs them on Linux and Windows with `npm run versions -- check`. `npm run versions -- set roadPack 0.8.1` changes a mod's version in every place that states it. The native modules can only be compiled on Windows with the SML toolchain, so CI does not compile them.

## Layout

- `mods/MKIRoads`: MKI Roads' plugin source, its content manifest and icon golden, and the editor script that builds its content.
- `mods/MKIBridge`: MKI Bridge's plugin source.
- `tools/build-tools`: the build, packaging, install and versioning tools for the mods, and their tests. The MCP server that talks to the bridge is a separate project.

A new mod gets its own folder under `mods/`.

## Media

`media/` holds the MidKnight Industries logo and the images the mods' ficsit.app pages show.

## License

GPL-3.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). Both mods link Satisfactory Mod Loader, which is GPL-3.0.
