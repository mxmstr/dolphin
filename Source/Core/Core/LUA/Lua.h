// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <mutex>
#include <deque>
#include <map>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "InputCommon/GCPadStatus.h"

extern struct lua_State;

typedef int                 BOOL;
int ReadValue8(lua_State *L);
int ReadValue16(lua_State *L);
int ReadValue32(lua_State *L);
int ReadValueFloat(lua_State *L);
int ReadValueString(lua_State *L);
int WriteValue8(lua_State *L);
int WriteValue16(lua_State *L);
int WriteValue32(lua_State *L);
int WriteValueFloat(lua_State *L);
int WriteValueString(lua_State *L);
int WriteAssembly(lua_State *L);
int GetPointerNormal(lua_State *L);
int GetGameID(lua_State *L);
int PressButton(lua_State *L);
int ReleaseButton(lua_State *L);
int SetMainStickX(lua_State *L);
int SetMainStickY(lua_State *L);
int SetCStickX(lua_State *L);
int SetCStickY(lua_State *L);
int SaveState(lua_State *L);
int LoadState(lua_State *L);
int GetFrameCount(lua_State *L);
int GetInputFrameCount(lua_State *L);
int SetScreenText(lua_State *L);
int PauseEmulation(lua_State *L);
int SetInfoDisplay(lua_State *L);
int MsgBox(lua_State *L);
int ConsoleLog(lua_State *L);
int CancelScript(lua_State *L);
void HandleLuaErrors(lua_State *L, int status);
int RegisterMemoryCallback(lua_State* L);
int UnregisterMemoryCallback(lua_State* L);
int VROverlayDrawSprite(lua_State* L);

namespace
{
void GetFileListing(const File::FSTEntry& entry, std::vector<std::string>& files)
{
    if (entry.isDirectory)
    {
        for (const auto& child : entry.children)
        {
            GetFileListing(child, files);
        }
    }
    else
    {
        files.push_back(entry.physicalName);
    }
}
}

namespace Lua
{
	struct StateEvent
	{
		bool doSave = false;
		bool useSlot = false;
		int slotID = 0;
		std::string fileName = "";
	};

	struct LuaScript
	{
		std::string fileName;
		lua_State *luaState;
		bool hasStarted;
		bool requestedTermination;
		bool wantsSavestateCallback;
	};

    // A structure to hold information for a memory event
    struct LuaMemoryEvent
    {
        u32 address;
        u64 value; // Use u64 to handle all write sizes
        u32 size;
        bool is_write;
    };

    // A structure to hold information about a registered callback
    struct LuaCallbackInfo
    {
        int function_ref; // A reference to the Lua function in the Lua registry
        lua_State* state; // The specific Lua state this callback belongs to
    };

    // The thread-safe queue and its mutex
    extern std::mutex g_memory_event_mutex;
    extern std::deque<LuaMemoryEvent> g_memory_event_queue;

    // The map that associates a memory address with a Lua callback
    extern std::map<u32, LuaCallbackInfo> g_memory_callbacks;


	extern bool lua_isStateOperation;
	extern bool lua_isStateDone;
	extern bool lua_isStateSaved;
	extern bool lua_isStateLoaded;
	extern StateEvent m_stateData;

	void Init();
	void Shutdown();
	void LoadScript(std::string fileName);
	void TerminateScript(std::string fileName);
	bool IsScriptRunning(std::string fileName);
	void UpdateScripts(GCPadStatus* PadStatus);
    u32 readPointer(u32 startAddress, u32 offset);
	u32 normalizePointer(u32 pointer);
    u32 ExecuteMultilevelLoop(lua_State *L);
    bool IsInMEMArea(u32 pointer);
    void QueueMemoryEvent(u32 address, u64 value, u32 size, bool is_write);

	void iPressButton(const char* button);
	void iReleaseButton(const char* button);
	void iSetMainStickX(int xVal);
	void iSetMainStickY(int yVal);
	void iSetCStickX(int xVal);
	void iSetCStickY(int yVal);
	void iSaveState(bool toSlot, int slotID, std::string fileName);
	void iLoadState(bool fromSlot, int slotID, std::string fileName);
	void iCancelCurrentScript();
}
