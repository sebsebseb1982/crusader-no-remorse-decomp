# Toward real 3D: the `lightview` prototype

`lightview` (src/tools/lightview.cpp) is an experiment: how far can the original isometric sprites go in a 3D renderer, with no new art? It reads the game data like `mapview` and draws the same display list, then treats the picture as 3D.

## What it does

1. **Geometry from the boxes.** Every item has a world box (TYPEFLAG.DAT footpads). The view ray through each opaque sprite pixel enters the box on its top, +x or +y face: that point and that face's normal are the pixel's 3D position. Characters (NPC ids 1..255 and NPC shapes) and pixels off their box are billboards through the box centre, with a normal rounded from the sprite's silhouette. Small bumps come from the sprite's own shading.
2. **Metric space.** The projection is a 2:1 dimetric view (camera 45° round, 30° down), so one z unit is 3.266 x/y units. Lighting works in that space.
3. **Lights.** Point lights with a smooth falloff. Automatic ones: every item with enough colour-cycled pixels (palette 1..14: lamps, screens, lasers) and a lamp over the Silencer. Ambient level adjustable.
4. **Shadows.** Rays to each light against the item boxes, cut by the sprites: a point of a box shades only where its projection falls on an opaque pixel of the item's sprite, so railings and bodies cast their outline. A light is never shadowed by the item that gives it; damaging items (lasers, electrified floors) cast no shadow.
5. **Turning the camera** (`--orbit yaw pitch`, Q/W/Z/X). Every pixel of every sprite around the view (not only those left on top after sorting) becomes a lit 3D point, splatted with a depth buffer from the turned camera.
   - Characters keep facing the camera and show the frame of the direction they present to it: their current frame is found in ANIM.DAT (action, direction, index) and taken in the direction turned by yaw / 22.5° (16 directions).
   - The far faces of boxes, never drawn, repeat the near ones mirrored through the box (-x from +x, -y from +y).

CPU only, multithreaded: about 16 ms per 640×480 frame with 5 lights and shadows, about 40 ms when turned.

## Findings

- The reconstructed geometry is clean (`--normals`): floors, walls and crates come out as boxes without manual data.
- Dynamic light changes the mood more than the picture: the sprites already have their lighting painted in. It works best dark (`--ambient 0`), with the glowing items as the light sources.
- Turning the camera works far better than expected once every sprite pixel is kept: no holes up to ±25°, and the scene stays coherent at 45°, 90° and even 180°. Thin walls are convincing from behind; thick objects look solid with the mirrored far faces, though their back is a guess.
- Characters turn convincingly with their 8 or 16 directions. They stay flat billboards.
- Holes left: under items whose floor was never drawn (pits), and the edge of the loaded area.

## Next steps, if pursued

- Move the renderer to the GPU (points or quads per sprite pixel with a depth buffer, lights and shadow maps in shaders), and plug it into the game behind an option.
- Light from game events: muzzle flashes, explosions, projectiles.
- Picking, aiming and the combat zone assume the isometric screen; they would need a definition for a turned camera.

## Useful commands

```sh
lightview data 1                                             # interactive, map 1
lightview data --save private/e2e/saves/garde-mort-debout.sav --ambient 0 --png out.png
lightview data --save private/e2e/saves/tapis-roulants-traverses.sav --orbit 180 0 --png back.png
lightview data 1 --probe 480 260 --png out.png              # what shadows a pixel, light by light
```
