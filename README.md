# PuffyEngine

PuffyEngine is a tiny (< 400 KB) single-file C++ launcher that embeds a Chromium-based Steam HTMLSurface browser in a native Windows window and serves your web game from a local HTTP server.  
Created by **@coeurnix**, licensed under the **Boost Software License 1.0**.

> **Status:** Windows only • Steam required and must be running

---

## How it works

- Starts a local HTTP server on `127.0.0.1:<port>` (default `19996`).
- Serves static files from a **`/game` directory** (default `./game/` next to the executable).
- Opens a Chromium-based Steam HTMLSurface pointed at that server.
- Forwards keyboard, mouse, and XInput gamepad to the web page.
- Exposes Steam features (achievements, stats, cloud saves) to JavaScript via a simple alert-based bridge.

Because it uses Steam’s Chromium layer, you get many modern browser features (HTML5, CSS, JavaScript, audio/video, etc.) without shipping a big runtime yourself.

---

## Project layout

Typical layout:

```text
PuffyEngine.exe
/game
  index.html
  main.js
  assets/...
````

You can override the game folder and other settings:

* `--puffyport <port>` – HTTP port (default: `19996`)
* `--puffydir <path>` – game folder (default: `./game/`)
* `--puffywidth <pixels>` – window width (default: ~80% of screen)
* `--puffyheight <pixels>` – window height (default: ~80% of screen)

---

## JavaScript bridge (callbacks)

PuffyEngine listens to `alert()` messages that start with `puffy:` and interprets them as engine commands.

Basic examples:

```js
// Exit the game
alert('puffy:exit');

// Toggle fullscreen
alert('puffy:setFullscreen(true)');
alert('puffy:setFullscreen(false)');

// Show/hide cursor
alert('puffy:setCursorVisible(false)');
alert('puffy:setCursorVisible(true)');

// Achievements and stats
alert('puffy:grantSteamAchievement("ACH_WIN_ONE_GAME")');
alert('puffy:grantSteamStat("total_deaths", 1)');
alert('puffy:grantSteamStat("playtime_hours", 0.5)');
```

Steam Cloud helpers:

```js
// Load a file from Steam Cloud, then receive it in a callback
function onCloudLoaded(text) {
  if (text === null) {
    // no save found
    return;
  }
  // parse and apply save data
}
alert('puffy:requestSteamCloudSave("save.json","onCloudLoaded")');

// Save text data
alert('puffy:saveToSteamCloudSave("save.json","some serialized data")');

// Delete a cloud save
alert('puffy:removeSteamCloudSave("save.json")');
```

All these commands are transported via `alert("puffy:...")`, parsed natively, and never shown as real UI dialogs.

---

## Building

You need a Windows toolchain, Steamworks SDK (for `steam_api64.lib` and headers), and the usual Windows libraries (linked in the source).

### Easiest (Windows)

From a Developer Command Prompt:

```bat
build.bat
```

### With CMake

Using the provided `CMakeFiles.txt`:

```bash
cmake -B build -S .
cmake --build build --config Release
```

Copy the resulting executable next to a `/game` folder and run it while Steam is running.

---

## License

This project is released under the **Boost Software License 1.0**.
