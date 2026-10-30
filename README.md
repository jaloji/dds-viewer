# dds-viewer

Windows Explorer thumbnails for **every** DDS format.

Windows only decodes BC1–BC3 (DXT1/3/5) DDS files. dds-viewer adds a WIC codec built on Microsoft's [DirectXTex](https://github.com/microsoft/DirectXTex), so BC4, BC5, BC6H, BC7, sRGB, uncompressed and float textures get thumbnails too.

- Thumbnails, preview pane and photo viewers.
- Small mipmaps are used for thumbnails, so large textures stay fast.
- sRGB is shown as stored, HDR is clipped, cubemaps and arrays show their first image.

## Install

1. Download a release (or build it) and extract it.
2. Run `install.bat` as administrator.
3. If old icons remain, run `clear-thumbnails.bat`.

Uninstall with `uninstall.bat`. If Microsoft Defender ASR rules block the DLL, allow `C:\Program Files\DdsViewer\DdsWic.dll` with `Add-MpPreference -AttackSurfaceReductionOnlyExclusions`.

## Build

Needs CMake 3.28+, Git and Visual Studio 2022 (C++). Run `build.bat`: the package is created in `dist\`. DirectXTex is downloaded at a pinned release.

## License

MIT. DirectXTex is © Microsoft, MIT license.
