# MediEvil (PS4) for Linux/Steam Deck

**MediEvil (PS4, 2019) running natively on Linux, forked by spacepepperoni from [bc160201983](https://github.com/bc160201983/medievil_2019_pc_port).**

**This client is a launcher that starts MediEvil, adds it to your desktop or adds it to your Steam Library. This fork features auto-updates and a Steam Deck mode that optimizes the game for the Steam Deck.**
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/6e044f3a-31e0-40d8-b56f-71134565899f" />

## Download
1. Download **[MediEvil-x86_64.AppImage](https://github.com/spacepepperoni/medievil_ps4_Linux/releases/latest/download/MediEvil-x86_64.AppImage)**.
2. Right-click it, open **Properties → Permissions** and tick **Is executable** (or
   **Allow executing file as program**).

## Updates

The launcher has a built-in updater that can be toggled on/off in Advanced.

- If opening the launcher itself, available updates show in the sidebar.
- If launching through Steam or Game Mode (Steam Deck), the update downloads in the background and is used the next time you start the game.

## Steam Deck
- Install the [AppImage](https://github.com/spacepepperoni/medievil_ps4_Linux/releases/latest/download/MediEvil-x86_64.AppImage), open it once in Desktop Mode and choose your game folder, then press **Advanced → Add to Steam** to play in Game Mode.
- In the Display tab, Steam Deck mode is auto-selected if you are on SteamOS: fullscreen, 720p, Low graphics and a 60 or 30 FPS cap.

## Files needed

You must have your own copy of MediEvil (PS4, CUSA11227 or the European CUSA12982), .pkg is also supported.

Point the launcher to it in the Game tab. Then Play, Add to Steam (recommended for Steam Deck) or create a Desktop shortcut.

> **No game files are included.** This project is not affiliated with Sony Interactive
> Entertainment or Other Ocean.

## System Requirements

- 64-bit x86 Linux with glibc 2.41 or newer: SteamOS 3.7+, Debian 13, Ubuntu 25.04, Fedora 42,
  Arch, or anything newer.
- A graphics card with Vulkan 1.3.

## Credits

Built on [bc160201983's MediEvil PC port](https://github.com/bc160201983/medievil_2019_pc_port), its Linux build by nydal91, and [FelipePrado0's gow3_pc](https://github.com/FelipePrado0/gow3_pc) (from Supermedo and deadinside28's bloodborne_pc), with the [shadPS4](https://github.com/shadps4-emu/shadPS4) renderer.

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.
