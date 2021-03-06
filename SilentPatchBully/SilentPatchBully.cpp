#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define WINVER 0x0502
#define _WIN32_WINNT 0x0502

#include <windows.h>
#include "MemoryMgr.h"

#include <cassert>

#include <Shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

#ifndef NDEBUG

#define INCLUDE_MEMORY_CHECKS 1

#else

#define INCLUDE_MEMORY_CHECKS 0

#endif


static HINSTANCE hDLLModule;

namespace FixedAllocators
{
	void InitMemoryMgr()
	{
		// Do nothing
	}

	void ShutDownMemoryMgr()
	{
		// Do nothing
	}

#if INCLUDE_MEMORY_CHECKS
	static constexpr size_t MEMORY_PROLOGUE_SIZE = sizeof(size_t) + sizeof(uint32_t);
	static constexpr size_t MEMORY_EPILOGUE_SIZE = sizeof(uint32_t);
	static constexpr size_t MEMORY_CANARIES_TOTAL_SIZE = MEMORY_PROLOGUE_SIZE + MEMORY_EPILOGUE_SIZE;

	static constexpr uint32_t MEMORY_CANARY = 0xFDFDFDFD;
#endif

	void* MemoryMgrMalloc( size_t size )
	{
		if ( size == 0 )
		{
			return nullptr;
		}

		// Their malloc is actually calloc, as allocated memory gets zeroed
#if INCLUDE_MEMORY_CHECKS
		// Debug memory is structured as follows:
		// Allocated size
		// FDFDFDFD
		// Allocated space
		// FDFDFDFD

		void* memory = calloc( size + MEMORY_CANARIES_TOTAL_SIZE, 1 );
		assert( memory != nullptr );

		uintptr_t memStart = uintptr_t(memory);
		*(size_t*)memStart = size;
		*(uint32_t*)( memStart + sizeof(size_t) ) = MEMORY_CANARY;
		*(uint32_t*)( memStart + MEMORY_PROLOGUE_SIZE + size ) = MEMORY_CANARY;

		return (void*)( memStart + MEMORY_PROLOGUE_SIZE );
#else
		return calloc( size, 1 );
#endif
	}

	void MemoryMgrFree( void* data )
	{
		if ( data != nullptr )
		{
#if INCLUDE_MEMORY_CHECKS
			uintptr_t mem = uintptr_t(data);
			uint32_t startCanary = *(uint32_t*)(mem - sizeof(uint32_t));
			assert( startCanary == MEMORY_CANARY );

			// If start canary is valid, we can check the end canary (since size is probably valid too)
			size_t size = *(size_t*)(mem - MEMORY_PROLOGUE_SIZE);
			uint32_t endCanary = *(uint32_t*)(mem + size);
			assert( endCanary == MEMORY_CANARY );

			free( (void*)(mem - MEMORY_PROLOGUE_SIZE) );
#else
			free( data );
#endif
		}
	}

	void* RwMallocAlign( size_t size, size_t align )
	{
		if ( size == 0 )
		{
			return nullptr;
		}
		void* memory = _aligned_malloc( size, align );
		assert( memory != nullptr );
		return memory;
	}

	void RwFreeAlign( void* data )
	{
		if ( data != nullptr )
		{
			_aligned_free( data );
		}
	}

	void OperatorDelete_Safe( void** pData )
	{
		if ( *pData != nullptr )
		{
			MemoryMgrFree( *pData );
			*pData = nullptr;
		}
	}

	void __stdcall MemoryHeap_Free( void* data )
	{
		MemoryMgrFree( data );
	}

	void* __stdcall MemoryHeap_MoveMemoryBully( void* data )
	{
		// Do NOT move
		return data;
	}

	size_t __stdcall MemoryHeap_GetMemoryUsed( int )
	{
		return 0;
	}

};



namespace FrameTimingFix
{
	void (*orgUpdateTimer)(bool);
	void UpdateTimerAndSleep( bool captureInput )
	{
		orgUpdateTimer( captureInput );
		Sleep( 100 );
	}

}

