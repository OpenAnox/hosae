/*
Copyright (C) 1997-2001 Id Software, Inc.

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

#include <SDL2/SDL.h>

#include "client/client.h"

static unsigned sys_msg_time;

// joystick defines and variables
// where should defines be moved?
#define JOY_ABSOLUTE_AXIS 0x00000000// control like a joystick
#define JOY_RELATIVE_AXIS 0x00000010// control like a mouse, spinner, trackball
#define JOY_MAX_AXES      6         // X, Y, Z, R, U, V
#define JOY_AXIS_X        0
#define JOY_AXIS_Y        1
#define JOY_AXIS_Z        2
#define JOY_AXIS_R        3
#define JOY_AXIS_U        4
#define JOY_AXIS_V        5

enum ControlList
{
	AxisNada = 0,
	AxisForward,
	AxisLook,
	AxisSide,
	AxisTurn,
	AxisUp
};

cvar_t *in_mouse;
cvar_t *in_joystick;

// none of these cvars are saved over a session
// this means that advanced controller configuration needs to be executed
// each time.  this avoids any problems with getting back to a default usage
// or when changing from one controller to another.  this way at least something
// works.
cvar_t *joy_name;
cvar_t *joy_advanced;
cvar_t *joy_advaxisx;
cvar_t *joy_advaxisy;
cvar_t *joy_advaxisz;
cvar_t *joy_advaxisr;
cvar_t *joy_advaxisu;
cvar_t *joy_advaxisv;
cvar_t *joy_forwardthreshold;
cvar_t *joy_sidethreshold;
cvar_t *joy_pitchthreshold;
cvar_t *joy_yawthreshold;
cvar_t *joy_forwardsensitivity;
cvar_t *joy_sidesensitivity;
cvar_t *joy_pitchsensitivity;
cvar_t *joy_yawsensitivity;
cvar_t *joy_upthreshold;
cvar_t *joy_upsensitivity;

bool in_appactive;

// forward-referenced functions
void IN_StartupJoystick();
void Joy_AdvancedUpdate_f();
void IN_JoyMove( usercmd_t *cmd );

/*
============================================================

  MOUSE CONTROL

============================================================
*/

// mouse variables
cvar_t *m_filter;

bool mlooking;

void IN_MLookDown() { mlooking = true; }
void IN_MLookUp()
{
	mlooking = false;
	if ( freelook->value == 0.0f && ( lookspring->value >= 1.0f ) )
	{
		IN_CenterView();
	}
}

static int mouse_buttons;
static int mouse_oldbuttonstate;
static int mouse_x, mouse_y,
        old_mouse_x, old_mouse_y,
        mx_accum, my_accum;

static bool isMouseActive;// false when not focus app

bool restore_spi;
bool mouseinitialized;
int  originalmouseparms[ 3 ], newmouseparms[ 3 ] = { 0, 0, 1 };
bool mouseparmsvalid;

static int window_center_x, window_center_y;

static int MapKey( int key )
{
	switch ( key )
	{
		case SDLK_UP:
			return K_UPARROW;
		case SDLK_DOWN:
			return K_DOWNARROW;
		case SDLK_LEFT:
			return K_LEFTARROW;
		case SDLK_RIGHT:
			return K_RIGHTARROW;
		case SDLK_BACKSPACE:
			return K_BACKSPACE;
		case SDLK_PAGEUP:
			return K_PGUP;
		case SDLK_PAGEDOWN:
			return K_PGDN;
		case SDLK_LALT:
		case SDLK_RALT:
			return K_ALT;
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:
			return K_SHIFT;
		case SDLK_LCTRL:
		case SDLK_RCTRL:
			return K_CTRL;
		case SDLK_SPACE:
			return K_SPACE;
		case SDLK_F1:
			return K_F1;
		case SDLK_F2:
			return K_F2;
		case SDLK_F3:
			return K_F3;
		case SDLK_F4:
			return K_F4;
		case SDLK_F5:
			return K_F5;
		case SDLK_F6:
			return K_F6;
		case SDLK_F7:
			return K_F7;
		case SDLK_F8:
			return K_F8;
		case SDLK_F9:
			return K_F9;
		case SDLK_F10:
			return K_F10;
		case SDLK_F11:
			return K_F11;
		case SDLK_F12:
			return K_F12;
		default:
			break;
	}

	if ( key >= K_MAX )
	{
		//assert( 0 );
		return K_INVALID;
	}

	return key;
}

