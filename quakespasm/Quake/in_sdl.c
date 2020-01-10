/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

static qboolean	textmode;
extern qboolean	bind_grab;	//from the menu code, so that we regrab the mouse in order to pass inputs through

static cvar_t in_debugkeys = {"in_debugkeys", "0", CVAR_NONE};

#ifdef __APPLE__
/* Mouse acceleration needs to be disabled on OS X */
#define MACOS_X_ACCELERATION_HACK
#endif

#ifdef MACOS_X_ACCELERATION_HACK
#include <IOKit/IOTypes.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/event_status_driver.h>
#endif

#ifdef USE_SIXENSE
#include <sixense.h>
#endif

// SDL2 Game Controller cvars
cvar_t	joy_deadzone = { "joy_deadzone", "0.175", CVAR_ARCHIVE };
cvar_t	joy_deadzone_trigger = { "joy_deadzone_trigger", "0.2", CVAR_ARCHIVE };
cvar_t	joy_sensitivity_yaw = { "joy_sensitivity_yaw", "300", CVAR_ARCHIVE };
cvar_t	joy_sensitivity_pitch = { "joy_sensitivity_pitch", "150", CVAR_ARCHIVE };
cvar_t	joy_invert = { "joy_invert", "0", CVAR_ARCHIVE };
cvar_t	joy_exponent = { "joy_exponent", "3", CVAR_ARCHIVE };
cvar_t	joy_exponent_move = { "joy_exponent_move", "3", CVAR_ARCHIVE };
cvar_t	joy_swapmovelook = { "joy_swapmovelook", "0", CVAR_ARCHIVE };
cvar_t	joy_enable = { "joy_enable", "1", CVAR_ARCHIVE };

#if defined(USE_SDL2)
static SDL_JoystickID joy_active_instaceid = -1;
static SDL_GameController *joy_active_controller = NULL;
#endif

static qboolean	no_mouse = false;

static int buttonremap[] =
{
	K_MOUSE1,
	K_MOUSE3,	/* right button		*/
	K_MOUSE2,	/* middle button	*/
#if !defined(USE_SDL2)	/* mousewheel up/down not counted as buttons in SDL2 */
	K_MWHEELUP,
	K_MWHEELDOWN,
#endif
	K_MOUSE4,
	K_MOUSE5
};

/* total accumulated mouse movement since last frame */
static int	total_dx, total_dy = 0;

static int SDLCALL IN_FilterMouseEvents (const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_MOUSEMOTION:
	// case SDL_MOUSEBUTTONDOWN:
	// case SDL_MOUSEBUTTONUP:
		return 0;
	}

	return 1;
}

#if defined(USE_SDL2)
static int SDLCALL IN_SDL2_FilterMouseEvents (void *userdata, SDL_Event *event)
{
	return IN_FilterMouseEvents (event);
}
#endif

static void IN_BeginIgnoringMouseEvents(void)
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter = NULL;
	void *currentUserdata = NULL;
	SDL_GetEventFilter(&currentFilter, &currentUserdata);

	if (currentFilter != IN_SDL2_FilterMouseEvents)
		SDL_SetEventFilter(IN_SDL2_FilterMouseEvents, NULL);
#else
	if (SDL_GetEventFilter() != IN_FilterMouseEvents)
		SDL_SetEventFilter(IN_FilterMouseEvents);
#endif
}

static void IN_EndIgnoringMouseEvents(void)
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter;
	void *currentUserdata;
	if (SDL_GetEventFilter(&currentFilter, &currentUserdata) == SDL_TRUE)
		SDL_SetEventFilter(NULL, NULL);
#else
	if (SDL_GetEventFilter() != NULL)
		SDL_SetEventFilter(NULL);
#endif
}

#ifdef MACOS_X_ACCELERATION_HACK
static cvar_t in_disablemacosxmouseaccel = {"in_disablemacosxmouseaccel", "1", CVAR_ARCHIVE};
static double originalMouseSpeed = -1.0;

static io_connect_t IN_GetIOHandle(void)
{
	io_connect_t iohandle = MACH_PORT_NULL;
	io_service_t iohidsystem = MACH_PORT_NULL;
	mach_port_t masterport;
	kern_return_t status;

	status = IOMasterPort(MACH_PORT_NULL, &masterport);
	if (status != KERN_SUCCESS)
		return 0;

	iohidsystem = IORegistryEntryFromPath(masterport, kIOServicePlane ":/IOResources/IOHIDSystem");
	if (!iohidsystem)
		return 0;

	status = IOServiceOpen(iohidsystem, mach_task_self(), kIOHIDParamConnectType, &iohandle);
	IOObjectRelease(iohidsystem);

	return iohandle;
}

static void IN_DisableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDGetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), &originalMouseSpeed) == kIOReturnSuccess)
		{
			if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), -1.0) != kIOReturnSuccess)
			{
				Cvar_Set("in_disablemacosxmouseaccel", "0");
				Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
			}
		}
		else
		{
			Cvar_Set("in_disablemacosxmouseaccel", "0");
			Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDGetAccelerationWithKey).\n");
		}
		IOServiceClose(mouseDev);
	}
	else
	{
		Cvar_Set("in_disablemacosxmouseaccel", "0");
		Con_Printf("WARNING: Could not disable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
}

static void IN_ReenableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), originalMouseSpeed) != kIOReturnSuccess)
			Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
		IOServiceClose(mouseDev);
	}
	else
	{
		Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
	originalMouseSpeed = -1;
}
#endif /* MACOS_X_ACCELERATION_HACK */

#if 0
static void IN_Activate (void)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	/* Save the status of mouse acceleration */
	if (originalMouseSpeed == -1 && in_disablemacosxmouseaccel.value)
		IN_DisableOSXMouseAccel();
#endif

#if defined(USE_SDL2)
	if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0)
	{
		Con_Printf("WARNING: SDL_SetRelativeMouseMode(SDL_TRUE) failed.\n");
	}
#else
	if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
	{
		SDL_WM_GrabInput(SDL_GRAB_ON);
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
			Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_ON) failed.\n");
	}

	if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
	{
		SDL_ShowCursor(SDL_DISABLE);
		if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
			Con_Printf("WARNING: SDL_ShowCursor(SDL_DISABLE) failed.\n");
	}
#endif

	IN_EndIgnoringMouseEvents();

	total_dx = 0;
	total_dy = 0;
}

static void IN_Deactivate (qboolean free_cursor)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	if (originalMouseSpeed != -1)
		IN_ReenableOSXMouseAccel();
#endif

	if (free_cursor)
	{
#if defined(USE_SDL2)
		SDL_SetRelativeMouseMode(SDL_FALSE);
#else
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
		{
			SDL_WM_GrabInput(SDL_GRAB_OFF);
			if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
				Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_OFF) failed.\n");
		}

		if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
		{
			SDL_ShowCursor(SDL_ENABLE);
			if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
				Con_Printf("WARNING: SDL_ShowCursor(SDL_ENABLE) failed.\n");
		}
