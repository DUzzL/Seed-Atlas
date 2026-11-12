# Third-party notices

Seed Atlas includes or uses the following third-party components. Their names
identify the actual dependencies and are not product branding.

## cubiomes

Copyright (c) 2020 Cubitect. Licensed under the MIT License. The complete
license is installed with the program as `cubiomes-LICENSE.txt` (next to the
other license files listed below) and is part of the source archive at
`seedatlas-engine/LICENSE`.

## Qt 6.8.3

Qt Core, GUI, Widgets, Network, and SVG libraries are dynamically linked and
distributed under the GNU Lesser General Public License version 3, as
applicable. Users may replace the Qt libraries with compatible modified builds.
The LGPLv3 text (`LGPL-3.0.txt`) and the GPLv3 text (`LICENSE.txt`) are
installed with the program:

- Windows: the `Licenses` folder of the installation;
- macOS: `Seed Atlas.app/Contents/Resources/Licenses`;
- Linux Flatpak: `/app/share/doc/seed-atlas`.

Qt licensing information: https://www.qt.io/licensing/

`src/qzipwriter.h` is derived from the QZipWriter class of Qt (Copyright (C)
2016 The Qt Company Ltd.), which is available under the LGPLv3 or the
GPLv2/GPLv3; Seed Atlas uses it under the GPLv3.

## Lua 5.4

Copyright (C) 1994-2022 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Vulkan API declarations

`src/vkmini.h` repeats the types, values and function signatures of the few
Vulkan API calls the GPU code uses, as declared in `vulkan_core.h` of the
Khronos Vulkan-Headers. Copyright 2015-2025 The Khronos Group Inc., licensed
under the Apache License 2.0 or the MIT License (SPDX: Apache-2.0 OR MIT);
Seed Atlas uses them under the MIT License. Source:
https://github.com/KhronosGroup/Vulkan-Headers

## libwinsane (Windows builds)

`src/libwinsane` by Christopher Wellons is free and unencumbered software
released into the public domain (The Unlicense).

## QDarkStyleSheet

Parts of the legacy dark stylesheet are derived from QDarkStyleSheet by Colin
Duquesnoy and contributors, licensed under the MIT License.
Source: https://github.com/ColinDuquesnoy/QDarkStyleSheet

## Amidst

Biome colors and some icon concepts are inspired by Amidst, licensed under
GPLv3. Source: https://github.com/toolbox4minecraft/amidst

## MinGW-w64 runtime

Compiler runtime libraries are distributed under their applicable GNU
licenses and runtime exceptions. The corresponding toolchain sources and
license information are available from https://www.mingw-w64.org/ and in the
complete Seed Atlas source and build instructions.

## Microsoft Direct3D compiler runtime

`D3Dcompiler_47.dll` is a Microsoft redistributable component and remains
subject to Microsoft's applicable redistribution terms.

## Minecraft structure icons

Structure marker textures include Minecraft Java 26.3 block, item and entity
textures owned by Mojang/Microsoft, composed using the mappings in
https://github.com/DUzzL/seed-atlas-xaero . See rc/icons/ICON_SOURCES.md.
These game assets are not covered by the application code license.