bool IN_HandleEvent( const SDL_Event &event )
{
	sys_msg_time = chr::globalApp->GetNumMilliseconds();

	switch ( event.type )
	{
		case SDL_MOUSEWHEEL:
		{
			if ( event.wheel.y > 0 )
			{
				Key_Event( K_MWHEELUP, true, sys_msg_time );
				Key_Event( K_MWHEELUP, false, sys_msg_time );
			}
			else
			{
				Key_Event( K_MWHEELDOWN, true, sys_msg_time );
				Key_Event( K_MWHEELDOWN, false, sys_msg_time );
			}
			return true;
		}
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		{
			int key = MapKey( event.key.keysym.sym );
			if ( key == K_INVALID )
			{
				Com_DPrintf( "Hit an unhandled key: %d\n", event.key.keysym.sym );
				return true;
			}

			Key_Event( key, ( event.key.state == SDL_PRESSED ), sys_msg_time );
			return true;
		}
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			int button;
			switch ( event.button.button )
			{
				case SDL_BUTTON_LEFT:
					button = K_MOUSE1;
					break;
				case SDL_BUTTON_RIGHT:
					button = K_MOUSE2;
					break;
				case SDL_BUTTON_MIDDLE:
					button = K_MOUSE3;
					break;
				default:
					assert( 0 );
					button = K_INVALID;
					break;
			}

			if ( button == K_INVALID )
			{
				Com_DPrintf( "Hit an unhandled button: %d\n", event.button.button );
				return true;
			}

			Key_Event( button, ( event.button.state == SDL_PRESSED ), sys_msg_time );
			return true;
		}
	}

	return false;
}

/**
 * Called when the window gains focus or changes in some way.
 */
void IN_ActivateMouse()
{
	if ( !mouseinitialized )
	{
		return;
	}

	if ( in_mouse->value <= 0.0f )
	{
		isMouseActive = false;
		return;
	}

	if ( isMouseActive )
	{
		return;
	}

	isMouseActive = true;

	window_center_x = ( int ) viddef.width / 2;
	window_center_y = ( int ) viddef.height / 2;

	chr::globalApp->ShowCursor( false );
}

/**
 * Called when the window loses focus.
 */
void IN_DeactivateMouse()
{
	if ( !mouseinitialized || !isMouseActive )
	{
		return;
	}

	isMouseActive = false;

	chr::globalApp->ShowCursor( true );
}

void IN_StartupMouse()
{
	cvar_t *cv = Cvar_Get( "in_initmouse", "1", CVAR_NOSET );
	if ( cv->value <= 0.0f )
	{
		return;
	}

	mouseinitialized = true;
	mouse_buttons    = 3;
}

extern SDL_Window *VID_GetSDLWindowHandle();

void IN_MouseMove( usercmd_t *cmd )
{
	if ( !isMouseActive )
	{
		return;
	}

	// find mouse movement
	int mx, my;
	SDL_GetMouseState( &mx, &my );
	mx -= window_center_x;
	my -= window_center_y;

	if ( m_filter->value > 0.0f )
	{
		mouse_x = ( mx + old_mouse_x ) * 0.5;
		mouse_y = ( my + old_mouse_y ) * 0.5;
	}
	else
	{
		mouse_x = mx;
		mouse_y = my;
	}

	old_mouse_x = mx;
	old_mouse_y = my;

	mouse_x *= sensitivity->value;
	mouse_y *= sensitivity->value;

	// add mouse X/Y movement to cmd
	if ( ( in_strafe.state & 1 ) || ( ( lookstrafe->value >= 1.0f ) && mlooking ) )
	{
		cmd->sidemove += m_side->value * mouse_x;
	}
	else
	{
		cl.viewangles[ YAW ] -= m_yaw->value * mouse_x;
	}

	if ( ( mlooking || freelook->value > 0.0f ) && !( in_strafe.state & 1 ) )
	{
		cl.viewangles[ PITCH ] += m_pitch->value * mouse_y;
	}
	else
	{
		cmd->forwardmove -= m_forward->value * mouse_y;
	}

	SDL_WarpMouseInWindow( VID_GetSDLWindowHandle(), window_center_x, window_center_y );
}


