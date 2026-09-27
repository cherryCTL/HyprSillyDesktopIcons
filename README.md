# HyprSillyDesktopIcons

> A modified and updated fork of [Oh My Desktop](https://github.com/EduuG/oh-my-desktop), renamed and adapted as [**HyprSillyDesktopIcons**](https://github.com/cherryCTL/HyprSillyDesktopIcons).

> This version was updated for my personal Hyprland setup, mainly to fix compatibility issues with newer Hyprland versions and update parts of the original project that no longer worked correctly.
>
> I didn't originally know C++, so I used AI assistance to help me understand, debug, and update the code. Feel free to read through the code and review the changes before using it on your own system.

<a href="screenshot.png"><img src="screenshot.png" alt="Screenshot" width="800"></a>

> **So, you might be wondering: what's so silly about this?**
>
> Honestly, nothing.
>
> **That's what makes it silly. :D**

Desktop icons manager for Hyprland. Hyprland has no built-in desktop icon support, so this fills that gap.

Built with C++17 and Qt6 + LayerShellQt, it provides a proper desktop with drag-and-drop, keyboard navigation, multi-monitor support, and file watching.

<p>
<a href="https://github.com/cherryCTL/HyprSillyDesktopIcons"><img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white" alt="C++17"></a>
<a href="https://github.com/cherryCTL/HyprSillyDesktopIcons"><img src="https://img.shields.io/badge/Qt-6-41CD52?logo=qt&logoColor=white" alt="Qt6"></a>
<a href="https://github.com/cherryCTL/HyprSillyDesktopIcons"><img src="https://img.shields.io/badge/Wayland-LayerShell-8054B8?logo=linux&logoColor=white" alt="Wayland LayerShell"></a>
<a href="https://github.com/cherryCTL/HyprSillyDesktopIcons"><img src="https://img.shields.io/badge/CMake-3.20+-064F8C?logo=cmake&logoColor=white" alt="CMake"></a>
<a href="https://github.com/cherryCTL/HyprSillyDesktopIcons"><img src="https://img.shields.io/badge/Linux-x86__64-E95420?logo=linux&logoColor=white" alt="Linux"></a>
</p>

## Tech Stack

* **Language:** C++17
* **Framework:** Qt6 (QGraphicsScene-based icon management)
* **Wayland:** LayerShellQt for layer-surface windows
* **Compositor integration:** Hyprland IPC via socket2
* **Build:** CMake 3.20+

## Build

```bash
cd HyprSillyDesktopIcons
rm -rf build
mkdir build
cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Requires Qt6 and LayerShellQt development packages.

## Runtime Dependencies

* `hyprctl`
* `gio`
* `xdg-mime`
* `xdg-open`
* A system terminal

## Run

### Hyprland Lua

For Hyprland configurations using `hyprland.lua`:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("/full/path/to/HyprSillyDesktopIcons/build/HyprSillyDesktopIcons")
end)
```

Or, if the binary is available in your `PATH`:

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("HyprSillyDesktopIcons")
end)
```

`hl.exec_cmd()` starts the process asynchronously, so no `&` or `disown` is required.

## Features

### Core

* Grid layout with manual positioning and Ctrl+multi-select
* Drag icons to move them — they snap to the nearest free cell
* Ctrl+scroll to resize icons (24–96px range)
* Image previews for common formats
* `.desktop` files use `Name=` and `Icon=` from the entry
* Pin applications to the desktop via the Properties dialog
* Positions and settings saved to `~/.config/HyprSillyDesktopIcons/icon-positions.ini`

### File Management

* Drag files onto the Recycle Bin or onto folders
* Smart text file opening
* File watcher automatically refreshes when the Desktop folder changes
* Recycle Bin accepts file drops and has an "Empty Recycle Bin" action

### Keyboard

* Arrow keys to navigate
* Enter to open
* Delete to trash
* Type-to-select files by name
* Full keyboard navigation without a mouse

### Context Menus

* Right-click the desktop for:

  * New Folder / Document
  * Open in Terminal
  * Arrange
  * System actions
  * Properties
* Right-click icons for:

  * Open
  * Rename
  * Delete
  * Properties
* Optional virtual icons:

  * Recycle Bin
  * My Computer
  * My Documents

### Wayland Integration

* Workspace switches clear menus and selection through Hyprland socket2
* Each monitor gets its own LayerShellQt window at `LayerBackground`
* Automatically accounts for panel areas when positioning the desktop

## How It Works

### LayerShellQt Windows

One desktop window is created for each monitor and rendered as a background layer surface. This keeps the desktop visible while placing it behind normal application windows.

When a new monitor is connected, a new desktop window is created automatically.

### Inline Menus

Standard `QMenu` popups can be unreliable when used from Wayland layer-shell surfaces.

HyprSillyDesktopIcons uses custom inline menus instead. These are regular child widgets positioned around the cursor, avoiding the popup grab issues that can occur with native menus on Wayland.

### Hyprland IPC

The application communicates with Hyprland at runtime rather than depending on Hyprland headers or compile-time integration.

It uses:

* `hyprctl` for monitor information
* Hyprland's socket2 IPC for workspace and focus events

If Hyprland IPC is unavailable, the desktop icon functionality can still operate, although workspace-aware behavior will be unavailable.

### Grid Layout

The desktop grid handles:

* Manual icon positioning
* Drag-and-drop
* Cell occupation
* Multi-selection
* Icon resizing
* Automatic snapping to available cells

## Notes

This project is mainly maintained for my personal Hyprland setup and experimentation.

If you like this project, consider giving the original author a ⭐ on [Oh My Desktop](https://github.com/EduuG/oh-my-desktop). It's genuinely a really good project, and this project wouldn't exist without it.

---

**What's so silly about it?**

Nothing.

**That's the whole point. :D**
