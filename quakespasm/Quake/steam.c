#include "quakedef.h"
#if !defined(WIN32)
#include <dlfcn.h>
#endif

#if defined(WIN32)
#define dlopen(handler, mode) LoadLibraryA(handler)
#define dlsym(handler, funcname) (void*)GetProcAddress(handler, funcname)
#define dlerror() "fail"
#define dlclose(handler) FreeLibrary(handler)
#endif

const char *steam_wrap_dll = STEAM_WRAP_DLLNAME;

#define WRAP_POPULATEFUNCTION(handler, funcname) \
    wrap_##funcname = dlsym( handler, "c_"#funcname); \
    if (!wrap_##funcname) { \
        Sys_Printf("Error: %s\n", dlerror()); \
    }

int SteamInit = 0;
int SteamControllerInit = 0;
void *Steam_WrapHande;
int (*wrap_SteamAPI_Init)();
void (*wrap_SteamAPI_Shutdown)();
void (*wrap_SteamAPI_RunCallbacks)();
int (*wrap_SteamController_Init)();
uint64_t (*wrap_SteamController_GetControllerForGamepadIndex)(int);
void (*wrap_SteamController_SetLEDColor)(uint64_t, uint8_t, uint8_t, uint8_t, unsigned int nFlags);
int (*wrap_SteamController_GetInputTypeForHandle)(uint64_t);

void SteamInit_f()
{
	Steam_WrapHande = dlopen(steam_wrap_dll, RTLD_LAZY);
	if (!Steam_WrapHande) {
		/* fail to load the library */
		Sys_Printf("Failed to load Steam Wrap: %s\n", dlerror());
		dlclose(Steam_WrapHande);
        return;
	}
	else
	{
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamAPI_Init);

        if (wrap_SteamAPI_Init)
        {
            SteamInit = wrap_SteamAPI_Init();
            if (!SteamInit)
            {
                Sys_Printf("Error: wrap_SteamAPI_Init returned false!\n");
                dlclose(Steam_WrapHande);
                return;
            }
        }

        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamAPI_Shutdown);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamAPI_RunCallbacks);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamController_Init);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamController_GetControllerForGamepadIndex);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamController_SetLEDColor);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamController_GetInputTypeForHandle);
	}
}

void SteamController_Init_f()
{
        if (wrap_SteamController_Init)
        {
            SteamControllerInit = wrap_SteamController_Init();
            if (!SteamControllerInit)
            {
                Sys_Printf("Error: wrap_SteamController_Init returned false!\n");
            }
            else
            {
                Sys_Printf("Successfully wrap_SteamController_Init\n");
            }
        }
}

void SteamRunCallbacks_f()
{
    if (wrap_SteamAPI_RunCallbacks) { wrap_SteamAPI_RunCallbacks(); }
}

uint64_t SteamController_GetControllerForGamepadIndex_f(int cont_index)
{
    if (wrap_SteamController_GetControllerForGamepadIndex) {
        return wrap_SteamController_GetControllerForGamepadIndex(cont_index); 
    }
    else
    {
        return 0;
    }
}

int SteamController_GetInputTypeForHandle_f( uint64_t inputHandle )
{
    if (wrap_SteamController_GetInputTypeForHandle)
    { 
        return wrap_SteamController_GetInputTypeForHandle(inputHandle);
    }
    else
    {
        return 0;
    }
}

void SteamController_SetLEDColor_f(uint64_t inputHandle, uint8_t nColorR, uint8_t nColorG, uint8_t nColorB, unsigned int nFlags)
{
    if (wrap_SteamController_SetLEDColor)
    {
        wrap_SteamController_SetLEDColor(inputHandle, nColorR, nColorG, nColorB, nFlags);
    }
}

void SteamShutdown_f()
{
	if (SteamInit && Steam_WrapHande && wrap_SteamAPI_Shutdown)
	{
        wrap_SteamAPI_Shutdown();
        Sys_Printf("Shutting down Steam API\n");
		dlclose(Steam_WrapHande);
	}
}