/*
=========================================================================

VIEW CENTERING

=========================================================================
*/

cvar_t *v_centermove;
cvar_t *v_centerspeed;


/*
===========
IN_Init
===========
*/
void IN_Init()
{
	// mouse variables
	m_filter = Cvar_Get( "m_filter", "0", 0 );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );

	// joystick variables
	in_joystick            = Cvar_Get( "in_joystick", "0", CVAR_ARCHIVE );
	joy_name               = Cvar_Get( "joy_name", "joystick", 0 );
	joy_advanced           = Cvar_Get( "joy_advanced", "0", 0 );
	joy_advaxisx           = Cvar_Get( "joy_advaxisx", "0", 0 );
	joy_advaxisy           = Cvar_Get( "joy_advaxisy", "0", 0 );
	joy_advaxisz           = Cvar_Get( "joy_advaxisz", "0", 0 );
	joy_advaxisr           = Cvar_Get( "joy_advaxisr", "0", 0 );
	joy_advaxisu           = Cvar_Get( "joy_advaxisu", "0", 0 );
	joy_advaxisv           = Cvar_Get( "joy_advaxisv", "0", 0 );
	joy_forwardthreshold   = Cvar_Get( "joy_forwardthreshold", "0.15", 0 );
	joy_sidethreshold      = Cvar_Get( "joy_sidethreshold", "0.15", 0 );
	joy_upthreshold        = Cvar_Get( "joy_upthreshold", "0.15", 0 );
	joy_pitchthreshold     = Cvar_Get( "joy_pitchthreshold", "0.15", 0 );
	joy_yawthreshold       = Cvar_Get( "joy_yawthreshold", "0.15", 0 );
	joy_forwardsensitivity = Cvar_Get( "joy_forwardsensitivity", "-1", 0 );
	joy_sidesensitivity    = Cvar_Get( "joy_sidesensitivity", "-1", 0 );
	joy_upsensitivity      = Cvar_Get( "joy_upsensitivity", "-1", 0 );
	joy_pitchsensitivity   = Cvar_Get( "joy_pitchsensitivity", "1", 0 );
	joy_yawsensitivity     = Cvar_Get( "joy_yawsensitivity", "-1", 0 );

	// centering
	v_centermove  = Cvar_Get( "v_centermove", "0.15", 0 );
	v_centerspeed = Cvar_Get( "v_centerspeed", "500", 0 );

	Cmd_AddCommand( "+mlook", IN_MLookDown );
	Cmd_AddCommand( "-mlook", IN_MLookUp );

	Cmd_AddCommand( "joy_advancedupdate", Joy_AdvancedUpdate_f );

	IN_StartupMouse();
	IN_StartupJoystick();
}

void IN_Shutdown()
{
	IN_DeactivateMouse();
}

/**
 * Called when the main window gains or loses focus.
 * The window may have been destroyed and recreated
 * between a deactivate and an activate.
 */
void IN_Activate( bool active )
{
	Key_ClearStates();

	in_appactive  = active;
	isMouseActive = !active;// force a new window check or turn off
}

/**
 * Called every frame, even if not generating commands.
 */
void IN_Frame()
{
	if ( !mouseinitialized )
	{
		return;
	}

	if ( !in_mouse || !in_appactive )
	{
		IN_DeactivateMouse();
		return;
	}

	if ( !cl.refresh_prepped || cls.key_dest == key_console || cls.key_dest == key_menu )
	{
		// temporarily deactivate if in fullscreen
		if ( Cvar_VariableValue( "vid_fullscreen" ) == 0 )
		{
			IN_DeactivateMouse();
			return;
		}
	}

	IN_ActivateMouse();
}