#endif
	}

	/* discard all mouse events when input is deactivated */
	if (cl.qcvm.extfuncs.CSQC_InputEvent && free_cursor)
		IN_EndIgnoringMouseEvents();
	else
		IN_BeginIgnoringMouseEvents();
}
#endif

static void IN_UpdateGrabs_Internal(qboolean forecerelease)
{
	qboolean wantcursor;	//we're trying to get a cursor here...
	qboolean freemouse;		//the OS should have a free cursor too...
	qboolean needevents;	//whether we want to receive events still

	wantcursor = (key_dest == key_console || (key_dest == key_menu&&!bind_grab)) || (key_dest == key_game && cl.csqc_cursorforced);
	freemouse = wantcursor && (modestate == MS_WINDOWED || (key_dest == key_game && cl.csqc_cursorforced));
	needevents = (!wantcursor) || key_dest == key_game;

	if (isDedicated)
		return;

	if (forecerelease)
		needevents = freemouse = wantcursor = true;

#ifdef MACOS_X_ACCELERATION_HACK
	if (!freemouse)
	{	/* Save the status of mouse acceleration */
		if (originalMouseSpeed == -1 && in_disablemacosxmouseaccel.value)
			IN_DisableOSXMouseAccel();
	}
	else if (originalMouseSpeed != -1)
		IN_ReenableOSXMouseAccel();
#endif

#if defined(USE_SDL2)
	if (SDL_SetRelativeMouseMode(freemouse?SDL_FALSE:SDL_TRUE) != 0)
	{
		Con_Printf("WARNING: SDL_SetRelativeMouseMode(%s) failed.\n", freemouse?"SDL_FALSE":"SDL_TRUE");
	}
#else
	if (freemouse)
	{
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
		{
			SDL_WM_GrabInput(SDL_GRAB_OFF);
			if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
				Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_OFF) failed.\n");
		}

		if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
		{
			SDL_ShowCursor(SDL_ENABLE);
			if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
				Con_Printf("WARNING: SDL_ShowCursor(SDL_ENABLE) failed.\n");
		}
	}
	else
	{
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
		{
			SDL_WM_GrabInput(SDL_GRAB_ON);
			if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
				Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_ON) failed.\n");
		}

		if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
		{
			SDL_ShowCursor(SDL_DISABLE);
			if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
				Con_Printf("WARNING: SDL_ShowCursor(SDL_DISABLE) failed.\n");
		}
	}
#endif

	if (needevents)
		IN_EndIgnoringMouseEvents();
	else
		IN_BeginIgnoringMouseEvents();
}
void IN_UpdateGrabs(void)
{
	IN_UpdateGrabs_Internal(false);
}

void IN_StartupJoystick (void)
{
#if defined(USE_SDL2)
	int i;
	int nummappings;
	char controllerdb[MAX_OSPATH];
	SDL_GameController *gamecontroller;
	
	if (COM_CheckParm("-nojoy"))
		return;
	
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == -1 )
	{
		Con_Warning("could not initialize SDL Game Controller\n");
		return;
	}
	
	// Load additional SDL2 controller definitions from gamecontrollerdb.txt
	q_snprintf (controllerdb, sizeof(controllerdb), "%s/gamecontrollerdb.txt", com_basedir);
	nummappings = SDL_GameControllerAddMappingsFromFile(controllerdb);
	if (nummappings > 0)
		Con_Printf("%d mappings loaded from gamecontrollerdb.txt\n", nummappings);
	
	// Also try host_parms->userdir
	if (host_parms->userdir != host_parms->basedir)
	{
		q_snprintf (controllerdb, sizeof(controllerdb), "%s/gamecontrollerdb.txt", host_parms->userdir);
		nummappings = SDL_GameControllerAddMappingsFromFile(controllerdb);
		if (nummappings > 0)
			Con_Printf("%d mappings loaded from gamecontrollerdb.txt\n", nummappings);
	}

	for (i = 0; i < SDL_NumJoysticks(); i++)
	{
		const char *joyname = SDL_JoystickNameForIndex(i);
		if ( SDL_IsGameController(i) )
		{
			const char *controllername = SDL_GameControllerNameForIndex(i);
			gamecontroller = SDL_GameControllerOpen(i);
			if (gamecontroller)
			{
				Con_Printf("detected controller: %s\n", controllername != NULL ? controllername : "NULL");
				
				joy_active_instaceid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamecontroller));
				joy_active_controller = gamecontroller;
				break;
			}
			else
			{
				Con_Warning("failed to open controller: %s\n", controllername != NULL ? controllername : "NULL");
			}
		}
		else
		{
			Con_Warning("joystick missing controller mappings: %s\n", joyname != NULL ? joyname : "NULL" );
		}
	}
#endif
}

void IN_ShutdownJoystick (void)
{
#if defined(USE_SDL2)
	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
}

#if defined(USE_SIXENSE)
static qboolean sixenseIsInit = false;
static int sixenseConnectedBase = -1;
static sixenseAllControllerData allcontrollerdata;

void IN_StartupSixense (void)
{
	if (COM_CheckParm("-nosixense"))
		return;

	if ( sixenseInit() == -1 )
	{
		Con_Warning("could not initialize Sixense library\n");
		return;
	}

	int MaxBases = sixenseGetMaxBases();
	Con_Printf("Maximum bases supported is %d\n", MaxBases);

	//Need to sleep a bit to determine
	Sys_Sleep(500);

	for (int i = 0; i < MaxBases; i++)
	{
		if ( sixenseIsBaseConnected(i) )
		{
			sixenseConnectedBase = i;
			sixenseSetActiveBase(i);
			Con_Printf("Base %d is used\n", sixenseConnectedBase);
			break;
		}
	}

	if (sixenseConnectedBase == -1)
	{
		Con_Warning("could not determine active base.\n");
		sixenseConnectedBase = 0;
		if ( sixenseSetActiveBase(0) )
		{
			Con_Printf("Setting to active base 0\n");
		}
		else {
			sixenseExit();
			return;
		}
	}

	sixenseGetAllData(0, &allcontrollerdata);

	sixenseIsInit = true;
}

void IN_ShutdownSixense (void)
{
	if ( sixenseIsInit ) sixenseExit();
}
#endif

void IN_Init (void)
{
	textmode = Key_TextEntry();

#if !defined(USE_SDL2)
	SDL_EnableUNICODE (textmode);
	if (SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL) == -1)
		Con_Printf("Warning: SDL_EnableKeyRepeat() failed.\n");
#else
	if (textmode)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
