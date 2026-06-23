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
#include "stdafx.h"
#include "CameraManipulator.h"
#include "GameConstants.h"
#include "Globals.h"
#include "Camera.h"
#include "GameCameraData.h"
#include "SimpleMath.h"
#include "MessageHandler.h"
#include "MinHook.h"
#include <vector>
#include <cstring>

using namespace DirectX;
using namespace std;
using namespace DirectX::SimpleMath;

extern "C" {
	LPBYTE g_cameraStructAddress = nullptr;
}

// ---------------------------------------------------------------
// DCS struct layout (all doubles, 8 bytes each):
//   +0x08:  FOV (degrees)
//   +0xA0:  rotation matrix row 0 (right vector), stride 0x10 per row
//   +0xB0:  rotation matrix row 1 (up vector)
//   +0xC0:  rotation matrix row 2 (forward vector)
//   +0x100: position X (forward in DCS)
//   +0x108: position Y (up in DCS)
//   +0x110: position Z (right in DCS)
//
// DCS coordinate convention: X=forward, Y=up, Z=right
// IGCS coordinate convention: X=right,   Y=up, Z=forward
// Conversion: swap X and Z axes
// ---------------------------------------------------------------

// --- Visualizer.dll rotation buffer ---
// The visual rotation is a standard 4x4 double homogeneous matrix at Visualizer.dll base + 0x2B1278
// Row stride = 4 doubles (0x20 bytes)
// Rotation in cols 0-2 of rows 0-2; col 3 = 0; row 3 = [posX, posY, posZ, 1.0]
// The function that writes it is at Visualizer.dll base + 0x162160
static LPBYTE g_visualizerBase = nullptr;
static const DWORD VISUALIZER_ROTATION_OFFSET = 0x2B1278;
static const DWORD VISUALIZER_ROTATION_FUNC_OFFSET = 0x162160;

static double* getVisualRotationMatrix()
{
	if (nullptr == g_visualizerBase) return nullptr;
	return reinterpret_cast<double*>(g_visualizerBase + VISUALIZER_ROTATION_OFFSET);
}

// MinHook trampoline for the rotation write function
typedef void(__fastcall* VisualizerRotationFunc)(void* rcx, void* rdx, void* r8, void* r9);
static VisualizerRotationFunc g_originalRotationFunc = nullptr;

// --- smCamera_Implement::SetPosition (the renderer's per-frame view-matrix setter) ---
// Visualizer.dll + 0x13A670. Copies an incoming 4-row double matrix (rdx) into the render
// camera at +0x448 (orient rows at +0,+0x20,+0x40 ; position row at +0x60). ~21 instances
// (main view, mirrors, shadow cascades, MFDs...). MEASUREMENT: log each distinct camera once
// to find which one is the main view. Read-only; calls original unchanged.
static const DWORD SMCAMERA_SETPOS_OFFSET = 0x13A670;
typedef void(__fastcall* SetPositionFunc)(LPBYTE cam, double* mat);
static SetPositionFunc g_originalSetPosition = nullptr;

// our rotation; defined below. Forward-declared so the SetPosition hook can drive it.
extern "C" double g_renderRot[9];

// Drive control for the render-camera hook. Default = LOG ONLY (no view change), so this is
// safe against the working build. Flip g_driveRenderCam to true to overwrite the render camera's
// rotation. g_driveCamType limits the write to one camType (set it from the census log once we
// know the main view); -1 drives every camera (test only -- would also tilt mirrors/shadows).
static bool g_driveRenderCam = false;
static int  g_driveCamType   = -1;

// r8 (3rd arg of the per-frame render-graph func 0x162160) is the final view matrix the active
// camera funnels into, every mode. TEST: overwrite its 3x3 with a fixed, obviously-wrong rotation
// every frame. If the view snaps to it in F1/F2/F11, r8 is the all-modes override point.
static bool g_testR8Override = true;

static void __fastcall hookedSetPosition(LPBYTE cam, double* mat)
{
	// Unconditional liveness probe: is this function ever called per-frame at all?
	static volatile long s_setPosCalls = 0;
	long spN = InterlockedIncrement(&s_setPosCalls);
	if (spN == 1 || (spN % 600) == 0)
		IGCS::MessageHandler::logLine("SetPos fired: %ld calls", spN);

	if (cam != nullptr && mat != nullptr)
	{
		const int camType = *reinterpret_cast<int*>(cam + 0x5F0);

		// Track cameras we've seen + their last orientation. Incoming pose is a 4x4 of doubles,
		// stride 0x20/row: orient rows mat[0..2],[4..6],[8..10]; position row mat[12..14].
		static LPBYTE s_seenCams[64] = { 0 };
		static double s_lastOrient[64] = { 0 };
		static int s_seenCount = 0;
		int idx = -1;
		for (int i = 0; i < s_seenCount; i++) { if (s_seenCams[i] == cam) { idx = i; break; } }

		if (idx < 0 && s_seenCount < 64)
		{
			// Census: log each distinct camera once -> which cams/types fire in THIS mode.
			idx = s_seenCount++;
			s_seenCams[idx] = cam;
			s_lastOrient[idx] = mat[0];
			IGCS::MessageHandler::logLine("smCam SetPos #%d: cam=%p type=%d pos=(%.1f, %.1f, %.1f) orient0=(%.4f %.4f %.4f)",
				s_seenCount, (void*)cam, camType, mat[12], mat[13], mat[14], mat[0], mat[1], mat[2]);
		}
		else if (idx >= 0)
		{
			// Rotation tracking: when a known camera's orientation changes, log it (throttled).
			// Rotate the view in each mode -> the camType whose orient moves IS that mode's view.
			static long s_rotLogged = 0;
			if (s_rotLogged < 200 && fabs(mat[0] - s_lastOrient[idx]) > 0.02)
			{
				s_lastOrient[idx] = mat[0];
				s_rotLogged++;
				IGCS::MessageHandler::logLine("smCam ROT cam=%p type=%d orient=(%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f)",
					(void*)cam, camType,
					mat[0], mat[1], mat[2], mat[4], mat[5], mat[6], mat[8], mat[9], mat[10]);
			}
		}

		// Gated write (OFF by default): replace the incoming orientation rows with ours, in place,
		// before the original copies them into the camera. Same safe arg-rewrite as the +0xA0 path.
		if (g_driveRenderCam && (g_driveCamType < 0 || camType == g_driveCamType))
		{
			mat[0] = g_renderRot[0]; mat[1] = g_renderRot[1]; mat[2]  = g_renderRot[2];
			mat[4] = g_renderRot[3]; mat[5] = g_renderRot[4]; mat[6]  = g_renderRot[5];
			mat[8] = g_renderRot[6]; mat[9] = g_renderRot[7]; mat[10] = g_renderRot[8];
		}
	}
	g_originalSetPosition(cam, mat);
}
static volatile long g_hookCallCount = 0;