void IN_Move( usercmd_t *cmd )
{
	IN_MouseMove( cmd );
	IN_JoyMove( cmd );
}

void IN_ClearStates()
{
	mx_accum             = 0;
	my_accum             = 0;
	mouse_oldbuttonstate = 0;
}


/*
=========================================================================

JOYSTICK

=========================================================================
*/

void IN_StartupJoystick()
{
#if 0
	int      numdevs;
	JOYCAPS  jc;
	MMRESULT mmr;
	cvar_t  *cv;

	// assume no joystick
	joy_avail = false;

	// abort startup if user requests no joystick
	cv = Cvar_Get( "in_initjoy", "1", CVAR_NOSET );
	if ( !cv->value )
		return;

	// verify joystick driver is present
	if ( ( numdevs = joyGetNumDevs() ) == 0 )
	{
		//		Com_Printf ("\njoystick not found -- driver not present\n\n");
		return;
	}

	// cycle through the joystick ids for the first valid one
	for ( joy_id = 0; joy_id < numdevs; joy_id++ )
	{
		memset( &ji, 0, sizeof( ji ) );
		ji.dwSize = sizeof( ji );
		ji.dwFlags = JOY_RETURNCENTERED;

		if ( ( mmr = joyGetPosEx( joy_id, &ji ) ) == JOYERR_NOERROR )
			break;
	}

	// abort startup if we didn't find a valid joystick
	if ( mmr != JOYERR_NOERROR )
	{
		Com_Printf( "\njoystick not found -- no valid joysticks (%x)\n\n", mmr );
		return;
	}

	// get the capabilities of the selected joystick
	// abort startup if command fails
	memset( &jc, 0, sizeof( jc ) );
	if ( ( mmr = joyGetDevCaps( joy_id, &jc, sizeof( jc ) ) ) != JOYERR_NOERROR )
	{
		Com_Printf( "\njoystick not found -- invalid joystick capabilities (%x)\n\n", mmr );
		return;
	}

	// save the joystick's number of buttons and POV status
	joy_numbuttons = jc.wNumButtons;
	joy_haspov = jc.wCaps & JOYCAPS_HASPOV;

	// old button and POV states default to no buttons pressed
	joy_oldbuttonstate = joy_oldpovstate = 0;

	// mark the joystick as available and advanced initialization not completed
	// this is needed as cvars are not available during initialization

	joy_avail = true;
	joy_advancedinit = false;

	Com_Printf( "\njoystick detected\n\n" );
#endif
}

void Joy_AdvancedUpdate_f()
{
#if 0
	// called once by IN_ReadJoystick and by user whenever an update is needed
	// cvars are now available
	int   i;
	DWORD dwTemp;

	// initialize all the maps
	for ( i = 0; i < JOY_MAX_AXES; i++ )
	{
		dwAxisMap[ i ] = AxisNada;
		dwControlMap[ i ] = JOY_ABSOLUTE_AXIS;
		pdwRawValue[ i ] = RawValuePointer( i );
	}

	if ( joy_advanced->value == 0.0 )
	{
		// default joystick initialization
		// 2 axes only with joystick control
		dwAxisMap[ JOY_AXIS_X ] = AxisTurn;
		// dwControlMap[JOY_AXIS_X] = JOY_ABSOLUTE_AXIS;
		dwAxisMap[ JOY_AXIS_Y ] = AxisForward;
		// dwControlMap[JOY_AXIS_Y] = JOY_ABSOLUTE_AXIS;
	}
	else
	{
		if ( strcmp( joy_name->string, "joystick" ) != 0 )
		{
			// notify user of advanced controller
			Com_Printf( "\n%s configured\n\n", joy_name->string );
		}

		// advanced initialization here
		// data supplied by user via joy_axisn cvars
		dwTemp = ( DWORD ) joy_advaxisx->value;
		dwAxisMap[ JOY_AXIS_X ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_X ] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = ( DWORD ) joy_advaxisy->value;
		dwAxisMap[ JOY_AXIS_Y ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_Y ] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = ( DWORD ) joy_advaxisz->value;
		dwAxisMap[ JOY_AXIS_Z ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_Z ] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = ( DWORD ) joy_advaxisr->value;
		dwAxisMap[ JOY_AXIS_R ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_R ] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = ( DWORD ) joy_advaxisu->value;
		dwAxisMap[ JOY_AXIS_U ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_U ] = dwTemp & JOY_RELATIVE_AXIS;
		dwTemp = ( DWORD ) joy_advaxisv->value;
		dwAxisMap[ JOY_AXIS_V ] = dwTemp & 0x0000000f;
		dwControlMap[ JOY_AXIS_V ] = dwTemp & JOY_RELATIVE_AXIS;
	}

	// compute the axes to collect from DirectInput
	joy_flags = JOY_RETURNCENTERED | JOY_RETURNBUTTONS | JOY_RETURNPOV;
	for ( i = 0; i < JOY_MAX_AXES; i++ )
	{
		if ( dwAxisMap[ i ] != AxisNada )
		{
			joy_flags |= dwAxisFlags[ i ];
		}
	}
#endif
}

