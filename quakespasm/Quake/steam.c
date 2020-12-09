#include "quakedef.h"
#include <dlfcn.h>

const char *steam_wrap_dll = STEAM_WRAP_DLLNAME;

#define WRAP_POPULATEFUNCTION(handler, funcname) \
    wrap_##funcname = dlsym( handler, "c_"#funcname); \
    if (!wrap_##funcname) { \
        Sys_Printf("Error: %s\n", dlerror()); \
    }

int SteamInit = 0;
int SteamInputInit = 0;
void *Steam_WrapHande;
int (*wrap_SteamAPI_Init)();
void (*wrap_SteamAPI_Shutdown)();
void (*wrap_SteamAPI_RunCallbacks)();
int (*wrap_SteamInput_Init)();
uint64_t (*wrap_SteamInput_GetControllerForGamepadIndex)(int);
void (*wrap_SteamInput_SetLEDColor)(uint64_t, uint8_t, uint8_t, uint8_t, unsigned int nFlags);

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
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamInput_Init);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamInput_GetControllerForGamepadIndex);
        WRAP_POPULATEFUNCTION(Steam_WrapHande, SteamInput_SetLEDColor);

        if (wrap_SteamInput_Init)
        {
            SteamInputInit = wrap_SteamInput_Init();
            if (!SteamInputInit)
            {
                Sys_Printf("Error: wrap_SteamInput_Init returned false!\n");
            }
            else
            {
                Sys_Printf("Successfully wrap_SteamInput_Init\n");
            }
        }
	}
}

void SteamRunCallbacks_f()
{
    if (wrap_SteamAPI_RunCallbacks) { wrap_SteamAPI_RunCallbacks(); }
}

uint64_t SteamInput_GetControllerForGamepadIndex_f(int cont_index)
{
    if (wrap_SteamInput_GetControllerForGamepadIndex) {
        return wrap_SteamInput_GetControllerForGamepadIndex(cont_index); 
    }
    else
    {
        return 0;
    }
}

void SteamInput_SetLEDColor_f(uint64_t inputHandle, uint8_t nColorR, uint8_t nColorG, uint8_t nColorB, unsigned int nFlags)
{
    if (wrap_SteamInput_SetLEDColor) { wrap_SteamInput_SetLEDColor(inputHandle, nColorR, nColorG, nColorB, nFlags); }
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