void InjectHooks()
{
	using namespace Memory;

	// If it's not 1.200, bail out
	if ( !MemEquals( 0x860C6B, { 0xC7, 0x45, 0xFC, 0xFE, 0xFF, 0xFF, 0xFF } ) )
	{
#ifndef _DEBUG
		MessageBoxW( nullptr, L"You are using an executable version not supported by SilentPatch (most likely 1.154)!\n\n"
			L"I strongly recommend obtaining a 1.200 executable - if you are using a retail version, just download an official 1.200 patch; "
			L"if you are using a Steam version, verify your game's files (since by default Steam uses 1.200).",
			L"SilentPatch", MB_OK | MB_ICONWARNING );
#endif
		return;
	}

	std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule( GetModuleHandle( nullptr ), ".text" );

	// Obtain a path to the ASI
	wchar_t			wcModulePath[MAX_PATH];
	GetModuleFileNameW(hDLLModule, wcModulePath, _countof(wcModulePath) - 3); // Minus max required space for extension
	PathRenameExtensionW(wcModulePath, L".ini");

	// Replaced custom CMemoryHeap with regular CRT functions (like in GTA)
	{
		using namespace FixedAllocators;

		InjectHook( 0x5EE630, InitMemoryMgr, PATCH_JUMP );
		InjectHook( 0x5EE5A0, ShutDownMemoryMgr, PATCH_JUMP );		
		InjectHook( 0x5EE830, MemoryMgrMalloc, PATCH_JUMP );
		InjectHook( 0x5EE940, MemoryMgrFree, PATCH_JUMP );
		InjectHook( 0x5EE9C0, RwMallocAlign, PATCH_JUMP );
		// 0x5EEA50 - RwMemoryMgrMalloc - jumps to MemoryMgrMalloc
		// 0x5EEA60 - RwMemoryMgrFree - jumps to MemoryMgrFree
		InjectHook( 0x5EEA70, RwFreeAlign, PATCH_JUMP );

		InjectHook( 0x5EEEF0, MemoryHeap_Free, PATCH_JUMP );
		InjectHook( 0x5EF4D0, MemoryHeap_MoveMemoryBully, PATCH_JUMP );

		InjectHook( 0x5EEDD0, MemoryHeap_GetMemoryUsed, PATCH_JUMP );

		// Fixed CPedType::Shutdown (zero pointers to prevent a double free)
		Patch<uint8_t>( 0x499CD8, 0x56 );
		InjectHook( 0x499CD9, OperatorDelete_Safe );

		// Don't call cSCREAMAudioManager::CleanupAfterMission from cSCREAMAudioManager::Terminate (used already freed memory)
		Nop( 0x5963C3, 5 );

		// Write a pointer to fake 'upper memory bound' so CStreaming::MakeSpaceFor is pleased
		static const uintptr_t FAKE_MAX_MEMORY = 0x7FFFFFFF;
		Patch( 0xD141A8, &FAKE_MAX_MEMORY );
	}


	// Fixed a crash in CFileLoader::LoadCollisionModel occuring with a replaced allocator
	// Fixed COLL vertex loading
	Patch<uint8_t>( 0x42BE80 + 2, 16 );

	{
		using namespace FrameTimingFix;

		// DO NOT sleep when limiting FPS in game...
		Nop( 0x4061C4, 2 + 6 );

		// ...sleep for 100ms periodically when minimized
		ReadCall( 0x43D660, orgUpdateTimer );
		InjectHook( 0x43D660, UpdateTimerAndSleep );

		// Because we're doing a busy loop now, 31FPS cap can now become a 30FPS cap
		if ( const int INIoption = GetPrivateProfileIntW(L"SilentPatch", L"FPSLimit", -1, wcModulePath); INIoption != -1 )
		{
			Patch<int32_t>( 0x40618F + 1, INIoption > 0 ? INIoption : INT_MAX );
		}

		// Revert code changes 60FPS EXE does, we don't need them anymore
		Patch<int8_t>( 0x4061BE + 1, 0x4 );
		Patch<uint8_t>( 0x4061C2, 0x73 );
	}

	// Remove FILE_FLAG_NO_BUFFERING from CdStreams
	Patch<uint32_t>( 0x73ABEA + 6, FILE_FLAG_OVERLAPPED );


	// Fixed crash in Nutcracking
	// Consistently treat playercount as ID, not actual size
	Nop( 0x6FB302, 6 );
	Nop( 0x6FB3EB, 6 );
	Nop( 0x6FC920, 2 );
	Nop( 0x6FC945, 2 );
	Nop( 0x6FC94F, 2 );
	Nop( 0x6FC97C, 2 );
	Nop( 0x6FCE91, 2 );
}