void IN_Commands()
{
#if 0
	int   i, key_index;
	DWORD buttonstate, povstate;

	if ( !joy_avail )
	{
		return;
	}


	// loop through the joystick buttons
	// key a joystick event or auxillary event for higher number buttons for each state change
	buttonstate = ji.dwButtons;
	for ( i = 0; i < joy_numbuttons; i++ )
	{
		if ( ( buttonstate & ( 1 << i ) ) && !( joy_oldbuttonstate & ( 1 << i ) ) )
		{
			key_index = ( i < 4 ) ? K_JOY1 : K_AUX1;
			Key_Event( key_index + i, true, 0 );
		}

		if ( !( buttonstate & ( 1 << i ) ) && ( joy_oldbuttonstate & ( 1 << i ) ) )
		{
			key_index = ( i < 4 ) ? K_JOY1 : K_AUX1;
			Key_Event( key_index + i, false, 0 );
		}
	}
	joy_oldbuttonstate = buttonstate;

	if ( joy_haspov )
	{
		// convert POV information into 4 bits of state information
		// this avoids any potential problems related to moving from one
		// direction to another without going through the center position
		povstate = 0;
		if ( ji.dwPOV != JOY_POVCENTERED )
		{
			if ( ji.dwPOV == JOY_POVFORWARD )
				povstate |= 0x01;
			if ( ji.dwPOV == JOY_POVRIGHT )
				povstate |= 0x02;
			if ( ji.dwPOV == JOY_POVBACKWARD )
				povstate |= 0x04;
			if ( ji.dwPOV == JOY_POVLEFT )
				povstate |= 0x08;
		}
		// determine which bits have changed and key an auxillary event for each change
		for ( i = 0; i < 4; i++ )
		{
			if ( ( povstate & ( 1 << i ) ) && !( joy_oldpovstate & ( 1 << i ) ) )
			{
				Key_Event( K_AUX29 + i, true, 0 );
			}

			if ( !( povstate & ( 1 << i ) ) && ( joy_oldpovstate & ( 1 << i ) ) )
			{
				Key_Event( K_AUX29 + i, false, 0 );
			}
		}
		joy_oldpovstate = povstate;
	}
#endif
}

bool IN_ReadJoystick()
{
#if 0
	memset( &ji, 0, sizeof( ji ) );
	ji.dwSize = sizeof( ji );
	ji.dwFlags = joy_flags;

	if ( joyGetPosEx( joy_id, &ji ) == JOYERR_NOERROR )
	{
		return true;
	}
#endif

	return false;
}

