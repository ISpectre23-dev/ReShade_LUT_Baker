# Third-party notices

ReShade LUT Baker's own source is covered by [LICENSE](LICENSE).

The Wilds writer statically links the CPU GDeflate codec and its libdeflate
dependency. They are included in the add-on binary, not separate runtime DLLs.
Distributions must retain their license texts and copyright notices.

- [Microsoft DirectStorage GDeflate reference implementation](https://github.com/microsoft/DirectStorage/tree/c53f1499d5f67a61b69a1a348d22dcd2b4cb4ede/GDeflate/GDeflate):
  Apache License 2.0. Copyright Microsoft Corporation and Copyright (c) 2020,
  2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
- [NVIDIA libdeflate](https://github.com/NVIDIA/libdeflate/tree/8ba9502fb30d2bf728592d121f0d402e40c8cb05):
  MIT License, Copyright 2016 Eric Biggers. GDeflate contributions are Apache
  License 2.0, Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES.
  All rights reserved.

CMake copies the unmodified upstream license texts into
`build/<configuration>/licenses/GDeflate.txt` and `licenses/libdeflate.txt`.
The Windows artifact includes those files. Keep the `licenses` directory and
this notice in release packages. They do not need to be beside the installed
add-on for it to run.

No native game texture, proprietary shader, game DLL or game dump is bundled.