// FOV: tracked internally in radians, written to game struct in degrees
static double g_customFoV = 0.0;
extern "C" double g_customFovDegrees = 0.0;

// Rotation fed to the WRITE1/WRITE2 interceptors. With the skip removed, those game functions
// build the render rotation in place from the +0xA0 matrix; we pre-load OUR rotation here so
// they produce ours instead of the game's. Layout = matBase[0,1,2,4,5,6,8,9,10] (row-major 3x3).
extern "C" double g_renderRot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

// Active view camera's rotation-matrix pointer (= camera + 0xA0), captured by the WRITE1/WRITE2
// interceptors for WHATEVER view camera is currently being updated (cockpit/external/freecam).
// camera base = g_activeCameraMat - 0xA0; *(void**)base = its vtable (identifies the type).
extern "C" volatile LPBYTE g_activeCameraMat = nullptr;

// Render FOV pointer — captured by ASM interceptor
extern "C" LPBYTE g_fovRenderAddress = nullptr;
extern "C" volatile DWORD g_fovHookCallCount = 0;

// Own position tracker: maintains double-precision position independent of game struct.
// Prevents drift from unblocked game writes (wind, inertia, autopilot corrections)
// and avoids double->float->double precision loss.
static double s_ownPosition[3] = { 0, 0, 0 };  // DCS coordinates (X=fwd, Y=up, Z=right)
static bool s_ownPositionInitialized = false;

// Cached rotation for re-application in the Visualizer hook
static double s_cachedVizValues[16] = { 0 };
static bool s_vizValuesCached = false;
static double s_lastWrittenRot[12] = { 0 };
static int s_diagFrameCount = 0;

// Rotation-write diagnostics. s_diagReadbackRot holds the Viz rotation cells as seen
// at the START of the hook (before we re-apply) -> reveals whether the game overwrote
// our previous frame's rotation. s_diagLastYaw drives input-triggered logging.
static double s_diagReadbackRot[9] = { 0 };
static float s_diagLastYaw = 0.0f;
static int s_diagRotLogged = 0;

// Render-matrix finder: on enable, scan all writable memory for every stride-4 rotation matrix
// equal to the seeded (real) rotation. A few seconds later, the copies that did NOT follow our
// rotation are the matrices the renderer reads but we don't drive -> the render (and HUD) matrix.
static std::vector<double*> s_renderCandidates;
// Confirmed render matrices we drive every frame (divergent + valid rotation, no garbage).
static std::vector<double*> s_renderTargets;

// True if d holds a plausible stride-4 rotation matrix (finite, |cells|<2, not degenerate).
// Guards against writing into a heap block that was freed/reused with garbage.
static bool plausibleRot(const double* d)
{
	static const int idx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
	double sumsq = 0.0;
	for (int i = 0; i < 9; i++)
	{
		double v = d[idx[i]];
		if (!(v == v) || v >= 2.0 || v <= -2.0) return false;  // NaN/inf/huge -> reject
		sumsq += v * v;
	}
	return sumsq > 1.5;  // a real rotation has sum of squares == 3
}

// Double-precision position cache for IGCSDOF sessions
static double s_igcsSessionStartPos[3] = { 0, 0, 0 };
static bool s_igcsSessionPositionCached = false;


