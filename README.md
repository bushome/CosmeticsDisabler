```
 _____                          _   _              ⠀⠀⠀⠀⠀⠀⠀⠀⣀⣤⣴⣶⣶⣶⣶⣶⣶⣦⣤⣀
/  __ \                        | | (_)             ⠀⠀⠀⠀⠀⣠⢶⠿⠋⠉⠀⠀⠀⠀   ⠀ ⠀⠉⠙⠿⣶⣄
| /  \/ ___  ___ _ __ ___   ___| |_ _  ___ ___     ⠀⠀⠀⢠⣞⢿⣷⣄⠀⠀⠀⠀⠀⠀⠀⠀    ⠀⠀⠀⠈⠻⣷⡄
| |    / _ \/ __| '_ ` _ \ / _ \ __| |/ __/ __|    ⠀⠀⢠⡿⠃⠀ ⠙⢿⣷⣄⢤⣄⠀⠀⣠⣤⣄⠀⠀  ⠀⠀⠘⢿⡄
| \__/\ (_) \__ \ | | | | |  __/ |_| | (__\__ \    ⠀⢀⣿⠃⠀ ⠀⣀⣤⣝⢿⣷⣝⢦⣴⣿⣿⣿⣷⣤⣀⠀⠀ ⠘⣿⡀
 \____/\___/|___/_| |_| |_|\___|\__|_|\___|___/    ⠀⢸⣿⠀ ⠠⠾⠿⠿⠿⠷⠝⢿⣷⣝⠿⠿⠿⠿⠿⠿⠷⠄ ⠀⣿⡇
______ _           _     _                         ⠀⢸⣿⠀ ⠐⣶⣶⣶⣶⣶⣶⣶⣝⢿⣷⣔⢶⣶⣶⣶⣶⠂⠀ ⣿⡇
|  _  (_)         | |   | |                        ⠀⠈⣿⡄⠀ ⠈⠻⢿⣿⣿⣿⣿⣿⣷⣝⢿⣷⣝⠿⠟⠁⠀ ⢠⣿⠁
| | | |_ ___  __ _| |__ | | ___ _ __               ⠀⠀⠘⣷⡄⠀  ⠀⠀⠉⠙⠛⠛⠛⠛⠛⠓⠙⢿⣷⣄⠀ ⢠⣾⠃
| | | | / __|/ _` | '_ \| |/ _ \ '__|              ⠀⠀⠀⠘⢿⣦⡀⠀ ⠀⠀⠀⠀⠀⠀ ⠀ ⠀⠀ ⠀⠙⢿⣷⡽⠃
| |/ /| \__ \ (_| | |_) | |  __/ |                 ⠀⠀⠀⠀⠀⠙⠿⣶⣄⣀⠀⠀⠀⠀     ⠀⣀⣠⣶⠷⠋
|___/ |_|___/\__,_|_.__/|_|\___|_|                 ⠀⠀⠀⠀⠀⠀⠀⠀⠉⠛⠻⠿⠿⠿⠿⠿⠿⠿⠟⠛⠉
```

Server plugin that keeps applied custom cosmetics (structure skins, item skins, player hair/eyebrow/facial hair
cosmetics) from reaching clients, to stop the crashes that come with downloading and displaying them (console
players are hit hardest), plus an optional admin command to strip cosmetics off everything without removing the
item, structure or creature they are on.

**Status: 0.6.0.** Console players get no structure or item skins by default (PC players keep theirs; nothing on the
server changes); structure blocking is verified on Xbox Series X, Series S and PS5, item blocking so far with PC players
handled as consoles. Opt-in `/hideskins` for PC players (file or MariaDB storage shared across a cluster). Report-first,
backed-up structure strips for the whole map, one tribe (admin) or your own tribe (`/stripskins`), all undoable with
`Restore`. Item skin strips on entry and on load, an apply guard and an item data filter, all off by default. Player
hair / eyebrow / facial hair cosmetics are not handled yet.

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