static void ProcHook()
{
	static bool		bPatched = false;
	if ( !bPatched )
	{
		bPatched = true;

		InjectHooks();
	}
}

static uint8_t orgCode[5];
static decltype(SystemParametersInfoA)* pOrgSystemParametersInfoA;
BOOL WINAPI SystemParametersInfoA_Hook( UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni )
{
	ProcHook();
	return pOrgSystemParametersInfoA( uiAction, uiParam, pvParam, fWinIni );
}

BOOL WINAPI SystemParametersInfoA_OverwritingHook( UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni )
{
	Memory::VP::Patch( pOrgSystemParametersInfoA, { orgCode[0], orgCode[1], orgCode[2], orgCode[3], orgCode[4] } );
	return SystemParametersInfoA_Hook( uiAction, uiParam, pvParam, fWinIni );
}

static bool PatchIAT()
{
	HINSTANCE					hInstance = GetModuleHandle(nullptr);
	PIMAGE_NT_HEADERS			ntHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)hInstance + ((PIMAGE_DOS_HEADER)hInstance)->e_lfanew);

	// Find IAT	
	PIMAGE_IMPORT_DESCRIPTOR	pImports = (PIMAGE_IMPORT_DESCRIPTOR)((DWORD_PTR)hInstance + ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	// Find user32.dll
	for ( ; pImports->Name != 0; pImports++ )
	{
		if ( !_stricmp((const char*)((DWORD_PTR)hInstance + pImports->Name), "USER32.DLL") )
		{
			if ( pImports->OriginalFirstThunk != 0 )
			{
				PIMAGE_IMPORT_BY_NAME*		pFunctions = (PIMAGE_IMPORT_BY_NAME*)((DWORD_PTR)hInstance + pImports->OriginalFirstThunk);

				// user32.dll found, find SystemParametersInfoA
				for ( ptrdiff_t j = 0; pFunctions[j] != nullptr; j++ )
				{
					if ( !strcmp((const char*)((DWORD_PTR)hInstance + pFunctions[j]->Name), "SystemParametersInfoA") )
					{
						// Overwrite the address with the address to a custom SystemParametersInfoA
						DWORD			dwProtect[2];
						DWORD_PTR*		pAddress = &((DWORD_PTR*)((DWORD_PTR)hInstance + pImports->FirstThunk))[j];

						VirtualProtect(pAddress, sizeof(DWORD_PTR), PAGE_EXECUTE_READWRITE, &dwProtect[0]);
						pOrgSystemParametersInfoA = **(decltype(pOrgSystemParametersInfoA)*)pAddress;
						*pAddress = (DWORD_PTR)SystemParametersInfoA_Hook;
						VirtualProtect(pAddress, sizeof(DWORD_PTR), dwProtect[0], &dwProtect[1]);

						return true;
					}
				}
			}
		}
	}
	return false;
}

static bool PatchIAT_ByPointers()
{
	pOrgSystemParametersInfoA = SystemParametersInfoA;
	memcpy( orgCode, pOrgSystemParametersInfoA, sizeof(orgCode) );
	Memory::VP::InjectHook( pOrgSystemParametersInfoA, SystemParametersInfoA_OverwritingHook, PATCH_JUMP );
	return true;
}

static void InstallHooks()
{
	bool getStartupInfoHooked = PatchIAT();
	if ( !getStartupInfoHooked )
	{
		PatchIAT_ByPointers();
	}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	UNREFERENCED_PARAMETER(hinstDLL);
	UNREFERENCED_PARAMETER(lpvReserved);

	if ( fdwReason == DLL_PROCESS_ATTACH )
	{
		hDLLModule = hinstDLL;
		InstallHooks();
	}
	return TRUE;
}