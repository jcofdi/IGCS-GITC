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
#include "InterceptorHelper.h"
#include "GameConstants.h"
#include "GameImageHooker.h"
#include <map>
#include <cstdio>
#include <cstring>
#include "MessageHandler.h"
#include "CameraManipulator.h"
#include "Globals.h"

using namespace std;

//--------------------------------------------------------------------------------------------------------------------------------
// external asm functions
extern "C" {
	void cameraStructInterceptor();
	void cameraWrite1Interceptor();
	void cameraWrite2Interceptor();
	void cameraFovWriteInterceptor();
}

// external addresses used in asm.
extern "C" {
	LPBYTE _cameraStructInterceptionContinue = nullptr;
	LPBYTE _cameraWrite1InterceptionContinue = nullptr;
	LPBYTE _cameraWrite2InterceptionContinue = nullptr;
	LPBYTE _cameraFovWriteInterceptionContinue = nullptr;
}


namespace IGCS::GameSpecific::InterceptorHelper
{
	void initializeAOBBlocks(LPBYTE hostImageAddress, DWORD hostImageSize, map<string, AOBBlock*> &aobBlocks)
	{
		// 1. Camera position write block (also captures RBX as struct base)
		//   movsd [rbx+100h], xmm9   movsd [rbx+108h], xmm6   addsd xmm10,[rbp-50h]   movsd [rbx+110h], xmm10
		aobBlocks[CAMERA_ADDRESS_INTERCEPT_KEY] = new AOBBlock(CAMERA_ADDRESS_INTERCEPT_KEY,
			"F2 44 0F 11 8B 00 01 00 00 F2 0F 11 B3 08 01 00 00 F2 44 0F 58 55 B0 F2 44 0F 11 93 10 01 00 00", 1);

		// 2. Rotation function A entry (yaw) - hardcoded offset from DCS.exe base
		//    AOB has 9+ matches and occurrence ordering is unstable across updates.
		//    Address confirmed via breakpoint: DCS.exe+659200
		aobBlocks[CAMERA_WRITE1_INTERCEPT_KEY] = new AOBBlock(CAMERA_WRITE1_INTERCEPT_KEY, "CC", 1);
		aobBlocks[CAMERA_WRITE1_INTERCEPT_KEY]->storeFoundLocation(hostImageAddress + 0x659200);

		// 3. Rotation function B entry (pitch) - hardcoded offset from DCS.exe base
		//    Address confirmed via breakpoint: DCS.exe+659050
		aobBlocks[CAMERA_WRITE2_INTERCEPT_KEY] = new AOBBlock(CAMERA_WRITE2_INTERCEPT_KEY, "CC", 1);
		aobBlocks[CAMERA_WRITE2_INTERCEPT_KEY]->storeFoundLocation(hostImageAddress + 0x659050);

		// 4. FOV clamp+write - maxsd xmm0,[rax]; minsd xmm0,xmm1; movsd [rax],xmm0; mov rax,[rdi]
		aobBlocks[FOV_WRITE_INTERCEPT_KEY] = new AOBBlock(FOV_WRITE_INTERCEPT_KEY,
			"F2 0F 5F 00 F2 0F 5D C1 F2 0F 11 00 48 8B 07 F2 0F 10 00", 1);

		map<string, AOBBlock*>::iterator it;
		bool result = true;
		for (it = aobBlocks.begin(); it != aobBlocks.end(); it++)
		{
			// Skip scan for blocks with pre-set addresses (hardcoded offsets)
			if (it->second->locationInImage() != nullptr)
			{
				MessageHandler::logLine("AOB [%s] HARDCODED at %p", it->first.c_str(), (void*)it->second->locationInImage());
				continue;
			}
			bool found = it->second->scan(hostImageAddress, hostImageSize);
			result &= found;
			if (found)
			{
				MessageHandler::logLine("AOB [%s] FOUND at %p", it->first.c_str(), (void*)it->second->locationInImage());
			}
			else
			{
				MessageHandler::logError("AOB [%s] NOT FOUND", it->first.c_str());
			}
		}
		if (result)
		{
			MessageHandler::logLine("All interception offsets found.");
		}
		else
		{
			MessageHandler::logError("One or more interception offsets weren't found: tools aren't compatible with this game's version.");
		}
	}


	void setCameraStructInterceptorHook(map<string, AOBBlock*> &aobBlocks)
	{
		// Hook all 4 position instructions (32 bytes = 0x20)
		GameImageHooker::setHook(aobBlocks[CAMERA_ADDRESS_INTERCEPT_KEY], 0x20, &_cameraStructInterceptionContinue, &cameraStructInterceptor);
	}