// --- Visualizer.dll hook ---
// Called every frame by DCS to update the visual rotation/projection buffer.
// We call the original (to get projection updates), then re-apply our rotation+position.
void __fastcall hookedRotationFunc(void* rcx, void* rdx, void* r8, void* r9)
{
	InterlockedIncrement(&g_hookCallCount);

	// FOV PROBE: 0x162160 copies TWO 4x4 matrices from r8 -- view at r8+0x00, projection at r8+0x80.
	// Log the projection (16 doubles at r8+0x80) so we can confirm it's a perspective matrix and find
	// the focal-length (FOV) terms. Log once, then again whenever the likely vertical-focal term p[5]
	// changes >2% (i.e. when you zoom) so we can map term -> FOV. All modes, no enable needed.
	if (r8 != nullptr)
	{
		double* p = reinterpret_cast<double*>(r8) + 16;   // second matrix (r8 + 0x80)
		static bool s_projLogged = false;
		static double s_lastP5 = 0.0;
		if (!s_projLogged || fabs(p[5] - s_lastP5) > 0.02 * (fabs(s_lastP5) + 1e-9))
		{
			s_projLogged = true; s_lastP5 = p[5];
			IGCS::MessageHandler::logLine("PROJ r8+0x80: [%.5f %.5f %.5f %.5f][%.5f %.5f %.5f %.5f][%.5f %.5f %.5f %.5f][%.5f %.5f %.5f %.5f]",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		}
	}


	// ALL-MODES render rotation override (CONFIRMED): r8 is the per-frame view matrix; forcing it
	// locks rotation in F1/F2/F11. When the camera is enabled, drive OUR rotation into it every
	// frame, every mode. (g_testR8Override is the master toggle.)
	// BASIS: r8 is consumed in the SAME (DCS) basis as g_renderRot -- write it RAW. An earlier
	// P*g_renderRot*P attempt produced a clean one-conjugation error in-game (yaw mirrored across
	// N/S, pitch<->roll swapped) = exactly the X<->Z swap, proving the swap must NOT be applied here.
	if (g_testR8Override && g_cameraEnabled && r8 != nullptr)
	{
		double* m = reinterpret_cast<double*>(r8);   // view matrix, stride 0x20/row
		m[0] = g_renderRot[0]; m[1] = g_renderRot[1]; m[2]  = g_renderRot[2];
		m[4] = g_renderRot[3]; m[5] = g_renderRot[4]; m[6]  = g_renderRot[5];
		m[8] = g_renderRot[6]; m[9] = g_renderRot[7]; m[10] = g_renderRot[8];

		// Drive POSITION too: the render reads its eye position from this same matrix's position row
		// (r8[12,13,14]). s_ownPosition is our DCS-basis tracker -- freecam movement AND IGCS-DOF
		// micro-steps both land in it (IGCS_MoveCameraMultishot -> stepCameraforIGCSSession ->
		// _camera.moveRight/Up -> s_ownPosition). Diag showed r8 pos == DCS struct pos == s_ownPosition
		// (no axis swap on position). Gate on init so we never write a stale (0,0,0) and teleport.
		if (s_ownPositionInitialized)
		{
			m[12] = s_ownPosition[0]; m[13] = s_ownPosition[1]; m[14] = s_ownPosition[2];
		}
	}

	if (g_cameraEnabled)
	{
		double* vizRotEntry = getVisualRotationMatrix();

		// DIAGNOSTIC: snapshot the rotation cells as the game left them at hook entry.
		// If these don't match what we wrote last frame, the game is overwriting our rotation.
		if (vizRotEntry != nullptr)
		{
			s_diagReadbackRot[0] = vizRotEntry[0]; s_diagReadbackRot[1] = vizRotEntry[1]; s_diagReadbackRot[2] = vizRotEntry[2];
			s_diagReadbackRot[3] = vizRotEntry[4]; s_diagReadbackRot[4] = vizRotEntry[5]; s_diagReadbackRot[5] = vizRotEntry[6];
			s_diagReadbackRot[6] = vizRotEntry[8]; s_diagReadbackRot[7] = vizRotEntry[9]; s_diagReadbackRot[8] = vizRotEntry[10];
		}

		// Fix DCS.exe struct rotation BEFORE calling original
		if (s_vizValuesCached && g_cameraStructAddress != nullptr)
		{
			double* matBase = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
			matBase[0] = s_lastWrittenRot[0]; matBase[1] = s_lastWrittenRot[1]; matBase[2] = s_lastWrittenRot[2];
			matBase[4] = s_lastWrittenRot[3]; matBase[5] = s_lastWrittenRot[4]; matBase[6] = s_lastWrittenRot[5];
			matBase[8] = s_lastWrittenRot[6]; matBase[9] = s_lastWrittenRot[7]; matBase[10] = s_lastWrittenRot[8];
		}

		// CANDIDATE FIX (experimental): also apply our rotation to the Viz buffer BEFORE the
		// original runs, in case the render consumes rotation mid-function (before our post-call
		// re-apply). Rotation cells only -- position row 3 is left to the proven post-call path.
		if (s_vizValuesCached && vizRotEntry != nullptr)
		{
			vizRotEntry[0]  = s_cachedVizValues[0];  vizRotEntry[1]  = s_cachedVizValues[1];  vizRotEntry[2]  = s_cachedVizValues[2];
			vizRotEntry[4]  = s_cachedVizValues[4];  vizRotEntry[5]  = s_cachedVizValues[5];  vizRotEntry[6]  = s_cachedVizValues[6];
			vizRotEntry[8]  = s_cachedVizValues[8];  vizRotEntry[9]  = s_cachedVizValues[9];  vizRotEntry[10] = s_cachedVizValues[10];
		}

		g_originalRotationFunc(rcx, rdx, r8, r9);

		// Re-apply our cached rotation + position
		if (s_vizValuesCached)
		{
			double* vizRot = getVisualRotationMatrix();
			if (vizRot != nullptr)
			{
				vizRot[0]  = s_cachedVizValues[0];  vizRot[1]  = s_cachedVizValues[1];  vizRot[2]  = s_cachedVizValues[2];
				vizRot[4]  = s_cachedVizValues[4];  vizRot[5]  = s_cachedVizValues[5];  vizRot[6]  = s_cachedVizValues[6];
				vizRot[8]  = s_cachedVizValues[8];  vizRot[9]  = s_cachedVizValues[9];  vizRot[10] = s_cachedVizValues[10];
				vizRot[12] = s_cachedVizValues[12]; vizRot[13] = s_cachedVizValues[13]; vizRot[14] = s_cachedVizValues[14];
			}
		}
		return;
	}
	g_originalRotationFunc(rcx, rdx, r8, r9);
}


// --- Helper: read DCS 3x3 matrix (stride 0x10) into IGCS-convention XMMATRIX ---
// DCS rows: row0=right, row1=up, row2=forward (each 3 doubles, stride 4 doubles)
// IGCS expects: row0=right, row1=up, row2=forward (same semantic, but X/Z swapped)
// Conversion: R_IGCS[i][j] = R_DCS[swap(i)][swap(j)] where swap(0)=2, swap(1)=1, swap(2)=0
static XMMATRIX readDcsRotationMatrix()
{
	double* vizRot = getVisualRotationMatrix();
	if (vizRot != nullptr)
	{
		// Visualizer buffer: rotation in cols 0-2 of rows 0-2
		return XMMATRIX(
			(float)vizRot[10], (float)vizRot[9], (float)vizRot[8], 0.0f,
			(float)vizRot[6],  (float)vizRot[5], (float)vizRot[4], 0.0f,
			(float)vizRot[2],  (float)vizRot[1], (float)vizRot[0], 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		);
	}

	// Fallback: DCS.exe struct (stride 4 doubles per row)
	double* matBase = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
	return XMMATRIX(
		(float)matBase[10], (float)matBase[9], (float)matBase[8], 0.0f,
		(float)matBase[6],  (float)matBase[5], (float)matBase[4], 0.0f,
		(float)matBase[2],  (float)matBase[1], (float)matBase[0], 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	);
}


// Permanent enable-time diagnostic. Fires once each time the camera is enabled
// (the disable path resets s_ownPositionInitialized). Dumps everything needed to
// diagnose a DCS update without rebuilding: struct/buffer addresses, the raw Viz
// 4x4, a layout guess, and the rotation matrix as currently interpreted.
static void dumpEnableDiagnostics(double* pos, double* vizRot)
{
	IGCS::MessageHandler::logLine("================ IGCS ENABLE DIAGNOSTIC ================");
	IGCS::MessageHandler::logLine("cameraStruct=%p  vizBase=%p  vizBuf=%p",
		(void*)g_cameraStructAddress, (void*)g_visualizerBase, (void*)vizRot);
	if (pos != nullptr)
		IGCS::MessageHandler::logLine("DCS struct pos[0..2]: %.3f  %.3f  %.3f", pos[0], pos[1], pos[2]);

	if (vizRot != nullptr)
	{
		IGCS::MessageHandler::logLine("Viz 4x4 (row-major doubles):");
		IGCS::MessageHandler::logLine("  [ %12.6f %12.6f %12.6f %12.6f ]", vizRot[0], vizRot[1], vizRot[2], vizRot[3]);
		IGCS::MessageHandler::logLine("  [ %12.6f %12.6f %12.6f %12.6f ]", vizRot[4], vizRot[5], vizRot[6], vizRot[7]);
		IGCS::MessageHandler::logLine("  [ %12.6f %12.6f %12.6f %12.6f ]", vizRot[8], vizRot[9], vizRot[10], vizRot[11]);
		IGCS::MessageHandler::logLine("  [ %12.6f %12.6f %12.6f %12.6f ]", vizRot[12], vizRot[13], vizRot[14], vizRot[15]);

		// Layout auto-detect: DCS world coords are large (thousands); rotation cells are in [-1,1].
		// Whichever of row-3 / col-3 holds the big numbers is the translation; that tells us the layout.
		bool rowPosBig = (fabs(vizRot[12]) > 50.0 || fabs(vizRot[13]) > 50.0 || fabs(vizRot[14]) > 50.0);
		bool colPosBig = (fabs(vizRot[3])  > 50.0 || fabs(vizRot[7])  > 50.0 || fabs(vizRot[11]) > 50.0);
		const char* guess = rowPosBig ? "position in ROW 3 (idx 12-14), rotation in cols 0-2  [CURRENT code assumption]"
			: (colPosBig ? "position in COL 3 (idx 3,7,11), rotation in cols 1-3  [OLD layout]" : "UNKNOWN");
		IGCS::MessageHandler::logLine("Layout guess: %s", guess);
		if (!rowPosBig && !colPosBig)
			IGCS::MessageHandler::logLine("  WARNING: no large translation cell found -- buffer offset may be wrong.");
	}
	else
	{
		IGCS::MessageHandler::logLine("Viz buffer NULL (using DCS struct rotation fallback).");
	}

	XMMATRIX r = readDcsRotationMatrix();
	XMFLOAT4X4 rf; XMStoreFloat4x4(&rf, r);
	IGCS::MessageHandler::logLine("readDcsRotation rows: [%.4f %.4f %.4f] [%.4f %.4f %.4f] [%.4f %.4f %.4f]",
		rf._11, rf._12, rf._13, rf._21, rf._22, rf._23, rf._31, rf._32, rf._33);
	IGCS::MessageHandler::logLine("=======================================================");
}


// Walk all committed, writable memory regions and collect every address where a stride-4 3x3
// rotation matrix equals the seeded rotation R0 (within tolerance). Safe: VirtualQuery skips
// unreadable/guard pages. One-time on enable; may cause a brief (~1-2s) hitch.
static void scanForRenderMatrix(const double R0[9])
{
	s_renderCandidates.clear();
	const double tol = 5e-4;
	SYSTEM_INFO si; GetSystemInfo(&si);
	LPBYTE maxA = (LPBYTE)si.lpMaximumApplicationAddress;
	int found = 0;
	for (LPBYTE base = (LPBYTE)si.lpMinimumApplicationAddress; base < maxA && found < 256; )
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0) break;
		LPBYTE regionBase = (LPBYTE)mbi.BaseAddress;
		SIZE_T regionSize = mbi.RegionSize;
		bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD) && regionSize >= 11 * sizeof(double))
		{
			LPBYTE end = regionBase + regionSize - 11 * sizeof(double);
			for (LPBYTE q = regionBase; q <= end; q += sizeof(double))
			{
				double* d = (double*)q;
				if (fabs(d[0] - R0[0]) > tol) continue;
				if (fabs(d[1] - R0[1]) < tol && fabs(d[2] - R0[2]) < tol &&
					fabs(d[4] - R0[3]) < tol && fabs(d[5] - R0[4]) < tol && fabs(d[6] - R0[5]) < tol &&
					fabs(d[8] - R0[6]) < tol && fabs(d[9] - R0[7]) < tol && fabs(d[10] - R0[8]) < tol)
				{
					s_renderCandidates.push_back(d);
					if (++found >= 256) break;
				}
			}
		}
		base = regionBase + regionSize;
	}
	IGCS::MessageHandler::logLine("RENDER SCAN: %d rotation copies match seeded rotation; divergent ones reported at frame 180", found);
}


