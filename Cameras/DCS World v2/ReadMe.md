Injectable Camera for DCS World
================================
Current supported game version: 2.9.x  
Camera version: 2.0  
Credits: jcofdi  

Rebuild Notes
========================
v2.0 - Clean rebuild from Sekiro template (matrix-based camera)
     - Based on Sekiro's clean 3x3 matrix structure instead of AC6's quaternion template
     - Double-precision position and rotation handling
     - Visualizer.dll hook for visual rotation control
     - Own position tracker to prevent drift from wind/autopilot
     - Proper MinHook cleanup on DLL detach (fixes crash on close)
     - SimpleMath ToEuler for IGCS connector axis reporting

Technical Notes
========================
- Camera code hooks DCS.exe (position writes, rotation functions, FOV clamp)
- Visualizer.dll rotation function hooked via MinHook for visual rotation control
- DCS uses X=forward, Y=up, Z=right; IGCS uses X=right, Y=up, Z=forward (X/Z swapped)
- All camera values are double-precision (64-bit)
- FOV stored in degrees in the game struct, tracked internally in radians

Features
===============
- IGCSDOF support
- Freecamera control (keyboard, mouse, gamepad)
- Field of View control
- Hotsampling (window resize)

Camera control device
========================
In the configuration tab of the IGCS Client, you can specify what to use for controlling
the camera: controller, keyboard+mouse, or both.
