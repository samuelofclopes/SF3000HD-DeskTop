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

**For Developers:**
To compile this project, you must use the specific toolchain for this console alongside the required dependencies:
*   [SF3000 Toolchain v0.1](https://github.com/game-de-it/sf3000/releases/tag/sf3000_toolchain_v0.1)
*   [SDL](https://github.com/libsdl-org/SDL)
*   [FreeType (2.10.4)](https://github.com/bebrws/freetype-2.10.4)
*   [TreeFrogUI](https://github.com/tzubertowski/TreeFrogUI)

*Note:* This project modifies one of TreeFrogUI's original cores. If you plan to add a custom core, check the existing core implementations to ensure proper communication with `mywm`.

## 📄 License & Credits

This project is licensed under **Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**. Feel free to open issues or submit pull requests.

Special thanks to [**tzubertowski**](https://github.com/tzubertowski) for creating and sharing the amazing TreeFrogUI project.