namespace IGCS::GameSpecific::CameraManipulator
{
	void setVisualizerBase(LPBYTE base)
	{
		g_visualizerBase = base;

		if (base != nullptr)
		{
			LPVOID targetFunc = (LPVOID)(base + VISUALIZER_ROTATION_FUNC_OFFSET);
			IGCS::MessageHandler::logLine("Visualizer.dll base: %p, hooking rotation func at: %p",
				(void*)base, targetFunc);
			MH_STATUS status = MH_CreateHook(targetFunc, &hookedRotationFunc,
				reinterpret_cast<LPVOID*>(&g_originalRotationFunc));
			if (status == MH_OK)
			{
				MH_STATUS enableStatus = MH_EnableHook(targetFunc);
				if (enableStatus == MH_OK)
				{
					IGCS::MessageHandler::logLine("Visualizer rotation hook installed successfully!");
				}
				else
				{
					IGCS::MessageHandler::logError("MH_EnableHook failed with status: %d", (int)enableStatus);
				}
			}
			else
			{
				IGCS::MessageHandler::logError("MH_CreateHook failed with status: %d", (int)status);
			}

			// Also hook smCamera_Implement::SetPosition -- the render camera's per-frame pose
			// setter (exported, offset confirmed via PE export table). It fires in EVERY view mode
			// regardless of g_cameraEnabled, so the census/ROT logs reveal which camType is the
			// live view in F1/F2/F11. Write is gated off (g_driveRenderCam) -> log-only by default.
			LPVOID setPosFunc = (LPVOID)(base + SMCAMERA_SETPOS_OFFSET);
			IGCS::MessageHandler::logLine("Hooking smCamera_Implement::SetPosition at: %p", setPosFunc);
			MH_STATUS spStatus = MH_CreateHook(setPosFunc, &hookedSetPosition,
				reinterpret_cast<LPVOID*>(&g_originalSetPosition));
			if (spStatus == MH_OK)
			{
				if (MH_EnableHook(setPosFunc) == MH_OK)
					IGCS::MessageHandler::logLine("SetPosition hook installed (%s).",
						g_driveRenderCam ? "LOG+DRIVE" : "log-only");
				else
					IGCS::MessageHandler::logError("SetPosition MH_EnableHook failed");
			}
			else
			{
				IGCS::MessageHandler::logError("SetPosition MH_CreateHook failed with status: %d", (int)spStatus);
			}
		}
	}