#endif
	if (safemode || COM_CheckParm("-nomouse"))
	{
		no_mouse = true;
		/* discard all mouse events when input is deactivated */
		IN_BeginIgnoringMouseEvents();
	}

#ifdef MACOS_X_ACCELERATION_HACK
	Cvar_RegisterVariable(&in_disablemacosxmouseaccel);
#endif
	Cvar_RegisterVariable(&in_debugkeys);
	Cvar_RegisterVariable(&joy_sensitivity_yaw);
	Cvar_RegisterVariable(&joy_sensitivity_pitch);
	Cvar_RegisterVariable(&joy_deadzone);
	Cvar_RegisterVariable(&joy_deadzone_trigger);
	Cvar_RegisterVariable(&joy_invert);
	Cvar_RegisterVariable(&joy_exponent);
	Cvar_RegisterVariable(&joy_exponent_move);
	Cvar_RegisterVariable(&joy_swapmovelook);
	Cvar_RegisterVariable(&joy_enable);

	IN_UpdateGrabs();
	IN_StartupJoystick();
#if defined (USE_SIXENSE)
	IN_StartupSixense();
#endif
}

void IN_Shutdown (void)
{
	IN_UpdateGrabs();
	IN_ShutdownJoystick();
#if defined(USE_SIXENSE)
	IN_ShutdownSixense();
#endif
}

extern cvar_t cl_maxpitch; /* johnfitz -- variable pitch clamping */
extern cvar_t cl_minpitch; /* johnfitz -- variable pitch clamping */


void IN_MouseMotion(int dx, int dy, int wx, int wy)
{
	if (key_dest != key_game && key_dest != key_message)
		dx = dy = 0;
	else if (cl.qcvm.extfuncs.CSQC_InputEvent)
	{
		PR_SwitchQCVM(&cl.qcvm);
		if (cl.csqc_cursorforced)
		{
			float s = CLAMP (1.0, scr_sbarscale.value, (float)glwidth / 320.0);
			wx /= s;
			wy /= s;

			G_FLOAT(OFS_PARM0) = CSIE_MOUSEABS;
			G_VECTORSET(OFS_PARM1, wx, wy, 0);	//x
			G_VECTORSET(OFS_PARM2, wy, 0, 0);	//y
			G_VECTORSET(OFS_PARM3, 0, 0, 0);	//devid
		}
		else
		{
			G_FLOAT(OFS_PARM0) = CSIE_MOUSEDELTA;
			G_VECTORSET(OFS_PARM1, dx, dy, 0);	//x
			G_VECTORSET(OFS_PARM2, dy, 0, 0);	//y
			G_VECTORSET(OFS_PARM3, 0, 0, 0);	//devid
		}
		PR_ExecuteProgram(cl.qcvm.extfuncs.CSQC_InputEvent);
		if (G_FLOAT(OFS_RETURN) || cl.csqc_cursorforced)
			dx = dy = 0;	//if the qc says it handled it, swallow the movement.
		PR_SwitchQCVM(NULL);
	}
	total_dx += dx;
	total_dy += dy;
}

#if defined(USE_SDL2)
typedef struct joyaxis_s
{
	float x;
	float y;
} joyaxis_t;

typedef struct joy_buttonstate_s
{
	qboolean buttondown[SDL_CONTROLLER_BUTTON_MAX];
} joybuttonstate_t;

typedef struct axisstate_s
{
	float axisvalue[SDL_CONTROLLER_AXIS_MAX]; // normalized to +-1
} joyaxisstate_t;

static joybuttonstate_t joy_buttonstate;
static joyaxisstate_t joy_axisstate;

static double joy_buttontimer[SDL_CONTROLLER_BUTTON_MAX];
static double joy_emulatedkeytimer[10];

#ifdef __WATCOMC__ /* OW1.9 doesn't have powf() / sqrtf() */
#define powf pow
#define sqrtf sqrt
#endif

#if defined(USE_SIXENSE)
typedef enum
{
	SIXENSE_BUTTON_INVALID = -1,
	SIXENSE_BUTTON_L1,
	SIXENSE_BUTTON_L2,
	SIXENSE_BUTTON_L3,
	SIXENSE_BUTTON_L4,
	SIXENSE_BUTTON_LS,
	SIXENSE_BUTTON_LB,
	SIXENSE_BUTTON_LSTART,
	SIXENSE_BUTTON_R1,
	SIXENSE_BUTTON_R2,
	SIXENSE_BUTTON_R3,
	SIXENSE_BUTTON_R4,
	SIXENSE_BUTTON_RS,
	SIXENSE_BUTTON_RB,
	SIXENSE_BUTTON_RSTART,
	SIXENSE_BUTTON_MAX
} sixense_button;

typedef enum
{
	SIXENSE_AXIS_INVALID = -1,
	SIXENSE_AXIS_LX,
	SIXENSE_AXIS_LY,
	SIXENSE_AXIS_RX,
	SIXENSE_AXIS_RY,
	SIXENSE_AXIS_LT,
	SIXENSE_AXIS_RT,
	SIXENSE_AXIS_MAX
} sixense_axis;

typedef struct sixenseaxis_s
{
	float x;
	float y;
} sixenseaxis_t;

typedef struct sixense_buttonstate_s
{
	qboolean buttondown[SIXENSE_BUTTON_MAX];
} sixensebuttonstate_t;

typedef struct sixenseaxisstate_s
{
	float axisvalue[SIXENSE_AXIS_MAX]; // normalized to +-1
} sixenseaxisstate_t;

static sixensebuttonstate_t sixense_buttonstate;
static sixenseaxisstate_t sixense_axisstate;

static double sixense_buttontimer[SIXENSE_BUTTON_MAX];
static double sixense_emulatedkeytimer[10];

sixense_data_t sixense_view, sixense_move;
#endif

/*
================
IN_AxisMagnitude

Returns the vector length of the given joystick axis
================
*/
static vec_t IN_AxisMagnitude(joyaxis_t axis)
{
	vec_t magnitude = sqrtf((axis.x * axis.x) + (axis.y * axis.y));
	return magnitude;
}

/*
================
IN_ApplyEasing

assumes axis values are in [-1, 1] and the vector magnitude has been clamped at 1.
Raises the axis values to the given exponent, keeping signs.
================
*/
static joyaxis_t IN_ApplyEasing(joyaxis_t axis, float exponent)
{
	joyaxis_t result = {0};
	vec_t eased_magnitude;
	vec_t magnitude = IN_AxisMagnitude(axis);
	
	if (magnitude == 0)
		return result;
	
	eased_magnitude = powf(magnitude, exponent);
	
	result.x = axis.x * (eased_magnitude / magnitude);
	result.y = axis.y * (eased_magnitude / magnitude);
	return result;
}