	// Reads the bytes at the hook target and compares against the prologue the ASM interceptor
	// will replay. On mismatch the hardcoded offset is stale (DCS update moved/changed the
	// function): log the diff LOUDLY and REFUSE to hook, so a bad offset can't crash the game.
	// Returns true only if the signature matched and the hook was installed.
	static bool validateAndHookEntry(const char* name, AOBBlock* block, const uint8_t* expected,
		int len, LPBYTE* continuePtr, void* asmFunc)
	{
		LPBYTE loc = (block != nullptr) ? block->locationInImage() : nullptr;
		if (loc == nullptr)
		{
			MessageHandler::logError("HOOK [%s] target is null; not hooking.", name);
			return false;
		}
		BYTE found[32] = { 0 };
		GameImageHooker::readRange(loc, found, len);
		if (memcmp(found, expected, len) != 0)
		{
			char expHex[128] = { 0 };
			char fndHex[128] = { 0 };
			for (int i = 0; i < len; i++)
			{
				sprintf_s(expHex + (i * 3), sizeof(expHex) - (i * 3), "%02X ", expected[i]);
				sprintf_s(fndHex + (i * 3), sizeof(fndHex) - (i * 3), "%02X ", found[i]);
			}
			MessageHandler::logError("HOOK [%s] SIGNATURE MISMATCH at %p -- offset is STALE (DCS update).", name, (void*)loc);
			MessageHandler::logError("  expected: %s", expHex);
			MessageHandler::logError("  found:    %s", fndHex);
			MessageHandler::logError("  REFUSING to hook [%s]. Re-find this function's offset and update InterceptorHelper.", name);
			return false;
		}
		MessageHandler::logLine("HOOK [%s] signature OK at %p", name, (void*)loc);
		GameImageHooker::setHook(block, len, continuePtr, asmFunc);
		return true;
	}


	void setPostCameraStructHooks(map<string, AOBBlock*> &aobBlocks)
	{
		// Expected entry prologues (14 bytes). The ASM interceptors replay these exact bytes,
		// so a mismatch means the replay would corrupt registers -> crash. Validate first.
		// WRITE1 (yaw):  push rbx; sub rsp,40h; movaps [rsp+30h],xmm6; mov rbx,rcx
		static const uint8_t write1Sig[14] = { 0x40,0x53,0x48,0x83,0xEC,0x40,0x0F,0x29,0x74,0x24,0x30,0x48,0x8B,0xD9 };
		// WRITE2 (pitch): push rbx; sub rsp,40h; movaps [rsp+30h],xmm6; movaps xmm0,xmm1
		static const uint8_t write2Sig[14] = { 0x40,0x53,0x48,0x83,0xEC,0x40,0x0F,0x29,0x74,0x24,0x30,0x0F,0x28,0xC1 };

		validateAndHookEntry("WRITE1", aobBlocks[CAMERA_WRITE1_INTERCEPT_KEY], write1Sig, 14,
			&_cameraWrite1InterceptionContinue, &cameraWrite1Interceptor);

		validateAndHookEntry("WRITE2", aobBlocks[CAMERA_WRITE2_INTERCEPT_KEY], write2Sig, 14,
			&_cameraWrite2InterceptionContinue, &cameraWrite2Interceptor);

		// Hook FOV clamp+write (15 bytes = 0x0F)
		if (aobBlocks[FOV_WRITE_INTERCEPT_KEY]->locationInImage() != nullptr)
		{
			GameImageHooker::setHook(aobBlocks[FOV_WRITE_INTERCEPT_KEY], 0x0F, &_cameraFovWriteInterceptionContinue, &cameraFovWriteInterceptor);
		}
		else
		{
			MessageHandler::logLine("FOV hook skipped (AOB not found).");
		}
	}


	void toggleHud(map<string, AOBBlock*>& aobBlocks, bool hudVisible) {}
	void togglePause(map<string, AOBBlock*>& aobBlocks, bool enabled) {}
	void getAbsoluteAddresses(map<string, AOBBlock*>& aobBlocks) {}
	void cameraSetup(map<string, AOBBlock*>& aobBlocks, bool enabled) {}
	void toolsInit(map<string, AOBBlock*>& aobBlocks) {}
	void handleSettings(map<string, AOBBlock*>& aobBlocks) {}
}
