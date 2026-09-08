[ English | [Português](README_pt.md) ]

# SF3000HD-DeskTop

**SF3000HD-Desktop** is an add-on for [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI). It introduces an app named `myapp`, which acts as a full desktop environment for the handheld console.

## 📦 Included Applications

*   **`mywm`**: The window manager and desktop environment itself.
*   **`myterm`**: A custom terminal emulator built for the console.
*   **`mykeyboard`**: A virtual on-screen keyboard compatible with any app.
*   **`paint`**: A simple drawing simulation (currently in early stage, without color palette options).
*   **`demo`**: A gradient rendering test to benchmark the console's performance.

## 🛠️ Installation & Compilation

**For Users:**
Installation is straightforward. Download the files, extract them, and copy everything directly to the root of your SD card. Folders will automatically merge with your existing file structure.

**Compatibility:** the desktop is currently tested only with **TreeFrogUI 1.3.10** (tzubertowski's `treefrogui` branch) — it appears in the **Apps** tab of that version. Older cores (e.g. the frozen `master` branch) do not list standalone apps.

**For Developers:**
To compile this project you need the console's toolchain, the base libraries, and a `~/sf3000` working directory:
*   [SF3000 Toolchain v0.1](https://github.com/game-de-it/sf3000/releases/tag/sf3000_toolchain_v0.1) — extract it **inside your home folder** (`$HOME`) and run `relocate-sdk.sh` once; `build_all.sh` locates the compiler automatically
*   [SDL 1.2.15](https://github.com/libsdl-org/SDL-1.2) — classic SDL 1.2 API (not SDL2), cross-compiled **static** (`libSDL.a`)
*   [SDL_ttf 2.0.11](https://github.com/libsdl-org/SDL_ttf-2.0) (`libSDL_ttf.a`) and [SDL_image 1.2.12](https://github.com/libsdl-org/SDL_image-1.2) (`libSDL_image.a`)
*   [FreeType 2.10.4](https://github.com/bebrws/freetype-2.10.4) (`libfreetype.a`)
*   [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI) — reference for adding custom cores

   > Shortcut: if you cloned this repo, the static libraries are already bundled in `libs/` — just run `cp -r libs/* ~/sf3000/sdl/` instead of building them yourself.

Build steps (expected by `build_all.sh`):

1. Extract the toolchain anywhere under your `$HOME` (e.g. `~/mipsel-buildroot-linux-gnu_sdk-buildroot`) and run `relocate-sdk.sh` once — **mandatory**, without it the compiler won't start.
2. Create `~/sf3000/sdl/` with the static MIPS libraries in `lib/` (`libSDL.a`, `libSDL_ttf.a`, `libSDL_image.a`, `libfreetype.a`) and the headers in `include/SDL/` and `include/freetype2/`.
   > The toolchain only ships SDL as shared libraries (`.so`); the static `.a` files must be cross-compiled separately for MIPS.
3. Copy the sources — `src/*.c`, `src/*.h` and `src/build_all.sh` — to `~/sf3000/`.
4. Run `sh ~/sf3000/build_all.sh`. You should end up with `myapp` plus `demo`, `paint`, `myterm` and `mykeyboard`, all statically linked.
5. Copy everything to the SD card **all together** (the compositor expects its companions): `myapp` → `cubegm/desktop`, and the other four → `cubegm/wm/`.

*Note:* This project modifies one of TreeFrogUI's original cores. If you plan to add a custom core, check the existing core implementations to ensure proper communication with `mywm`.

## 📄 License & Credits

This project is licensed under **Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**. Feel free to open issues or submit pull requests.

Special thanks to [**tzubertowski**](https://github.com/tzubertowski) for creating and sharing the amazing TreeFrogUI project.