/*
================
IN_ApplyMoveEasing

same as IN_ApplyEasing, but scales the output by sqrt(2).
this gives diagonal stick inputs coordinates of (+/-1,+/-1).

forward/back/left/right will return +/- 1.41; this shouldn't be a problem because
you can pull back on the stick to go slower (and the final speed is clamped
by sv_maxspeed).
================
*/
static joyaxis_t IN_ApplyMoveEasing(joyaxis_t axis, float exponent)
{
	joyaxis_t result = IN_ApplyEasing(axis, exponent);
	const float v = sqrtf(2.0f);
	
	result.x *= v;
	result.y *= v;

	return result;
}

/*
================
IN_ApplyDeadzone

in: raw joystick axis values converted to floats in +-1
out: applies a circular deadzone and clamps the magnitude at 1
     (my 360 controller is slightly non-circular and the stick travels further on the diagonals)

deadzone is expected to satisfy 0 < deadzone < 1

from https://github.com/jeremiah-sypult/Quakespasm-Rift
and adapted from http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html
================
*/
static joyaxis_t IN_ApplyDeadzone(joyaxis_t axis, float deadzone)
{
	joyaxis_t result = {0};
	vec_t magnitude = IN_AxisMagnitude(axis);
	
	if ( magnitude > deadzone ) {
		const vec_t new_magnitude = q_min(1.0, (magnitude - deadzone) / (1.0 - deadzone));
		const vec_t scale = new_magnitude / magnitude;
		result.x = axis.x * scale;
		result.y = axis.y * scale;
	}
	
	return result;
}

/*
================
IN_KeyForControllerButton
================
*/
static int IN_KeyForControllerButton(SDL_GameControllerButton button)
{
	switch (button)
	{
		case SDL_CONTROLLER_BUTTON_A: return K_ABUTTON;
		case SDL_CONTROLLER_BUTTON_B: return K_BBUTTON;
		case SDL_CONTROLLER_BUTTON_X: return K_XBUTTON;
		case SDL_CONTROLLER_BUTTON_Y: return K_YBUTTON;
		case SDL_CONTROLLER_BUTTON_BACK: return K_TAB;
		case SDL_CONTROLLER_BUTTON_START: return K_ESCAPE;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return K_LTHUMB;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return K_RTHUMB;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_LSHOULDER;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_RSHOULDER;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UPARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWNARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFTARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHTARROW;
		default: return 0;
	}
}

#ifdef USE_SIXENSE
static int IN_KeyForSixenseButton(sixense_button button)
{
	//This is absolute evil. It converts Hydra buttons to Xbox buttons...
	switch (button)
	{
		case SIXENSE_BUTTON_R1: return K_ABUTTON;
		case SIXENSE_BUTTON_R2: return K_BBUTTON;
		case SIXENSE_BUTTON_R3: return K_XBUTTON;
		case SIXENSE_BUTTON_R4: return K_YBUTTON;
		case SIXENSE_BUTTON_LSTART: return K_TAB;
		case SIXENSE_BUTTON_RSTART: return K_ESCAPE;
		case SIXENSE_BUTTON_LS: return K_LTHUMB;
		case SIXENSE_BUTTON_RS: return K_RTHUMB;
		case SIXENSE_BUTTON_LB: return K_LSHOULDER;
		case SIXENSE_BUTTON_RB: return K_RSHOULDER;
		case SIXENSE_BUTTON_L1: return K_JOY1;
		case SIXENSE_BUTTON_L2: return K_JOY2;
		case SIXENSE_BUTTON_L3: return K_JOY3;
		case SIXENSE_BUTTON_L4: return K_JOY4;
		default: return 0;
	}
}
#endif

/*
================
IN_JoyKeyEvent

Sends a Key_Event if a unpressed -> pressed or pressed -> unpressed transition occurred,
and generates key repeats if the button is held down.

Adapted from DarkPlaces by lordhavoc
================
*/
static void IN_JoyKeyEvent(qboolean wasdown, qboolean isdown, int key, double *timer)
{
	// we can't use `realtime` for key repeats because it is not monotomic
	const double currenttime = Sys_DoubleTime();
	
	if (wasdown)
	{
		if (isdown)
		{
			if (currenttime >= *timer)
			{
				*timer = currenttime + 0.1;
				Key_Event(key, true);
			}
		}
		else
		{
			*timer = 0;
			Key_Event(key, false);
		}
	}
	else
	{
		if (isdown)
		{
			*timer = currenttime + 0.5;
			Key_Event(key, true);
		}
	}
}
#endif

#if defined(USE_SIXENSE)
void SixenseQuatsToEuler(float *rot_quat, vec3_t angle)
{
	const float q0 = -rot_quat[2]; //x
	const float q1 = rot_quat[0]; //y
	const float q2 = rot_quat[1]; //z
	const float q3 = rot_quat[3]; //w

	float sqw = q0*q0;
	float sqx = q1*q1;
	float sqy = q2*q2;
	float sqz = q3*q3;

	angle[ROLL] = -atan2(2 * (q1*q2 + q0*q3), sqw - sqx + sqy - sqz) * 180.0f / M_PI +180.0f;
	angle[YAW] = -asin(-2 * (q2*q3 - q0*q1)) * 180.0f / M_PI;
	angle[PITCH] = atan2(2 * (q1*q3 + q0*q2), sqw - sqx - sqy + sqz) * 180.0f / M_PI;
}

void SixensePopulateData( sixense_data_t *sixense_data, sixenseControllerData *controller)
{
	//Quake 0 1 2 = Sixense -2 0 1

	sixense_data->pos[0] = -controller->pos[2] * SIXENSE_TO_QUAKE_SCALE;
	sixense_data->pos[1] = controller->pos[0] * SIXENSE_TO_QUAKE_SCALE;
	sixense_data->pos[2] = controller->pos[1] * SIXENSE_TO_QUAKE_SCALE;

	SixenseQuatsToEuler(controller->rot_quat, sixense_data->angles);
	AngleVectors(sixense_data->angles, sixense_data->forward, sixense_data->right, sixense_data->up); 

	sixense_data->isactive = true;
}

