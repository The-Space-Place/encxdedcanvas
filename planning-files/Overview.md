# EncxdedCanvas
This is a vibecoded infinite zoom canvas app designed to facilitate a smooth experience creating mindmaps and other doodles for the purpose of high-quality encoding and teaching. The design is meant to be sleek and somewhat minimalistic with SVG, PNG, and JPG exports for still images and MP4 exports (including microphone if desired) for teaching sessions and live drawings. The design is based on Ahmni and the code is based on InfiniPaint. The project should be largely up-to-date with InfiniPaint, but since so much will be built on top of it and reworked, it’s expected that some harmonization is done if InfiniPaint happens to go through a substantial number of incompatible changes that may be beneficial for this project.

## Goal
Create a more Ahmni-like appearance, having the most essential tools on the top left, with 5 or so colors available. Have a main menu that features canvases, a basic schedule (for retrieval + goals, with the same features as the corresponding Ahmni image), and settings (generally the same as the standard InfiniPaint settings, but I’m interested in adding tons of features from Concepts as well).

## Some pointers + Ideas
- Make the layout highly similar to Ahmni.
- Optimize the program such that it runs even faster than it already does (seems to be very noticeably less responsive compared to Concepts)
- Make the written text appear completely smooth (interpolated) instead of blocky when zoomed in. (It's based on polygons and is rendered as polygons, but making the path smooth, like in an SVG, would make it appear significantly better at render-time. The thickness, of course, could be interpolated across the values based on the pressure and position).
- Make custom profiles such that the user can enable a canvas profile whenever they wish; would be best accomplished by doing the same thing as Concepts and adopting all the settings related to the previously opened canvas.
- Add dot grids and line grids; option to fix to the canvas (if you zoom out too far, the smaller grid fades away and a multiple comes back in; for example, a grid 4x coarser comes in once you zoom out far enough), and option to fix to the view (constantly in the same positions on screen, + one option where the grid still rotates when the view rotates though).


Reference all the images within planning-files/ to get a gist of inspiration images.