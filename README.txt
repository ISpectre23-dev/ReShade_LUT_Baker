ReShade LUT Baker
By ISpectre23

Exports the color grading of selected ReShade techniques to a 3D lookup
table (LUT). The bake follows ReShade's technique order and uses a
floating-point target instead of a screenshot.


Requirements
------------
- 64-bit Windows.
- ReShade 6.8.0 or newer, with full add-on support.
- Performance mode turned off when baking techniques.

The add-on is built for ReShade 6.8.0. Newer versions must remain compatible
with its add-on interface.


Installation
------------
1. Close the game.
2. Copy ReShadeLUTBaker.addon64 next to the game's ReShade DLL, normally in
   the folder of the game executable.
3. Start the game and open ReShade's Add-ons tab.
4. Check that ReShade LUT Baker is listed.

Only the .addon64 file needs to be copied into the game folder. No extra
textures, shaders or libraries are required by the baker.


Bake a LUT
----------
1. Set up your grading techniques and their parameters in ReShade.
2. Open Add-ons > ReShade LUT Baker.
3. Under Techniques, select the techniques to bake. Only the checked
   techniques are included, in their listed ReShade execution order.
4. Under Output, choose the format, size and other available options.
5. Optionally enter a File name. Leave it empty for an automatic name.
6. Press the bake button and wait for the Result section.

Select active selects exactly the techniques currently enabled in ReShade.
You can also select disabled techniques. Clear deselects everything.
Refresh reloads the technique list.

The baker keeps unsaved shader parameters, technique states and order. It
does not save your preset. While baking, settings are locked. Cancel export
is available until file writing starts. It stops the bake without writing
a file.

With nothing selected, the button exports an identity LUT (no color change)
and reports how accurately the bake reproduced it.


Output formats
--------------
CUBE (.cube)
  Preserves floating-point values, including values outside 0-1.
  Choose 16, 32, 64 or 128 points per color axis, or a custom size from 2 to
  128. A size of 64 means 64x64x64 points and is the recommended default.
  A size of 128 has eight times as many points and is rarely needed.

PNG (.png)
  Choose 8 bits per channel (default) or 16 bits per channel.
  Horizontal strip supports sizes 16, 32, 64 and 128.
  Square tiles supports sizes 16 and 64. It is not the Hald layout.
  At size 64, these layouts are 4096x64 and 512x512 pixels respectively.
  The reader must support the chosen layout, size and bit depth.
  ReShade 6.8.0 loads PNG samples as 8 bits, so 16-bit export only adds
  precision in applications that read 16-bit PNG.

Monster Hunter Rise (.tex.28)
  Exports the game's native TEX v28 format at a fixed size of 32x32x32,
  with 8 bits per channel. Use a suitable LUT manager or mod loader to
  install it. The baker does not modify the game.

PNG and Rise only store values in 0-1. Values outside that range are
rejected unless you enable Clamp to 0-1, which clips them. The Result
section reports clipping when it occurs.


Find and load your export
-------------------------
Files are written to LUT_Bakes inside ReShade's base folder. Use Open output
folder in the add-on to find them. Existing exports are never overwritten;
the baker adds a suffix to the name when needed.

The baker only creates the file. To apply it, use a LUT reader or application
that supports its format and size. No LUT loader is included in this package.
For PNG, its layout and bit depth must also match the reader's requirements.


Limitations
-----------
A 3D LUT maps one RGB (red, green and blue) color to another. Select only
techniques whose output depends on the color of the input pixel.

Do not include sharpening, blur, bloom, vignette, film grain, dithering,
depth effects or other spatial, temporal or random processing. The baker
does not classify techniques for you.

The bake uses its own floating-point target, not the game image. Shaders
that depend on buffer size, format, color space or sRGB conversion (how
color values are read and written) can behave differently, especially
with high dynamic range (HDR) output.
Time-dependent shader inputs are not frozen, and other add-ons can affect
rendering. Compare the exported LUT with the original grading before
relying on it.


Troubleshooting and removal
---------------------------
If the add-on is not listed, check its installation folder and make sure
your ReShade build has full add-on support. Check ReShade.log for errors.

If a bake fails, read the message in Result and follow its instructions.
A technique that cannot compile for the offscreen target cannot be baked.

To uninstall, close the game and remove ReShadeLUTBaker.addon64. Your
exported LUT files are not removed.


License and documentation
-------------------------
See LICENSE for the MIT terms, third-party notices and name/logo policy.
The full documentation is available at:
https://github.com/ISpectre23/ReShade-LUT-Baker