void IN_SixenseCommands (void)
{
	int i;
	const float stickthreshold = 0.9;
	const float triggerthreshold = joy_deadzone_trigger.value;
	sixenseaxisstate_t new_sixense_axisstate;
	sixensebuttonstate_t new_sixense_buttonstate;
	unsigned int buttons = 0;

	/*if (!sixense_enable.value) return;*/

	if (!sixenseIsInit) return;

	//Avoid garbage, initialize them with zeroes
	for ( i = 0; i < SIXENSE_BUTTON_MAX; i++ ) new_sixense_buttonstate.buttondown[i] = false;
	for ( i = 0; i < SIXENSE_AXIS_MAX; i++ ) new_sixense_axisstate.axisvalue[i] = 0.0f;

	//If these won't be discovered later, threat them as inactive 
	sixense_view.isactive = false;
	sixense_move.isactive = false;
	
	sixenseGetAllNewestData( &allcontrollerdata );

	for ( i = 0; i < SIXENSE_MAX_CONTROLLERS ; i++ )
	{
		sixenseControllerData controller = allcontrollerdata.controllers[ i ];
			//Con_Printf("Enabled: %d\n", controller.enabled);
	    
			if ( controller.enabled && (!controller.is_docked) )
			{
				/*printf("Position X Y Z: %.0f %.0f %.0f\n", controller.pos[0], controller.pos[1], controller.pos[2]);
				printf("Docked: %d\n", controller.is_docked);
				printf("Which hand: %d\n", controller.which_hand);
				printf("Buttons: %d\n", controller.buttons);*/
				buttons = controller.buttons;
				if ( controller.which_hand == 1 ) {
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_L1] = ( buttons & SIXENSE_BUTTON_1 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_L2] = ( buttons & SIXENSE_BUTTON_2 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_L3] = ( buttons & SIXENSE_BUTTON_3 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_L4] = ( buttons & SIXENSE_BUTTON_4 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_LS] = ( buttons & SIXENSE_BUTTON_JOYSTICK ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_LB] = ( buttons & SIXENSE_BUTTON_BUMPER ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_LSTART] = ( buttons & SIXENSE_BUTTON_START ) ? true : false;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LX] = controller.joystick_x;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LY] = -controller.joystick_y;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LT] = controller.trigger;

					SixensePopulateData( &sixense_move, &controller );
				}
				else if ( controller.which_hand == 2 ) {
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_R1] = ( buttons & SIXENSE_BUTTON_1 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_R2] = ( buttons & SIXENSE_BUTTON_2 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_R3] = ( buttons & SIXENSE_BUTTON_3 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_R4] = ( buttons & SIXENSE_BUTTON_4 ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_RS] = ( buttons & SIXENSE_BUTTON_JOYSTICK ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_RB] = ( buttons & SIXENSE_BUTTON_BUMPER ) ? true : false;
					new_sixense_buttonstate.buttondown[SIXENSE_BUTTON_RSTART] = ( buttons & SIXENSE_BUTTON_START ) ? true : false;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RX] = controller.joystick_x;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RY] = -controller.joystick_y;
					new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RT] = controller.trigger;

					SixensePopulateData( &sixense_view, &controller );
				};
			}
	}

	for ( i = 0; i < SIXENSE_BUTTON_MAX; i++ )
	{
		qboolean newstate = new_sixense_buttonstate.buttondown[i];
		qboolean oldstate = sixense_buttonstate.buttondown[i];
		
		sixense_buttonstate.buttondown[i] = newstate;
		
		IN_JoyKeyEvent(oldstate, newstate, IN_KeyForSixenseButton( (sixense_button)i ), &sixense_buttontimer[i]);
	}
	
	// emit emulated arrow keys so the analog sticks can be used in the menu
	if (key_dest != key_game)
	{
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_LX] < -stickthreshold, new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LX] < -stickthreshold, K_LEFTARROW, &sixense_emulatedkeytimer[0]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_LX] > stickthreshold,  new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LX] > stickthreshold, K_RIGHTARROW, &sixense_emulatedkeytimer[1]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_LY] < -stickthreshold, new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LY] < -stickthreshold, K_UPARROW, &sixense_emulatedkeytimer[2]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_LY] > stickthreshold,  new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LY] > stickthreshold, K_DOWNARROW, &sixense_emulatedkeytimer[3]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_RX] < -stickthreshold,new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RX] < -stickthreshold, K_LEFTARROW, &sixense_emulatedkeytimer[4]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_RX] > stickthreshold, new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RX] > stickthreshold, K_RIGHTARROW, &sixense_emulatedkeytimer[5]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_RY] < -stickthreshold,new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RY] < -stickthreshold, K_UPARROW, &sixense_emulatedkeytimer[6]);
		IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_RY] > stickthreshold, new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RY] > stickthreshold, K_DOWNARROW, &sixense_emulatedkeytimer[7]);
	}
	
	// emit emulated keys for the analog triggers
	IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_LT] > triggerthreshold,  new_sixense_axisstate.axisvalue[SIXENSE_AXIS_LT] > triggerthreshold, K_LTRIGGER, &sixense_emulatedkeytimer[8]);
	IN_JoyKeyEvent(sixense_axisstate.axisvalue[SIXENSE_AXIS_RT] > triggerthreshold, new_sixense_axisstate.axisvalue[SIXENSE_AXIS_RT] > triggerthreshold, K_RTRIGGER, &sixense_emulatedkeytimer[9]);
	
	sixense_axisstate = new_sixense_axisstate;
}
#endif
/*
================
IN_Commands

Emit key events for game controller buttons, including emulated buttons for analog sticks/triggers
================
*/
void IN_Commands (void)
{
#if defined(USE_SIXENSE)
	IN_SixenseCommands();
#endif
#if defined(USE_SDL2)
	joyaxisstate_t newaxisstate;
	int i;
	const float stickthreshold = 0.9;
	const float triggerthreshold = joy_deadzone_trigger.value;
	
	if (!joy_enable.value)
		return;
	
	if (!joy_active_controller)
		return;

	// emit key events for controller buttons
	for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		qboolean newstate = SDL_GameControllerGetButton(joy_active_controller, (SDL_GameControllerButton)i);
		qboolean oldstate = joy_buttonstate.buttondown[i];
		
		joy_buttonstate.buttondown[i] = newstate;
		
		// NOTE: This can cause a reentrant call of IN_Commands, via SCR_ModalMessage when confirming a new game.
		IN_JoyKeyEvent(oldstate, newstate, IN_KeyForControllerButton((SDL_GameControllerButton)i), &joy_buttontimer[i]);
	}
	
	for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
	{
		newaxisstate.axisvalue[i] = SDL_GameControllerGetAxis(joy_active_controller, (SDL_GameControllerAxis)i) / 32768.0f;
	}
	
	// emit emulated arrow keys so the analog sticks can be used in the menu
	if (key_dest != key_game)
	{
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] < -stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] < -stickthreshold, K_LEFTARROW, &joy_emulatedkeytimer[0]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] > stickthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] > stickthreshold, K_RIGHTARROW, &joy_emulatedkeytimer[1]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] < -stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] < -stickthreshold, K_UPARROW, &joy_emulatedkeytimer[2]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] > stickthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] > stickthreshold, K_DOWNARROW, &joy_emulatedkeytimer[3]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] < -stickthreshold,newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] < -stickthreshold, K_LEFTARROW, &joy_emulatedkeytimer[4]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] > stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] > stickthreshold, K_RIGHTARROW, &joy_emulatedkeytimer[5]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] < -stickthreshold,newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] < -stickthreshold, K_UPARROW, &joy_emulatedkeytimer[6]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] > stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] > stickthreshold, K_DOWNARROW, &joy_emulatedkeytimer[7]);
	}
	
	// emit emulated keys for the analog triggers
	IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > triggerthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > triggerthreshold, K_LTRIGGER, &joy_emulatedkeytimer[8]);
	IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > triggerthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > triggerthreshold, K_RTRIGGER, &joy_emulatedkeytimer[9]);
	
	joy_axisstate = newaxisstate;
