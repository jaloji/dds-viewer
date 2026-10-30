# Test files

All files here were made for this project and carry no third-party content.

- `src/gradient.png`: a 256 x 256 RGBA gradient written by a small script.
- `gradient_<FORMAT>.DDS`: `src/gradient.png` converted with Microsoft's `texconv` (DirectXTex), one file per
  DXGI format, e.g. `texconv -y -f BC7_UNORM -sx _BC7_UNORM -o . src/gradient.png`. The `dx10_` files force the
  DX10 header (`-dx10`).

Check them with `wictest DdsWic.dll <file.dds> out.png`, then after installing with `wictest --system`.
