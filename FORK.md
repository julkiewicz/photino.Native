# Fork changes

This is a fork of [tryphotino/photino.Native](https://github.com/tryphotino/photino.Native) (Apache-2.0). Changes
on branch `windows-composition-hosting`, Windows only, behind a new constructor export
(`Photino_ctorWithOptions_win32`); `Photino_ctor` behaves as upstream:

- Modified: `Photino.Native/Photino.h`, `Photino.Native/Photino.Windows.cpp`, `Photino.Native/Exports.cpp`,
  `Photino.Native/Photino.Native.vcxproj`, `Photino.Native/Photino.Native.vcxproj.filters`.
- Added: `Photino.Native/Photino.Windows.Composition.cpp` (composition hosting, hit-region input routing, popup
  hosting), `packaging/windows/` (a Windows x64 package), `tests/CompositionCheck/` (a headless check run on a
  desktop of its own).

The options are documented on `PhotinoWindowsOptions` in `Photino.Native/Photino.h`.
