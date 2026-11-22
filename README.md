# nocturne-recomp

A static recompilation of Terminal Reality's Nocturne (1999) for Linux and Windows. The build translates the original `nocturne.exe` to C++ and runs it on native replacements for the Windows, DirectX, and renderer calls it makes. The game renders its 3D at your display's resolution, plays sound through SDL3, and shows a textured 3D view of the level when you wear the night-vision goggles.

The repository contains no game data. You need your own copy of the game on its two original CDs.

## What you need

- ISO images of both Nocturne discs. To make an image of a disc on Linux, run `dd if=/dev/sr0 of=nocturne1.iso bs=2048` with the disc in the drive. On Windows, use any tool that saves a disc as an `.iso` file.
- Python 3.8 or newer, for the setup script.
- A C++20 compiler, CMake 3.28 or newer, Ninja, and the development files of SDL3 and Zydis 4.
- A graphics driver with OpenGL 3.3.

The recompiler supports one build of `nocturne.exe`, with SHA-256 `216205a09fb00008b37f115820e82c588d083873a34164f41c4d353761a3a844`. The setup script stops if your discs contain a different one.

## Extract the game files

The setup script reads the installer cabinets straight from the disc images. It needs no installer, no Wine, and no mounted discs. Run it from the repository folder and pass both images:

```
python3 tools/setup_game.py path/to/disc1.iso path/to/disc2.iso
```

The script writes the game folder to `data/game`. To put it somewhere else, add `--out <folder>`. It takes about 10 seconds and checks every file against the MD5 checksum stored in the cabinet.

The script also changes two settings in `data/game/system/NOCTURNE.INI`:

- `useDirect3D=1` turns on the hardware renderer. Native resolution and the textured goggle view need it.
- `MaxSwLatency=0.1` cuts the sound lag from about half a second to about 0.1 seconds.

If you run the script again, it replaces the game files but keeps `NOCTURNE.INI`, `RENDER.INI`, and your saves.

## Build on Linux

Install the tools and libraries. On Arch Linux:

```
sudo pacman -S --needed base-devel cmake ninja sdl3 zydis python
```

On other distributions, install the same tools plus the development packages of SDL3 and Zydis 4.

Extract the game files first, because the build runs the recompiler on `data/game/nocturne.exe`. Then build:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

GCC and Clang both work. The build writes `build/nocturne` and the texture baker `build/nocbake`. To recompile a copy of `nocturne.exe` from another folder, add `-DNOCTURNE_EXE=<path>` to the first command.

## Build on Windows

1. Install [MSYS2](https://www.msys2.org).
2. Open the **MSYS2 UCRT64** shell from the Start menu. MSYS2 installs software with `pacman`, the package manager it takes from Arch Linux. Install the tools and libraries:

   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-zydis mingw-w64-ucrt-x86_64-python
   ```

3. In the same shell, go to the repository folder, then extract the game files and build. MSYS2 writes the drive `C:` as `/c`.

   ```
   python tools/setup_game.py /c/path/to/disc1.iso /c/path/to/disc2.iso
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

The build writes `build/nocturne.exe` and `build/nocbake.exe`, and copies `SDL3.dll` next to them. To run the game outside the MSYS2 shell, also copy the DLLs that `ldd build/nocturne.exe | grep ucrt64` lists into `build`.

## Cross-compile for Windows on Linux

1. Install MinGW-w64. On Arch Linux, run `sudo pacman -S mingw-w64-gcc`.
2. Download `SDL3-devel-<version>-mingw.tar.gz` from the [SDL releases page](https://github.com/libsdl-org/SDL/releases) and unpack it.
3. Build the Linux version as described above. The cross build runs its recompiler.
4. Configure and build the Windows version:

   ```
   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
       -DSDL3_DIR=<SDL3 folder>/x86_64-w64-mingw32/lib/cmake/SDL3 \
       -DNOCTURNE_RECOMPILER=$PWD/build/recompiler
   cmake --build build-win
   ```

5. Copy `build-win/nocturne.exe`, `build-win/nocbake.exe`, and `build-win/SDL3.dll` to the Windows machine. To make the game folder there, run the setup script on that machine, or copy `data/game` over.

## Bake the goggle view textures

The goggle view textures the 3D level with the game's pre-rendered backdrops. The game bakes a level's textures the first time you put on the goggles there, and saves them in `data/game/baked`. While the bake runs, the goggle view shows the level untextured.

To bake all levels ahead of time, run this once. It takes several minutes.

```
build/nocbake data/game all
```

## Run the game

On Linux:

```
build/nocturne data/game
```

On Windows, in a command prompt:

```
build\nocturne.exe data\game
```

When the 3D starts, the window switches to fullscreen. To keep a window, set `NOC_WINDOWED=1`. On Linux, run `NOC_WINDOWED=1 build/nocturne data/game`. On Windows, run `set NOC_WINDOWED=1` before you start the game.

On every start, a message says "Unable to open .AVI!" because the opening video doesn't play yet. Click **OK** to go on. On the first start, the game also runs its monitor calibration. Press any key to step through it.

The game writes its log to the terminal. On Windows, the log appears in the console window that opens with the game.

## Keys and settings

The game keeps its own controls. The recomp adds these keys:

| Key | Action |
| --- | --- |
| `I` | Puts the night-vision goggles on or off. This is the game's own key. With textured night vision on, the goggles show the 3D level with baked textures, lit by the flashlight when it is on. |
| `F8` | Sets how fast the flashlight and goggles drain the battery: 100%, 50%, 25%, or 0%. The default is 0%. |
| `F10` | Turns textured night vision on or off. |

The game folder holds these files:

- `nocturne-recomp.ini` holds the recomp's settings from `F8` and `F10`.
- `system/NOCTURNE.INI` holds the game's own settings.
- `save` holds your saves.

These environment variables change how the game runs:

| Variable | Effect |
| --- | --- |
| `NOC_WINDOWED=1` | Keeps a window instead of switching to fullscreen. |
| `NOC_RENDER_HEIGHT=<pixels>` | Sets the height of the 3D image. The default is the window's height, which in fullscreen is your display's height. |
| `NOC_FPS=1` | Prints the frame rate every 5 seconds. |

## Troubleshooting

- **"DATA2.CAB is not on the given disc images."** The game files span both discs. Pass the images of disc 1 and disc 2 to the setup script.
- **"nocturne.exe has SHA-256 ..."** Your discs contain a build of the game that the recompiler doesn't support.
- **The sound crackles.** Raise `MaxSwLatency` in `data/game/system/NOCTURNE.INI`, for example to `0.2`. Each 0.1 adds about 90 ms of sound lag.
- **The goggle view shows an untextured level.** The level's bake isn't ready yet. Wait a moment, or bake all levels ahead of time.

## Known issues

- The opening video doesn't play.
- Small props such as pews and bottles exist only in the pre-rendered backdrops, so the goggle view doesn't show them.
- After you load a save, some levels, such as GTOWN, play no ambient sound. Loading a save skips the level script that starts it. The original game's code does the same.