	XMFLOAT3 getCurrentCameraCoords()
	{
		if (nullptr == g_cameraStructAddress) return XMFLOAT3(0, 0, 0);
		if (g_cameraEnabled && s_ownPositionInitialized)
		{
			// DCS X(fwd)->IGCS Z, DCS Y(up)->IGCS Y, DCS Z(right)->IGCS X
			return XMFLOAT3((float)s_ownPosition[2], (float)s_ownPosition[1], (float)s_ownPosition[0]);
		}
		double* pos = reinterpret_cast<double*>(g_cameraStructAddress + COORDS_IN_STRUCT_OFFSET);
		return XMFLOAT3((float)pos[2], (float)pos[1], (float)pos[0]);
	}


	void writeNewCameraValuesToGameData(XMVECTOR newLookQuaternion)
	{
		if (!isCameraFound()) return;

		double* pos = reinterpret_cast<double*>(g_cameraStructAddress + COORDS_IN_STRUCT_OFFSET);

		// Seed own position on first frame
		if (!s_ownPositionInitialized)
		{
			double* vizRot = getVisualRotationMatrix();
			if (vizRot != nullptr)
			{
				s_ownPosition[0] = vizRot[12];  // DCS X (forward)
				s_ownPosition[1] = vizRot[13];  // DCS Y (up)
				s_ownPosition[2] = vizRot[14];  // DCS Z (right)
			}
			else
			{
				s_ownPosition[0] = pos[0];
				s_ownPosition[1] = pos[1];
				s_ownPosition[2] = pos[2];
			}
			s_ownPositionInitialized = true;
			s_diagFrameCount = 0;
			s_diagRotLogged = 0;
			s_diagLastYaw = 0.0f;
			dumpEnableDiagnostics(pos, vizRot);

			// At this instant vizRot still holds the game's real rotation (we haven't written
			// ours yet). Scan memory for every copy of it; the render matrix is among them.
			s_renderTargets.clear();
			// Memory scan DISABLED for the un-skip experiment (was only feeding the removed
			// auto-write). Re-enable if we return to the find-the-render-matrix approach.
			//if (vizRot != nullptr)
			//{
			//	double R0[9] = { vizRot[0], vizRot[1], vizRot[2], vizRot[4], vizRot[5], vizRot[6], vizRot[8], vizRot[9], vizRot[10] };
			//	scanForRenderMatrix(R0);
			//}
		}

		// Write position from double-precision tracker
		pos[0] = s_ownPosition[0];
		pos[1] = s_ownPosition[1];
		pos[2] = s_ownPosition[2];

		// Write FOV to DCS.exe camera struct (degrees)
		double* fov = reinterpret_cast<double*>(g_cameraStructAddress + FOV_IN_STRUCT_OFFSET);
		*fov = g_customFoV * 57.2957795;

		// Write FOV to render FOV address (also degrees, captured by ASM hook)
		if (g_fovRenderAddress != nullptr)
		{
			double* renderFov = reinterpret_cast<double*>(g_fovRenderAddress);
			*renderFov = g_customFoV * 57.2957795;
		}

		// Convert quaternion to rotation matrix
		XMMATRIX rotMatrix = XMMatrixRotationQuaternion(newLookQuaternion);
		XMFLOAT4X4 rot;
		XMStoreFloat4x4(&rot, rotMatrix);

		// Write rotation to DCS.exe struct (+0xA0, stride 4 doubles per row)
		// Reverse axis swap: R_DCS[swap(i)][swap(j)] = R_IGCS[i][j]
		double* matBase = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
		matBase[0] = (double)rot._33;  matBase[1] = (double)rot._32;  matBase[2] = (double)rot._31;
		matBase[4] = (double)rot._23;  matBase[5] = (double)rot._22;  matBase[6] = (double)rot._21;
		matBase[8] = (double)rot._13;  matBase[9] = (double)rot._12;  matBase[10] = (double)rot._11;

		// Cache for Visualizer hook re-application
		s_lastWrittenRot[0] = matBase[0]; s_lastWrittenRot[1] = matBase[1]; s_lastWrittenRot[2] = matBase[2];
		s_lastWrittenRot[3] = matBase[4]; s_lastWrittenRot[4] = matBase[5]; s_lastWrittenRot[5] = matBase[6];
		s_lastWrittenRot[6] = matBase[8]; s_lastWrittenRot[7] = matBase[9]; s_lastWrittenRot[8] = matBase[10];

		// Expose to the WRITE1/WRITE2 ASM interceptors (pre-loaded into +0xA0 before they run).
		for (int i = 0; i < 9; i++) g_renderRot[i] = s_lastWrittenRot[i];

		// Write rotation + position to Visualizer.dll buffer
		double* vizRot = getVisualRotationMatrix();
		if (vizRot != nullptr)
		{
			vizRot[0]  = (double)rot._33;  vizRot[1]  = (double)rot._32;  vizRot[2]  = (double)rot._31;
			vizRot[4]  = (double)rot._23;  vizRot[5]  = (double)rot._22;  vizRot[6]  = (double)rot._21;
			vizRot[8]  = (double)rot._13;  vizRot[9]  = (double)rot._12;  vizRot[10] = (double)rot._11;
			vizRot[12] = s_ownPosition[0];
			vizRot[13] = s_ownPosition[1];
			vizRot[14] = s_ownPosition[2];

			// Cache for re-application after Visualizer hook calls original
			s_cachedVizValues[0]  = vizRot[0];  s_cachedVizValues[1]  = vizRot[1];  s_cachedVizValues[2]  = vizRot[2];
			s_cachedVizValues[4]  = vizRot[4];  s_cachedVizValues[5]  = vizRot[5];  s_cachedVizValues[6]  = vizRot[6];
			s_cachedVizValues[8]  = vizRot[8];  s_cachedVizValues[9]  = vizRot[9];  s_cachedVizValues[10] = vizRot[10];
			s_cachedVizValues[12] = vizRot[12]; s_cachedVizValues[13] = vizRot[13]; s_cachedVizValues[14] = vizRot[14];
			s_vizValuesCached = true;
		}

		// NOTE: auto-write to scanned render targets REMOVED -- writing noisy heap candidates
		// corrupted game state and crashed DCS. The renderer reads a matrix we can't safely
		// pattern-write; the proper fix is to find/hook its writer.
	}