#endif
}

/*
================
IN_JoyMove
================
*/
void IN_JoyMove (usercmd_t *cmd)
{
#if defined(USE_SDL2)
	float	speed;
	joyaxis_t moveRaw, moveDeadzone, moveEased;
	joyaxis_t lookRaw, lookDeadzone, lookEased;

	if (!joy_enable.value)
		return;
	
	if (!joy_active_controller)
		return;
	
	moveRaw.x = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX];
	moveRaw.y = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY];
	lookRaw.x = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX];
	lookRaw.y = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY];
	
	if (joy_swapmovelook.value)
	{
		joyaxis_t temp = moveRaw;
		moveRaw = lookRaw;
		lookRaw = temp;
	}
	
	moveDeadzone = IN_ApplyDeadzone(moveRaw, joy_deadzone.value);
	lookDeadzone = IN_ApplyDeadzone(lookRaw, joy_deadzone.value);

	moveEased = IN_ApplyMoveEasing(moveDeadzone, joy_exponent_move.value);
	lookEased = IN_ApplyEasing(lookDeadzone, joy_exponent.value);
	
	if ((in_speed.state & 1) ^ (cl_alwaysrun.value != 0.0))
		speed = cl_movespeedkey.value;
	else
		speed = 1;

	cmd->sidemove += (cl_sidespeed.value * speed * moveEased.x);
	cmd->forwardmove -= (cl_forwardspeed.value * speed * moveEased.y);

	cl.viewangles[YAW] -= lookEased.x * joy_sensitivity_yaw.value * host_frametime * cl.csqc_sensitivity;
	cl.viewangles[PITCH] += lookEased.y * joy_sensitivity_pitch.value * (joy_invert.value ? -1.0 : 1.0) * host_frametime * cl.csqc_sensitivity;

	if (lookEased.x != 0 || lookEased.y != 0)
		V_StopPitchDrift();

	/* johnfitz -- variable pitch clamping */
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
		cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
		cl.viewangles[PITCH] = cl_minpitch.value;
#endif
}

#if defined(USE_SIXENSE)
void IN_SixenseMove (usercmd_t *cmd)
{
	float	speed;
	joyaxis_t moveRaw, moveDeadzone, moveEased;
	joyaxis_t lookRaw, lookDeadzone, lookEased;

	/*if (!sixense_enable.value) return;*/
	
	moveRaw.x = sixense_axisstate.axisvalue[SIXENSE_AXIS_LX];
	moveRaw.y = sixense_axisstate.axisvalue[SIXENSE_AXIS_LY];
	lookRaw.x = sixense_axisstate.axisvalue[SIXENSE_AXIS_RX];
	lookRaw.y = sixense_axisstate.axisvalue[SIXENSE_AXIS_RY];
	
	if (joy_swapmovelook.value)
	{
		joyaxis_t temp = moveRaw;
		moveRaw = lookRaw;
		lookRaw = temp;
	}
	
	moveDeadzone = IN_ApplyDeadzone(moveRaw, joy_deadzone.value);
	lookDeadzone = IN_ApplyDeadzone(lookRaw, joy_deadzone.value);

	moveEased = IN_ApplyMoveEasing(moveDeadzone, joy_exponent_move.value);
	lookEased = IN_ApplyEasing(lookDeadzone, joy_exponent.value);
	
	if ((in_speed.state & 1) ^ (cl_alwaysrun.value != 0.0))
		speed = cl_movespeedkey.value;
	else
		speed = 1;

	cmd->sidemove += (cl_sidespeed.value * speed * moveEased.x);
	cmd->forwardmove -= (cl_forwardspeed.value * speed * moveEased.y);

	cl.viewangles[YAW] -= lookEased.x * joy_sensitivity_yaw.value * host_frametime * cl.csqc_sensitivity;
	cl.viewangles[PITCH] += lookEased.y * joy_sensitivity_pitch.value * (joy_invert.value ? -1.0 : 1.0) * host_frametime * cl.csqc_sensitivity;

	if (lookEased.x != 0 || lookEased.y != 0)
		V_StopPitchDrift();

	/* johnfitz -- variable pitch clamping */
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
		cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
		cl.viewangles[PITCH] = cl_minpitch.value;
}

void IN_SixenseGestures (usercmd_t *cmd)
{
}
#endif

void IN_MouseMove(usercmd_t *cmd)
{
	int		dmx, dmy;

	dmx = total_dx * sensitivity.value;
	dmy = total_dy * sensitivity.value;

	total_dx = 0;
	total_dy = 0;

	if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * dmx;
	else
		cl.viewangles[YAW] -= m_yaw.value * dmx * cl.csqc_sensitivity;

	if (in_mlook.state & 1)
	{
		if (dmx || dmy)
			V_StopPitchDrift ();
	}

	if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * dmy * cl.csqc_sensitivity;
		/* johnfitz -- variable pitch clamping */
		if (cl.viewangles[PITCH] > cl_maxpitch.value)
			cl.viewangles[PITCH] = cl_maxpitch.value;
		if (cl.viewangles[PITCH] < cl_minpitch.value)
			cl.viewangles[PITCH] = cl_minpitch.value;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * dmy;
		else
			cmd->forwardmove -= m_forward.value * dmy;
	}
}

void IN_Move(usercmd_t *cmd)
{
	IN_JoyMove(cmd);
#if defined(USE_SIXENSE)
	IN_SixenseMove(cmd);
	IN_SixenseGestures(cmd);
#endif
	IN_MouseMove(cmd);
}

void IN_ClearStates (void)
{
}

void IN_UpdateInputMode (void)
{
	qboolean want_textmode = Key_TextEntry();
	if (textmode != want_textmode)
	{
		textmode = want_textmode;
#if !defined(USE_SDL2)
		SDL_EnableUNICODE(textmode);
		if (in_debugkeys.value)
			Con_Printf("SDL_EnableUNICODE %d time: %g\n", textmode, Sys_DoubleTime());
#else
		if (textmode)
		{
			SDL_StartTextInput();
			if (in_debugkeys.value)
				Con_Printf("SDL_StartTextInput time: %g\n", Sys_DoubleTime());
		}
		else
		{
			SDL_StopTextInput();
			if (in_debugkeys.value)
				Con_Printf("SDL_StopTextInput time: %g\n", Sys_DoubleTime());
		}
#endif
	}
}

