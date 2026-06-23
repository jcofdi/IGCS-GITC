////////////////////////////////////////////////////////////////////////////////////////////////////////
// Part of Injectable Generic Camera System
// Copyright(c) 2020, Frans Bouma
// All rights reserved.
// https://github.com/FransBouma/InjectableGenericCameraSystem
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////////
#pragma once

namespace IGCS::GameSpecific
{
	// Mandatory constants to define for a game
	#define GAME_NAME									"DCS World"
	#define CAMERA_VERSION								"2.0"
	#define CAMERA_CREDITS								"jcofdi"
	#define GAME_WINDOW_TITLE							"Digital Combat Simulator"
	#define INITIAL_PITCH_RADIANS						0.0f
	#define INITIAL_YAW_RADIANS							0.0f
	#define INITIAL_ROLL_RADIANS						0.0f
	#define CONTROLLER_Y_INVERT							false
	#define FASTER_MULTIPLIER							5.0f
	#define SLOWER_MULTIPLIER							0.07f
	#define MOUSE_SPEED_CORRECTION						0.2f
	#define DEFAULT_MOVEMENT_SPEED						0.05f
	#define DEFAULT_ROTATION_SPEED						0.003f
	#define DEFAULT_FOV_SPEED							0.05f
	#define DEFAULT_UP_MOVEMENT_MULTIPLIER				0.7f
	#define DEFAULT_FOV									0.94f	// ~54 degrees in radians
	#define DEFAULT_GAMESPEED							0.5f
	#define MATRIX_SIZE									12		// 3x3 rotation stored with stride 0x10 per row (doubles)
	#define COORD_SIZE									3
	#define DEFAULT_IGCS_TYPE							6
	// End Mandatory constants

	// AOB Keys for interceptor's AOB scanner
	#define CAMERA_ADDRESS_INTERCEPT_KEY				"AOB_CAMERA_ADDRESS_INTERCEPT"
	#define CAMERA_WRITE1_INTERCEPT_KEY					"AOB_CAMERA_WRITE1_INTERCEPT"
	#define CAMERA_WRITE2_INTERCEPT_KEY					"AOB_CAMERA_WRITE2_INTERCEPT"
	#define FOV_WRITE_INTERCEPT_KEY						"AOB_FOV_WRITE_INTERCEPT"

	// DCS World rendering camera struct (in DCS.exe, double-precision)
	// RBX = camera struct base
	//
	// Struct layout (all doubles, 8 bytes each):
	//   +0x08:  FOV (degrees)
	//   +0xA0:  rotation matrix row 0 (right.x, right.y, right.z) stride 0x10
	//   +0xB0:  rotation matrix row 1 (up.x, up.y, up.z)
	//   +0xC0:  rotation matrix row 2 (forward.x, forward.y, forward.z)
	//   +0x100: position X (forward in DCS)
	//   +0x108: position Y (up in DCS)
	//   +0x110: position Z (right in DCS)
	//
	// DCS coordinate convention: X=forward, Y=up, Z=right
	// IGCS coordinate convention: X=right, Y=up, Z=forward
	// Conversion: swap X and Z axes
	#define MATRIX_IN_STRUCT_OFFSET						0xA0
	#define COORDS_IN_STRUCT_OFFSET						0x100
	#define FOV_IN_STRUCT_OFFSET						0x08
	#define MATRIX_ROW_STRIDE							0x10	// 16 bytes between matrix rows (2 doubles)
}