	void updateCameraDataInGameData(Camera& camera)
	{
		if (!g_cameraEnabled) return;
		if (!isCameraFound()) return;

		XMVECTOR newLookQuaternion = camera.calculateLookQuaternion();

		// Use calculateNewCoords to get movement in IGCS space, then apply as double-precision delta
		XMFLOAT3 origin(0.0f, 0.0f, 0.0f);
		XMFLOAT3 moved = camera.calculateNewCoords(origin, newLookQuaternion);
		if (moved.x != 0.0f || moved.y != 0.0f || moved.z != 0.0f)
		{
			// IGCS: X=right, Y=up, Z=forward -> DCS: X=forward, Y=up, Z=right
			s_ownPosition[0] += (double)moved.z;  // DCS X (forward) += IGCS Z
			s_ownPosition[1] += (double)moved.y;  // DCS Y (up)
			s_ownPosition[2] += (double)moved.x;  // DCS Z (right) += IGCS X
		}

		writeNewCameraValuesToGameData(newLookQuaternion);

		// Per-enable trace: a few early frames + later checkpoints. vizHook advancing => the
		// Visualizer hook fires; moved!=0 => input is producing deltas; pos changing => writes land.
		s_diagFrameCount++;
		if (s_diagFrameCount <= 3 || s_diagFrameCount == 30 || s_diagFrameCount == 120)
		{
			MessageHandler::logLine("DIAG f%d: vizHook=%ld moved=(%.4f,%.4f,%.4f) vizCached=%d pos=(%.2f,%.2f,%.2f)",
				s_diagFrameCount, g_hookCallCount, moved.x, moved.y, moved.z,
				(int)s_vizValuesCached, s_ownPosition[0], s_ownPosition[1], s_ownPosition[2]);
		}

		// Rotation-write probe: fires only when yaw actually changes (you pressed a rotate key).
		// 'wrote' = heading cells we just wrote this frame; 'readback' = same cells as the game
		// left them at the last hook entry. If readback != wrote, the game is overwriting our
		// rotation (render uses a source we don't control). If they match but the view doesn't
		// turn, the render reads rotation from somewhere other than this buffer.
		float curYaw = camera.getYaw();
		if (fabsf(curYaw - s_diagLastYaw) > 1e-6f && s_diagRotLogged < 20)
		{
			s_diagRotLogged++;
			double* vr = getVisualRotationMatrix();
			MessageHandler::logLine("DIAG ROT f%d: yaw=%.4f wrote[0,2]=(%.4f,%.4f) readback[0,2]=(%.4f,%.4f)",
				s_diagFrameCount, curYaw,
				vr ? vr[0] : 0.0, vr ? vr[2] : 0.0,
				s_diagReadbackRot[0], s_diagReadbackRot[2]);
		}
		s_diagLastYaw = curYaw;

		// Log the active view camera (base + vtable) whenever it changes -> confirms which
		// vCamera type each mode uses (cockpit/external/freecam) and that we're not hitting
		// mirror/sub cameras.
		LPBYTE activeMat = (LPBYTE)g_activeCameraMat;
		if (activeMat != nullptr)
		{
			LPBYTE camBase = activeMat - MATRIX_IN_STRUCT_OFFSET;
			static LPBYTE s_lastLoggedCamBase = nullptr;
			if (camBase != s_lastLoggedCamBase)
			{
				s_lastLoggedCamBase = camBase;
				void* vtbl = *reinterpret_cast<void**>(camBase);
				HMODULE hm = nullptr; char modname[MAX_PATH] = { 0 };
				unsigned long long off = 0;
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)vtbl, &hm) && hm != nullptr)
				{
					GetModuleFileNameA(hm, modname, MAX_PATH);
					off = (unsigned long long)((LPBYTE)vtbl - (LPBYTE)hm);
				}
				const char* bn = strrchr(modname, '\\'); bn = bn ? bn + 1 : modname;
				MessageHandler::logLine("ACTIVE CAMERA: base=%p vtable=%s+0x%llX", (void*)camBase, bn, off);
			}
		}

		// Frame 180: report the scanned rotation copies that did NOT follow our rotation.
		// Those are matrices the renderer reads but we don't drive -> render + HUD candidates.
		if (s_diagFrameCount == 180 && !s_renderCandidates.empty())
		{
			double* viz = getVisualRotationMatrix();
			double* matBase = (g_cameraStructAddress != nullptr)
				? reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET) : nullptr;
			int reported = 0;
			for (double* d : s_renderCandidates)
			{
				if (d == viz || d == matBase) continue;
				double cur[9] = { d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10] };
				bool diverged = false;
				for (int i = 0; i < 9; i++) if (fabs(cur[i] - s_lastWrittenRot[i]) > 1e-2) { diverged = true; break; }
				if (!diverged) continue;

				// Divergent AND a valid rotation matrix -> a render matrix to drive (skip garbage).
				if (plausibleRot(d)) s_renderTargets.push_back(d);

				HMODULE hm = nullptr; char modname[MAX_PATH] = { 0 };
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)d, &hm) && hm != nullptr)
				{
					GetModuleFileNameA(hm, modname, MAX_PATH);
					const char* bn = strrchr(modname, '\\'); bn = bn ? bn + 1 : modname;
					MessageHandler::logLine("RENDER CANDIDATE %s+0x%llX cur=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]",
						bn, (unsigned long long)((LPBYTE)d - (LPBYTE)hm),
						cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
				}
				else
				{
					MessageHandler::logLine("RENDER CANDIDATE heap %p cur=[%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]",
						(void*)d, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
				}
				if (++reported >= 30) break;
			}
			MessageHandler::logLine("RENDER SCAN: ours[0..2]=%.3f %.3f %.3f ; %d divergent reported ; now driving %d render target(s)",
				s_lastWrittenRot[0], s_lastWrittenRot[1], s_lastWrittenRot[2], reported, (int)s_renderTargets.size());
		}
	}


	void applySettingsToGameState() {}

	void displayResolution(int width, int height)
	{
		MessageHandler::logDebug("Received Resolution: %i x %i", width, height);
	}

	void setResolution(int width, int height)
	{
		if (width <= 0 || height <= 0) return;
		HWND hwnd = Globals::instance().mainWindowHandle();
		if (hwnd == nullptr) return;

		MessageHandler::logLine("Hotsampling: resizing to %d x %d", width, height);
		LONG style = GetWindowLongPtr(hwnd, GWL_STYLE);
		LONG exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
		RECT rect = { 0, 0, width, height };
		AdjustWindowRectEx(&rect, style, FALSE, exStyle);
		SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void cachetimespeed() {}
	void cacheslowmospeed() {}
	void setTimeStopValue(bool pauseGame, bool slowmoEnabled) {}
	void setSlowMo(float amount, bool slowMo, bool gamepaused) {}


	void resetFoV(GameCameraData& cachedData)
	{
		if (nullptr == g_cameraStructAddress) return;
		double* fov = reinterpret_cast<double*>(g_cameraStructAddress + FOV_IN_STRUCT_OFFSET);
		*fov = cachedData._fov;
		g_customFoV = cachedData._fov * 0.0174532925;  // degrees -> radians
		g_customFovDegrees = cachedData._fov;
	}

	void changeFoV(float amount)
	{
		g_customFoV += (double)amount;
		g_customFovDegrees = g_customFoV * 57.2957795;
	}

	float getCurrentFoV()
	{
		return (float)g_customFoV;
	}

	bool isCameraFound()
	{
		return nullptr != g_cameraStructAddress;
	}

	void displayAddresses()
	{
		MessageHandler::logDebug("Camera struct address: %p", (void*)g_cameraStructAddress);
		MessageHandler::logDebug("Visualizer.dll base: %p", (void*)g_visualizerBase);
		MessageHandler::logDebug("Visualizer rotation buffer: %p", (void*)getVisualRotationMatrix());
		MessageHandler::logDebug("Visualizer hook call count: %ld", g_hookCallCount);
		MessageHandler::logLine("FOV hook call count: %u", g_fovHookCallCount);
		MessageHandler::logLine("Render FOV address: %p", (void*)g_fovRenderAddress);
		MessageHandler::logLine("IGCS custom FOV: %.4f rad (%.2f deg)", g_customFoV, g_customFoV * 57.2957795);
	}


	void restoreGameCameraData(GameCameraData& source)
	{
		if (!isCameraFound()) return;
		s_ownPositionInitialized = false;
		s_vizValuesCached = false;
		s_igcsSessionPositionCached = false;

		// Restore rotation matrix (doubles, stride 4 doubles per row)
		double* matBase = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
		for (int i = 0; i < MATRIX_SIZE; i++)
			matBase[i] = source._matrix[i];

		// Restore position (doubles)
		double* pos = reinterpret_cast<double*>(g_cameraStructAddress + COORDS_IN_STRUCT_OFFSET);
		for (int i = 0; i < COORD_SIZE; i++)
			pos[i] = source._coords[i];

		// Restore FOV (double, degrees)
		double* fov = reinterpret_cast<double*>(g_cameraStructAddress + FOV_IN_STRUCT_OFFSET);
		*fov = source._fov;
	}


	void cacheGameCameraData(GameCameraData& destination)
	{
		if (!isCameraFound()) return;

		// Cache rotation (stride 4 doubles per row, 3 values per row, 3 rows = 12 values)
		double* matBase = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
		for (int i = 0; i < MATRIX_SIZE; i++)
			destination._matrix[i] = matBase[i];

		// Cache position
		double* pos = reinterpret_cast<double*>(g_cameraStructAddress + COORDS_IN_STRUCT_OFFSET);
		for (int i = 0; i < COORD_SIZE; i++)
			destination._coords[i] = pos[i];

		// Cache FOV and initialize tracked FOV (game stores degrees, we track radians)
		double* fov = reinterpret_cast<double*>(g_cameraStructAddress + FOV_IN_STRUCT_OFFSET);
		destination._fov = *fov;
		g_customFoV = *fov * 0.0174532925;  // degrees -> radians
		g_customFovDegrees = *fov;
	}


	void cacheGameAddresses(GameAddressData& destination)
	{
		destination.cameraAddress = g_cameraStructAddress;
	}


	void cacheigcsData(Camera& cam, igcsSessionCacheData& igcscache)
	{
		igcscache.eulers.x = cam.getPitch();
		igcscache.eulers.y = cam.getYaw();
		igcscache.eulers.z = cam.getRoll();
		igcscache.Coordinates = getCurrentCameraCoords();
		igcscache.quaternion = cam.getToolsQuaternion();
		igcscache.fov = getCurrentFoV();
		s_igcsSessionStartPos[0] = s_ownPosition[0];
		s_igcsSessionStartPos[1] = s_ownPosition[1];
		s_igcsSessionStartPos[2] = s_ownPosition[2];
		s_igcsSessionPositionCached = true;
	}


	void restoreigcsData(Camera& cam, igcsSessionCacheData& igcscache)
	{
		cam.setPitch(igcscache.eulers.x);
		cam.setYaw(igcscache.eulers.y);
		cam.setRoll(igcscache.eulers.z);
		restoreCurrentCameraCoords(igcscache.Coordinates);
		restoreFOV(igcscache.fov);
		s_igcsSessionPositionCached = false;
	}


	void restoreCurrentCameraCoords(XMFLOAT3 coordstorestore)
	{
		if (nullptr == g_cameraStructAddress) return;
		double* pos = reinterpret_cast<double*>(g_cameraStructAddress + COORDS_IN_STRUCT_OFFSET);

		if (s_igcsSessionPositionCached)
		{
			pos[0] = s_igcsSessionStartPos[0];
			pos[1] = s_igcsSessionStartPos[1];
			pos[2] = s_igcsSessionStartPos[2];
		}
		else
		{
			// IGCS order (X=right, Y=up, Z=forward) -> DCS (X=forward, Y=up, Z=right)
			pos[0] = (double)coordstorestore.z;
			pos[1] = (double)coordstorestore.y;
			pos[2] = (double)coordstorestore.x;
		}
		s_ownPosition[0] = pos[0];
		s_ownPosition[1] = pos[1];
		s_ownPosition[2] = pos[2];
	}


	void restoreFOV(float fov)
	{
		if (nullptr == g_cameraStructAddress) return;
		double* fovAddr = reinterpret_cast<double*>(g_cameraStructAddress + FOV_IN_STRUCT_OFFSET);
		*fovAddr = (double)fov;
	}


	float fovinDegrees(float fov)
	{
		return fov * (180.0f / XM_PI);
	}

	float fovinRadians(float fov)
	{
		return fov * (XM_PI / 180.0f);
	}


	const LPBYTE getCameraStruct()
	{
		return g_cameraStructAddress;
	}


	void setMatrixRotationVectors(Camera& camera)
	{
		if (nullptr == g_cameraStructAddress) return;

		Vector3 rV, uV, fV, eulers;
		Vector4 quat;

		XMMATRIX m = readDcsRotationMatrix();
		XMVECTOR q = XMQuaternionRotationMatrix(m);

		// Use SimpleMath ToEuler for consistency with the framework
		Matrix sM = (Matrix)m;
		eulers = sM.ToEuler();

		XMStoreFloat3(&rV, m.r[0]);
		XMStoreFloat3(&uV, m.r[1]);
		XMStoreFloat3(&fV, m.r[2]);
		XMStoreFloat4(&quat, q);

		camera.setRightVector(rV);
		camera.setUpVector(uV);
		camera.setForwardVector(fV);
		camera.setGameQuaternion(quat);
		camera.setGameEulers(eulers);
	}


	// Seed the camera's pitch/yaw/roll from the current game rotation so the freecam
	// starts from the current view instead of snapping to the default orientation.
	//
	// Uses a custom Euler decomposition matched exactly to calculateLookQuaternion's
	// composition order (M = Rz(roll) * Rx(pitch) * Ry(yaw), row-vector convention).
	// SimpleMath::ToEuler uses a different order and won't round-trip cleanly through
	// calculateLookQuaternion, producing visible floating-point drift on seed.
	void seedCameraFromCurrentRotation(Camera& camera)
	{
		if (nullptr == g_cameraStructAddress) return;

		// Seed from the LIVE render view: the 0x2B1278 Visualizer cache holds the render's current
		// r8 -- the actual on-screen view for whatever mode (F1/F2/F11) you're in. The DCS struct
		// +0xA0 is STALE here (it holds the last freecam orientation -- logged the same yaw every
		// enable), which made enabling snap to the freecam view instead of the current one. Safe to
		// read: runs before g_cameraEnabled flips, so our r8 override hasn't written the cache yet.
		// Cache is the same P*R*P basis as +0xA0, so the [10,9,8 / 6,5,4 / 2,1,0] un-swap applies
		// identically. Fall back to +0xA0 if the cache pointer isn't available.
		double* src = getVisualRotationMatrix();
		if (nullptr == src) src = reinterpret_cast<double*>(g_cameraStructAddress + MATRIX_IN_STRUCT_OFFSET);
		XMMATRIX m(
			(float)src[10], (float)src[9], (float)src[8], 0.0f,
			(float)src[6],  (float)src[5], (float)src[4], 0.0f,
			(float)src[2],  (float)src[1], (float)src[0], 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);
		XMFLOAT4X4 mf;
		XMStoreFloat4x4(&mf, m);

		// From M = Rz * Rx * Ry:
		//   _32 = -sin(pitch)
		//   _31 = cos(pitch)*sin(yaw), _33 = cos(pitch)*cos(yaw)
		//   _12 = sin(roll)*cos(pitch), _22 = cos(roll)*cos(pitch)
		float s = -mf._32; if (s > 1.0f) s = 1.0f; if (s < -1.0f) s = -1.0f;  // clamp for asinf
		float pitch = asinf(s);
		float yaw   = atan2f(mf._31, mf._33);
		float roll  = atan2f(mf._12, mf._22);

		// calculateLookQuaternion applies -pitch and -roll internally, so negate here
		// to cancel out and reproduce the captured matrix on the first frame.
		camera.setPitch(-pitch);
		camera.setYaw(yaw);
		camera.setRoll(-roll);

		MessageHandler::logLine("SEED from live viz: pitch=%.4f yaw=%.4f roll=%.4f | src[0,1,2]=%.4f %.4f %.4f",
			-pitch, yaw, -roll, src[0], src[1], src[2]);
	}
}