#if !defined(USE_SDL2)
static inline int IN_SDL_KeysymToQuakeKey(SDLKey sym)
{
	if (sym > SDLK_SPACE && sym < SDLK_DELETE)
		return sym;

	switch (sym)
	{
	case SDLK_TAB: return K_TAB;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_SPACE: return K_SPACE;

	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP: return K_UPARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;

	case SDLK_LALT: return K_ALT;
	case SDLK_RALT: return K_ALT;
	case SDLK_LCTRL: return K_CTRL;
	case SDLK_RCTRL: return K_CTRL;
	case SDLK_LSHIFT: return K_SHIFT;
	case SDLK_RSHIFT: return K_SHIFT;

	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_INSERT: return K_INS;
	case SDLK_DELETE: return K_DEL;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;

	case SDLK_NUMLOCK: return K_KP_NUMLOCK;
	case SDLK_KP_DIVIDE: return K_KP_SLASH;
	case SDLK_KP_MULTIPLY: return K_KP_STAR;
	case SDLK_KP_MINUS:return K_KP_MINUS;
	case SDLK_KP7: return K_KP_HOME;
	case SDLK_KP8: return K_KP_UPARROW;
	case SDLK_KP9: return K_KP_PGUP;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_KP4: return K_KP_LEFTARROW;
	case SDLK_KP5: return K_KP_5;
	case SDLK_KP6: return K_KP_RIGHTARROW;
	case SDLK_KP1: return K_KP_END;
	case SDLK_KP2: return K_KP_DOWNARROW;
	case SDLK_KP3: return K_KP_PGDN;
	case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_KP0: return K_KP_INS;
	case SDLK_KP_PERIOD: return K_KP_DEL;

	case SDLK_LMETA: return K_COMMAND;
	case SDLK_RMETA: return K_COMMAND;

	case SDLK_BREAK: return K_PAUSE;
	case SDLK_PAUSE: return K_PAUSE;

	case SDLK_WORLD_18: return '~'; // the '�' key

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static inline int IN_SDL2_ScancodeToQuakeKey(SDL_Scancode scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_TAB: return K_TAB;
	case SDL_SCANCODE_RETURN: return K_ENTER;
	case SDL_SCANCODE_RETURN2: return K_ENTER;
	case SDL_SCANCODE_ESCAPE: return K_ESCAPE;
	case SDL_SCANCODE_SPACE: return K_SPACE;

	case SDL_SCANCODE_A: return 'a';
	case SDL_SCANCODE_B: return 'b';
	case SDL_SCANCODE_C: return 'c';
	case SDL_SCANCODE_D: return 'd';
	case SDL_SCANCODE_E: return 'e';
	case SDL_SCANCODE_F: return 'f';
	case SDL_SCANCODE_G: return 'g';
	case SDL_SCANCODE_H: return 'h';
	case SDL_SCANCODE_I: return 'i';
	case SDL_SCANCODE_J: return 'j';
	case SDL_SCANCODE_K: return 'k';
	case SDL_SCANCODE_L: return 'l';
	case SDL_SCANCODE_M: return 'm';
	case SDL_SCANCODE_N: return 'n';
	case SDL_SCANCODE_O: return 'o';
	case SDL_SCANCODE_P: return 'p';
	case SDL_SCANCODE_Q: return 'q';
	case SDL_SCANCODE_R: return 'r';
	case SDL_SCANCODE_S: return 's';
	case SDL_SCANCODE_T: return 't';
	case SDL_SCANCODE_U: return 'u';
	case SDL_SCANCODE_V: return 'v';
	case SDL_SCANCODE_W: return 'w';
	case SDL_SCANCODE_X: return 'x';
	case SDL_SCANCODE_Y: return 'y';
	case SDL_SCANCODE_Z: return 'z';

	case SDL_SCANCODE_1: return '1';
	case SDL_SCANCODE_2: return '2';
	case SDL_SCANCODE_3: return '3';
	case SDL_SCANCODE_4: return '4';
	case SDL_SCANCODE_5: return '5';
	case SDL_SCANCODE_6: return '6';
	case SDL_SCANCODE_7: return '7';
	case SDL_SCANCODE_8: return '8';
	case SDL_SCANCODE_9: return '9';
	case SDL_SCANCODE_0: return '0';

	case SDL_SCANCODE_MINUS: return '-';
	case SDL_SCANCODE_EQUALS: return '=';
	case SDL_SCANCODE_LEFTBRACKET: return '[';
	case SDL_SCANCODE_RIGHTBRACKET: return ']';
	case SDL_SCANCODE_BACKSLASH: return '\\';
	case SDL_SCANCODE_NONUSHASH: return '#';
	case SDL_SCANCODE_SEMICOLON: return ';';
	case SDL_SCANCODE_APOSTROPHE: return '\'';
	case SDL_SCANCODE_GRAVE: return '`';
	case SDL_SCANCODE_COMMA: return ',';
	case SDL_SCANCODE_PERIOD: return '.';
	case SDL_SCANCODE_SLASH: return '/';
	case SDL_SCANCODE_NONUSBACKSLASH: return '\\';

	case SDL_SCANCODE_BACKSPACE: return K_BACKSPACE;
	case SDL_SCANCODE_UP: return K_UPARROW;
	case SDL_SCANCODE_DOWN: return K_DOWNARROW;
	case SDL_SCANCODE_LEFT: return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT: return K_RIGHTARROW;

	case SDL_SCANCODE_LALT: return K_ALT;
	case SDL_SCANCODE_RALT: return K_ALT;
	case SDL_SCANCODE_LCTRL: return K_CTRL;
	case SDL_SCANCODE_RCTRL: return K_CTRL;
	case SDL_SCANCODE_LSHIFT: return K_SHIFT;
	case SDL_SCANCODE_RSHIFT: return K_SHIFT;

	case SDL_SCANCODE_F1: return K_F1;
	case SDL_SCANCODE_F2: return K_F2;
	case SDL_SCANCODE_F3: return K_F3;
	case SDL_SCANCODE_F4: return K_F4;
	case SDL_SCANCODE_F5: return K_F5;
	case SDL_SCANCODE_F6: return K_F6;
	case SDL_SCANCODE_F7: return K_F7;
	case SDL_SCANCODE_F8: return K_F8;
	case SDL_SCANCODE_F9: return K_F9;
	case SDL_SCANCODE_F10: return K_F10;
	case SDL_SCANCODE_F11: return K_F11;
	case SDL_SCANCODE_F12: return K_F12;
	case SDL_SCANCODE_INSERT: return K_INS;
	case SDL_SCANCODE_DELETE: return K_DEL;
	case SDL_SCANCODE_PAGEDOWN: return K_PGDN;
	case SDL_SCANCODE_PAGEUP: return K_PGUP;
	case SDL_SCANCODE_HOME: return K_HOME;
	case SDL_SCANCODE_END: return K_END;

	case SDL_SCANCODE_NUMLOCKCLEAR: return K_KP_NUMLOCK;
	case SDL_SCANCODE_KP_DIVIDE: return K_KP_SLASH;
	case SDL_SCANCODE_KP_MULTIPLY: return K_KP_STAR;
	case SDL_SCANCODE_KP_MINUS: return K_KP_MINUS;
	case SDL_SCANCODE_KP_7: return K_KP_HOME;
	case SDL_SCANCODE_KP_8: return K_KP_UPARROW;
	case SDL_SCANCODE_KP_9: return K_KP_PGUP;
	case SDL_SCANCODE_KP_PLUS: return K_KP_PLUS;
	case SDL_SCANCODE_KP_4: return K_KP_LEFTARROW;
	case SDL_SCANCODE_KP_5: return K_KP_5;
	case SDL_SCANCODE_KP_6: return K_KP_RIGHTARROW;
	case SDL_SCANCODE_KP_1: return K_KP_END;
	case SDL_SCANCODE_KP_2: return K_KP_DOWNARROW;
	case SDL_SCANCODE_KP_3: return K_KP_PGDN;
	case SDL_SCANCODE_KP_ENTER: return K_KP_ENTER;
	case SDL_SCANCODE_KP_0: return K_KP_INS;
	case SDL_SCANCODE_KP_PERIOD: return K_KP_DEL;

	case SDL_SCANCODE_LGUI: return K_COMMAND;
	case SDL_SCANCODE_RGUI: return K_COMMAND;

	case SDL_SCANCODE_PAUSE: return K_PAUSE;

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static void IN_DebugTextEvent(SDL_Event *event)
{
	Con_Printf ("SDL_TEXTINPUT '%s' time: %g\n", event->text.text, Sys_DoubleTime());
}
#endif

static void IN_DebugKeyEvent(SDL_Event *event)
{
	const char *eventtype = (event->key.state == SDL_PRESSED) ? "SDL_KEYDOWN" : "SDL_KEYUP";
#if defined(USE_SDL2)
	Con_Printf ("%s scancode: '%s' keycode: '%s' time: %g\n",
		eventtype,
		SDL_GetScancodeName(event->key.keysym.scancode),
		SDL_GetKeyName(event->key.keysym.sym),
		Sys_DoubleTime());
#else
	Con_Printf ("%s sym: '%s' unicode: %04x time: %g\n",
		eventtype,
		SDL_GetKeyName(event->key.keysym.sym),
		(int)event->key.keysym.unicode,
		Sys_DoubleTime());
#endif
}

void IN_SendKeyEvents (void)
{
	SDL_Event event;
	int key;
	qboolean down;

	IN_UpdateGrabs();

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
#if defined(USE_SDL2)
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				S_UnblockSound();
			else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				S_BlockSound();
			else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
			{
				vid.width = event.window.data1;
				vid.height = event.window.data2;
				Cvar_FindVar("scr_conscale")->callback(NULL);
			}
			break;
#else
		case SDL_ACTIVEEVENT:
			if (event.active.state & (SDL_APPINPUTFOCUS|SDL_APPACTIVE))
			{
				if (event.active.gain)
					S_UnblockSound();
				else	S_BlockSound();
			}
			break;
#endif
#if defined(USE_SDL2)
		case SDL_TEXTINPUT:
			if (in_debugkeys.value)
				IN_DebugTextEvent(&event);

		// SDL2: We use SDL_TEXTINPUT for typing in the console / chat.
		// SDL2 uses the local keyboard layout and handles modifiers
		// (shift for uppercase, etc.) for us.
			{
				unsigned char *ch;
				for (ch = (unsigned char *)event.text.text; *ch; ch++)
					if ((*ch & ~0x7F) == 0)
						Char_Event (*ch);
			}
			break;
#endif
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			down = (event.key.state == SDL_PRESSED);

			if (in_debugkeys.value)
				IN_DebugKeyEvent(&event);

#if defined(USE_SDL2)
		// SDL2: we interpret the keyboard as the US layout, so keybindings
		// are based on key position, not the label on the key cap.
			key = IN_SDL2_ScancodeToQuakeKey(event.key.keysym.scancode);
#else
			key = IN_SDL_KeysymToQuakeKey(event.key.keysym.sym);
#endif

			Key_Event (key, down);

#if !defined(USE_SDL2)
			if (down && (event.key.keysym.unicode & ~0x7F) == 0)
				Char_Event (event.key.keysym.unicode);
#endif
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (event.button.button < 1 ||
			    event.button.button > sizeof(buttonremap) / sizeof(buttonremap[0]))
			{
				Con_Printf ("Ignored event for mouse button %d\n",
							event.button.button);
				break;
			}
			Key_Event(buttonremap[event.button.button - 1], event.button.state == SDL_PRESSED);
			break;

#if defined(USE_SDL2)
		case SDL_MOUSEWHEEL:
			if (event.wheel.y > 0)
			{
				Key_Event(K_MWHEELUP, true);
				Key_Event(K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0)
			{
				Key_Event(K_MWHEELDOWN, true);
				Key_Event(K_MWHEELDOWN, false);
			}
			break;
#endif

		case SDL_MOUSEMOTION:
			IN_MouseMotion(event.motion.xrel, event.motion.yrel, event.motion.x, event.motion.y);
			break;

#if defined(USE_SDL2)
		case SDL_CONTROLLERDEVICEADDED:
			if (joy_active_instaceid == -1)
			{
				joy_active_controller = SDL_GameControllerOpen(event.cdevice.which);
				if (joy_active_controller == NULL)
					Con_DPrintf("Couldn't open game controller\n");
				else
				{
					SDL_Joystick *joy;
					joy = SDL_GameControllerGetJoystick(joy_active_controller);
					joy_active_instaceid = SDL_JoystickInstanceID(joy);
				}
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEADDED\n");
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			if (joy_active_instaceid != -1 && event.cdevice.which == joy_active_instaceid)
			{
				SDL_GameControllerClose(joy_active_controller);
				joy_active_controller = NULL;
				joy_active_instaceid = -1;
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEREMOVED\n");
			break;
		case SDL_CONTROLLERDEVICEREMAPPED:
			Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEREMAPPED\n");
			break;
#endif
				
		case SDL_QUIT:
			CL_Disconnect ();
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}

