```
 _____                          _   _
/  __ \                        | | (_)
| /  \/ ___  ___ _ __ ___   ___| |_ _  ___ ___
| |    / _ \/ __| '_ ` _ \ / _ \ __| |/ __/ __|
| \__/\ (_) \__ \ | | | | |  __/ |_| | (__\__ \        .---.
 \____/\___/|___/_| |_| |_|\___|\__|_|\___|___/      .' _/  '.
______ _           _     _                          /  /  []  \
|  _  (_)         | |   | |                        |  /        |
| | | |_ ___  __ _| |__ | | ___ _ __                \/        /
| | | | / __|/ _` | '_ \| |/ _ \ '__|                '._    .'
| |/ /| \__ \ (_| | |_) | |  __/ |                      '---'
|___/ |_|___/\__,_|_.__/|_|\___|_|
```

Server plugin that keeps applied custom cosmetics (structure skins, item skins, player hair/eyebrow/facial hair
cosmetics) from reaching clients, to stop the crashes that come with downloading and displaying them (console
players are hit hardest), plus an optional admin command to strip cosmetics off everything without removing the
item, structure or creature they are on.

**Status: 0.5 (test servers only).** Probe logging of what cosmetic data goes to clients; a dry-run-first, backed-up
Strip / Restore for structure skins (off until `EnableStrip` is set); item skin strips on entry (transmitter, vault,
obelisk, drops) and on load from the save, with a backup file; an apply guard for structures and items; and an
item data filter. Player hair / eyebrow / facial hair cosmetics are not handled yet.

| Folder | Game | Server API (submodule) |
|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` |

ASA only: cosmetics exist only in ASA, so there is no ASE version.

The folder is a self-contained Visual Studio solution. Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/CosmeticsDisabler.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

## License

MIT, see [LICENSE](LICENSE). Third-party code compiled into the plugin DLL is listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