void IN_JoyMove( usercmd_t *cmd )
{
#if 0
	float speed, aspeed;
	float fAxisValue;
	int   i;

	// complete initialization if first time in
	// this is needed as cvars are not available at initialization time
	if ( joy_advancedinit != true )
	{
		Joy_AdvancedUpdate_f();
		joy_advancedinit = true;
	}

	// verify joystick is available and that the user wants to use it
	if ( !joy_avail || !in_joystick->value )
	{
		return;
	}

	// collect the joystick data, if possible
	if ( IN_ReadJoystick() != true )
	{
		return;
	}

	if ( ( in_speed.state & 1 ) ^ ( int ) cl_run->value )
		speed = 2;
	else
		speed = 1;
	aspeed = speed * cls.frametime;

	// loop through the axes
	for ( i = 0; i < JOY_MAX_AXES; i++ )
	{
		// get the floating point zero-centered, potentially-inverted data for the current axis
		fAxisValue = ( float ) *pdwRawValue[ i ];
		// move centerpoint to zero
		fAxisValue -= 32768.0;

		// convert range from -32768..32767 to -1..1
		fAxisValue /= 32768.0;

		switch ( dwAxisMap[ i ] )
		{
			case AxisForward:
				if ( ( joy_advanced->value == 0.0 ) && mlooking )
				{
					// user wants forward control to become look control
					if ( std::fabs( fAxisValue ) > joy_pitchthreshold->value )
					{
						// if mouse invert is on, invert the joystick pitch value
						// only absolute control support here (joy_advanced is false)
						if ( m_pitch->value < 0.0 )
						{
							cl.viewangles[ PITCH ] -= ( fAxisValue * joy_pitchsensitivity->value ) * aspeed * cl_pitchspeed->value;
						}
						else
						{
							cl.viewangles[ PITCH ] += ( fAxisValue * joy_pitchsensitivity->value ) * aspeed * cl_pitchspeed->value;
						}
					}
				}
				else
				{
					// user wants forward control to be forward control
					if ( std::fabs( fAxisValue ) > joy_forwardthreshold->value )
					{
						cmd->forwardmove += ( fAxisValue * joy_forwardsensitivity->value ) * speed * cl_forwardspeed->value;
					}
				}
				break;

			case AxisSide:
				if ( std::fabs( fAxisValue ) > joy_sidethreshold->value )
				{
					cmd->sidemove += ( fAxisValue * joy_sidesensitivity->value ) * speed * cl_sidespeed->value;
				}
				break;

			case AxisUp:
				if ( std::fabs( fAxisValue ) > joy_upthreshold->value )
				{
					cmd->upmove += ( fAxisValue * joy_upsensitivity->value ) * speed * cl_upspeed->value;
				}
				break;

			case AxisTurn:
				if ( ( in_strafe.state & 1 ) || ( lookstrafe->value && mlooking ) )
				{
					// user wants turn control to become side control
					if ( std::fabs( fAxisValue ) > joy_sidethreshold->value )
					{
						cmd->sidemove -= ( fAxisValue * joy_sidesensitivity->value ) * speed * cl_sidespeed->value;
					}
				}
				else
				{
					// user wants turn control to be turn control
					if ( std::fabs( fAxisValue ) > joy_yawthreshold->value )
					{
						if ( dwControlMap[ i ] == JOY_ABSOLUTE_AXIS )
						{
							cl.viewangles[ YAW ] += ( fAxisValue * joy_yawsensitivity->value ) * aspeed * cl_yawspeed->value;
						}
						else
						{
							cl.viewangles[ YAW ] += ( fAxisValue * joy_yawsensitivity->value ) * speed * 180.0;
						}
					}
				}
				break;

			case AxisLook:
				if ( mlooking )
				{
					if ( std::fabs( fAxisValue ) > joy_pitchthreshold->value )
					{
						// pitch movement detected and pitch movement desired by user
						if ( dwControlMap[ i ] == JOY_ABSOLUTE_AXIS )
						{
							cl.viewangles[ PITCH ] += ( fAxisValue * joy_pitchsensitivity->value ) * aspeed * cl_pitchspeed->value;
						}
						else
						{
							cl.viewangles[ PITCH ] += ( fAxisValue * joy_pitchsensitivity->value ) * speed * 180.0;
						}
					}
				}
				break;

			default:
				break;
		}
	}
#endif
}
