# JoliPuTTY

**JoliPuTTY** is a playful fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/),
made for fun by **Sylvain Cresto**, with the help of Claude Opus. It gives
PuTTY (the `jputty` binary) a modern **WinUI 3** look on **Windows 11**,
with tabs, split panes and a redesigned configuration window, while
staying 100% PuTTY underneath.

It is **open source and freely distributable**, provided **as is, without
any warranty** that it works, or that it's fit for any purpose: use it at
your own risk.

JoliPuTTY is an **independent project and is not affiliated with the PuTTY
project**.

Repository: <https://github.com/scresto09/joliputty>

For issues, please visit <https://github.com/scresto09/joliputty/issues>

---

## What's different from PuTTY

The original PuTTY README has been renamed to [README.PuTTY](README.PuTTY).

All of this is Windows-only, in the WinUI 3 front end in `windows/winui`:

- **Tabs in the title bar**, with a per-tab right-click menu (New tab,
  Rename tab, Duplicate, Restart session, Close…), and **split panes**
  inside a tab.
- **A session pop-up in the title bar** (the chevron, or `Ctrl+Shift+Space`)
  listing the favorite, recent and saved sessions, with a box to type an
  address to connect to (`user@host:port`, `ssh://`, `telnet://`…).
- **A redesigned configuration window**: a Sessions page with Favorites,
  Recent and all the sessions in **folders**, the settings regrouped in
  fewer pages, a reset button on each changed setting, and the same window
  to change the settings of a running session (and save them, or not).
- **Folders compatible with PuTTY-ng**: a session called `Clients#ACME#web`
  is the session `web` in the folder `ACME` in the folder `Clients`.
- **Copy/paste pools**: everything copied is kept in the tab's history, and
  can be kept for the saved session or for all of them (stored encrypted
  with DPAPI), to paste again from a pop-up (`Ctrl+Alt+V`).
- **Send and receive files** over SFTP, in the SSH session already running.
- **Find** in the terminal, from the terminal's right-click menu.
- **Light / dark theme** that follows the Windows setting or not, with the
  selected tab taking the terminal's background colour if wanted.
- **Modern dialogs** everywhere: Event Log, About, file, font and colour
  pickers, host CAs, errors and questions.
- **Keyboard shortcuts**: `Ctrl+Alt+1`…`9` to go to a tab, `Alt+arrows` to
  move between panes, `Alt+Shift++` / `Alt+Shift+-` to split, `Ctrl+Shift+W`
  to close a pane. A session can send them to the server instead (Terminal
  settings).
- **Automate logon** commands, kept in the Windows Credential Manager.
- **The binary is renamed `jputty.exe`**, and it identifies itself to SSH
  servers as `JoliPuTTY_<version>`.

The saved sessions are stored where PuTTY stores them, so PuTTY and
PuTTY-ng can still use them. PuTTYtel, pterm and the command-line tools
(Plink, PSCP, PSFTP, Pageant, PuTTYgen) keep the classic Win32 interface.

---

## Prerequisites

- **Windows 11** (Windows 10 may work but is not the target).
- **Visual Studio** with the MSVC C++ toolchain, or clang-cl (the WinUI code
  is C++/WinRT and only builds with MSVC: MinGW cannot build it, and then
  `jputty.exe` is just left out).
- **CMake 3.18** or later.
- **Internet access** for the first CMake run, which downloads the Windows
  App SDK and C++/WinRT NuGet packages into the build directory.
- At **runtime**, the **Windows App Runtime** must be installed on the
  machine (JoliPuTTY offers to download it if it's missing), and
  `Microsoft.WindowsAppRuntime.Bootstrap.dll` must be next to the
  executable (the build copies it there).

---

## Building

From a **Visual Studio x64 Native Tools** command prompt, in the source
directory:

```
cmake .
cmake --build . --config Release
```

This builds `jputty.exe` along with the rest of PuTTY. The Windows App SDK
version can be changed with `-DPUTTY_WINAPPSDK_VERSION=<version>` (see
`cmake/winui.cmake`).

### Manual

The online manual is PuTTY's, with JoliPuTTY's own chapter and notes,
made by `joliputty-help/make_help.sh` (with Halibut, which it fetches and
builds if it isn't installed) in `joliputty-help/html/<version>/`. When a
`joliputty-X.Y` tag is pushed to GitHub, `.github/workflows/help.yml`
makes it and puts it on the web site, at
<https://scresto09.github.io/joliputty/X.Y/>, where that version's Help
looks for it; builds that aren't releases use `latest/`.

### MSI installer

A classic installer, which installs `jputty.exe` in
`Program Files\JoliPuTTY`, with Start menu shortcuts (and optionally a
desktop shortcut and the install directory on the `PATH`):

```
cmake --build . --config Release --target msi
```

This needs [WiX Toolset](https://wixtoolset.org/) 3.14 or later (on the
`PATH`, or where its installer puts it, in `%WIX%`), and creates
`JoliPuTTY_<version>_<arch>.msi`. It can be installed alongside PuTTY:
it has its own identity, and leaves the saved sessions alone. It
doesn't install the Windows App Runtime, which JoliPuTTY offers to
download when it's missing. See `windows/msi/joliputty.wxs`.

### MSIX package

JoliPuTTY can be packaged as a single MSIX, without installer, e.g. for the
Microsoft Store:

```
cmake --build . --config Release --target msix
```

This needs `makeappx.exe` and `makepri.exe` from the Windows SDK and creates
the unsigned `JoliPuTTY_<version>_<arch>.msix`. The Store signs it. The
package identity defaults to `SylvainCresto.JoliPuTTY`, with the publisher
of the Partner Center account; override it by running
`windows\msix\make_msix.ps1` by hand with `-Name`, `-Publisher` and
`-PublisherName` (e.g. for a self-signed test build, set `-Publisher` to the
test certificate's subject). To test it without signing, enable the
Developer Mode and register the staging directory:

```
Add-AppxPackage -Register msix\staging\AppxManifest.xml
```

`jputty` is then available in a command prompt. The package depends on the
Windows App Runtime used for building: the Store installs it, otherwise it
must already be installed. The saved sessions stay where PuTTY keeps them
in the registry, shared with PuTTY. The logos in `windows/msix/Assets` are
made from `windows/putty.ico` by `windows/msix/mkassets.py`.

---

## Status

This is an **experimental, for-fun fork**. Use at your own risk, and
enjoy! :-)

---

## Credits & license

JoliPuTTY is built on the wonderful **PuTTY** by Simon Tatham et al. The PuTTY
licence (the MIT licence, see [LICENCE](LICENCE)) applies to the original
PuTTY code; the JoliPuTTY-specific additions are distributed under the same
terms.
