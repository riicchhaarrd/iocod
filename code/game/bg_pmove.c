/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
//
// bg_pmove.c -- both games player movement code
// takes a playerstate and a usercmd as input and returns a modifed playerstate

#include "../qcommon/q_shared.h"
#include "bg_public.h"
#include "bg_local.h"

pmove_t		*pm;
pml_t		pml;

// movement parameters
float	pm_stopspeed = 100.0f;
#ifdef STANDALONE
float	pm_duckScale = 0.65f;			// CoD1: crouch = 65% of standing speed
float	pm_proneScale = 0.15f;			// CoD1: prone = 15% of standing speed
#else
float	pm_duckScale = 0.25f;
#endif
float	pm_swimScale = 0.50f;

float	pm_accelerate = 9.0f;			// CoD1: 9.0 (Q3: 10.0)
float	pm_airaccelerate = 1.0f;
float	pm_wateraccelerate = 4.0f;
float	pm_flyaccelerate = 8.0f;

float	pm_friction = 5.5f;			// CoD1: 5.5 (Q3: 6.0)
float	pm_waterfriction = 1.0f;
float	pm_flightfriction = 3.0f;
float	pm_spectatorfriction = 5.0f;

float	pm_ladderScale = 0.5f;

// CoD1-specific movement parameters (extracted from game.mp.i386.so)
float	pm_waterSwimScale = 0.5f;		// swim speed scale (CoD1: pm_waterSwimScale)
float	pm_waterWadeScale = 0.7f;		// wade (shallow water) speed scale
float	pm_ducked_accelerate = 12.0f;	// acceleration while crouched
float	pm_prone_accelerate = 19.0f;	// acceleration while prone
int		pm_ladderJumpTime = 300;		// ms before ladder re-grab allowed after jump
float	pm_ladderfriction = 16.0f;		// friction while on ladder
float	pm_ladderPushVel = 128.0f;		// velocity when jumping off ladder
float	pm_shellshockScale = 0.4f;		// movement scale during shellshock

int		c_pmove = 0;


/*
===============
PM_AddEvent

===============
*/
void PM_AddEvent( int newEvent ) {
	BG_AddPredictableEventToPlayerstate( newEvent, 0, pm->ps );
}

/*
===============
PM_AddTouchEnt
===============
*/
void PM_AddTouchEnt( int entityNum ) {
	int		i;

	if ( entityNum == ENTITYNUM_WORLD ) {
		return;
	}
	if ( pm->numtouch == MAXTOUCH ) {
		return;
	}

	// see if it is already added
	for ( i = 0 ; i < pm->numtouch ; i++ ) {
		if ( pm->touchents[ i ] == entityNum ) {
			return;
		}
	}

	// add it
	pm->touchents[pm->numtouch] = entityNum;
	pm->numtouch++;
}

/*
===================
PM_StartTorsoAnim
===================
*/
static void PM_StartTorsoAnim( int anim ) {
	if ( pm->ps->pm_type >= PM_DEAD ) {
		return;
	}
	pm->ps->torsoAnim = ( ( pm->ps->torsoAnim & ANIM_TOGGLEBIT ) ^ ANIM_TOGGLEBIT )
		| anim;
}
static void PM_StartLegsAnim( int anim ) {
	if ( pm->ps->pm_type >= PM_DEAD ) {
		return;
	}
	if ( pm->ps->legsTimer > 0 ) {
		return;		// a high priority animation is running
	}
	pm->ps->legsAnim = ( ( pm->ps->legsAnim & ANIM_TOGGLEBIT ) ^ ANIM_TOGGLEBIT )
		| anim;
}

static void PM_ContinueLegsAnim( int anim ) {
	if ( ( pm->ps->legsAnim & ~ANIM_TOGGLEBIT ) == anim ) {
		return;
	}
	if ( pm->ps->legsTimer > 0 ) {
		return;		// a high priority animation is running
	}
	PM_StartLegsAnim( anim );
}

static void PM_ContinueTorsoAnim( int anim ) {
	if ( ( pm->ps->torsoAnim & ~ANIM_TOGGLEBIT ) == anim ) {
		return;
	}
	if ( pm->ps->torsoTimer > 0 ) {
		return;		// a high priority animation is running
	}
	PM_StartTorsoAnim( anim );
}

static void PM_ForceLegsAnim( int anim ) {
	pm->ps->legsTimer = 0;
	PM_StartLegsAnim( anim );
}


/*
==================
PM_ClipVelocity

Slide off of the impacting surface
==================
*/
void PM_ClipVelocity( vec3_t in, vec3_t normal, vec3_t out, float overbounce ) {
	float	backoff;
	float	change;
	int		i;
	
	backoff = DotProduct (in, normal);
	
	if ( backoff < 0 ) {
		backoff *= overbounce;
	} else {
		backoff /= overbounce;
	}

	for ( i=0 ; i<3 ; i++ ) {
		change = normal[i]*backoff;
		out[i] = in[i] - change;
	}
}


/*
==================
PM_Friction

Handles both ground friction and water friction
==================
*/
static void PM_Friction( void ) {
	vec3_t	vec;
	float	*vel;
	float	speed, newspeed, control;
	float	drop;
	
	vel = pm->ps->velocity;
	
	VectorCopy( vel, vec );
	if ( pml.walking && !(pm->ps->pm_flags & PMF_ON_LADDER) ) {
		vec[2] = 0;	// ignore slope movement
	}

	speed = VectorLength(vec);
	if (speed < 1) {
		vel[0] = 0;
		vel[1] = 0;		// allow sinking underwater
		// FIXME: still have z friction underwater?
		return;
	}

	drop = 0;

	// apply ground friction
	if ( pm->waterlevel <= 1 ) {
		if ( pml.walking && !(pml.groundTrace.surfaceFlags & SURF_SLICK) ) {
			// if getting knocked back, no friction
			if ( ! (pm->ps->pm_flags & PMF_TIME_KNOCKBACK) ) {
#ifdef STANDALONE
				// CoD1: stance-based friction speed
				// Prone: use 30% of speed, Sprint: use 200% of speed
				{
					float frictionSpeed = speed;
					if ( pm->ps->pm_flags & PMF_PRONE ) {
						frictionSpeed = speed * 0.3f;
					} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
						// ducked: normal friction
					}
					control = frictionSpeed < pm_stopspeed ? pm_stopspeed : frictionSpeed;
				}
#else
				control = speed < pm_stopspeed ? pm_stopspeed : speed;
#endif
				drop += control*pm_friction*pml.frametime;
			}
		}
	}

	// apply water friction even if just wading
	if ( pm->waterlevel ) {
		drop += speed*pm_waterfriction*pm->waterlevel*pml.frametime;
	}

#ifndef STANDALONE
	// apply flying friction
	if ( pm->ps->powerups[PW_FLIGHT]) {
		drop += speed*pm_flightfriction*pml.frametime;
	}
#endif

	// Note: ladder friction is handled inside PM_LadderMove, not here

	if ( pm->ps->pm_type == PM_SPECTATOR) {
		drop += speed*pm_spectatorfriction*pml.frametime;
	}

	// scale the velocity
	newspeed = speed - drop;
	if (newspeed < 0) {
		newspeed = 0;
	}
	newspeed /= speed;

	vel[0] = vel[0] * newspeed;
	vel[1] = vel[1] * newspeed;
	vel[2] = vel[2] * newspeed;
}


/*
==============
PM_Accelerate

Handles user intended acceleration
==============
*/
static void PM_Accelerate( vec3_t wishdir, float wishspeed, float accel ) {
#if 1
	// q2 style
	int			i;
	float		addspeed, accelspeed, currentspeed;

	currentspeed = DotProduct (pm->ps->velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0) {
		return;
	}
	accelspeed = accel*pml.frametime*wishspeed;
	if (accelspeed > addspeed) {
		accelspeed = addspeed;
	}
	
	for (i=0 ; i<3 ; i++) {
		pm->ps->velocity[i] += accelspeed*wishdir[i];	
	}
#else
	// proper way (avoids strafe jump maxspeed bug), but feels bad
	vec3_t		wishVelocity;
	vec3_t		pushDir;
	float		pushLen;
	float		canPush;

	VectorScale( wishdir, wishspeed, wishVelocity );
	VectorSubtract( wishVelocity, pm->ps->velocity, pushDir );
	pushLen = VectorNormalize( pushDir );

	canPush = accel*pml.frametime*wishspeed;
	if (canPush > pushLen) {
		canPush = pushLen;
	}

	VectorMA( pm->ps->velocity, canPush, pushDir, pm->ps->velocity );
#endif
}



/*
============
PM_CmdScale

Returns the scale factor to apply to cmd movements
This allows the clients to use axial -127 to 127 values for all directions
without getting a sqrt(2) distortion in speed.
============
*/
static float PM_CmdScale( usercmd_t *cmd ) {
	int		max;
	float	total;
	float	scale;

	max = abs( cmd->forwardmove );
	if ( abs( cmd->rightmove ) > max ) {
		max = abs( cmd->rightmove );
	}
	if ( abs( cmd->upmove ) > max ) {
		max = abs( cmd->upmove );
	}
	if ( !max ) {
		return 0;
	}

	total = sqrt( cmd->forwardmove * cmd->forwardmove
		+ cmd->rightmove * cmd->rightmove + cmd->upmove * cmd->upmove );
	scale = (float)pm->ps->speed * max / ( 127.0 * total );

#ifdef STANDALONE
	// CoD1: pm_type speed multipliers
	if ( pm->ps->pm_type == PM_NOCLIP ) {
		scale *= 3.0f;
	} else if ( pm->ps->pm_type == PM_UFO ) {
		scale *= 6.0f;
	}
#endif

	return scale;
}


/*
================
PM_SetMovementDir

Determine the rotation of the legs relative
to the facing dir
================
*/
static void PM_SetMovementDir( void ) {
	if ( pm->cmd.forwardmove || pm->cmd.rightmove ) {
		if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 0;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 1;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 2;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 3;
		} else if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 4;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 5;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 6;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 7;
		}
	} else {
		// if they aren't actively going directly sideways,
		// change the animation to the diagonal so they
		// don't stop too crooked
		if ( pm->ps->movementDir == 2 ) {
			pm->ps->movementDir = 1;
		} else if ( pm->ps->movementDir == 6 ) {
			pm->ps->movementDir = 7;
		} 
	}
}

/*
=============
PM_CheckJump
=============
*/
static qboolean PM_CheckJump( void ) {
	if ( pm->ps->pm_flags & PMF_RESPAWNED ) {
		return qfalse;		// don't allow jump until all buttons are up
	}

	if ( pm->cmd.upmove < 10 ) {
		// not holding jump
		return qfalse;
	}

#ifdef STANDALONE
	// CoD1: 500ms cooldown between jumps (replaces PMF_TIME_LAND anti-bunny-hop)
	if ( pm->cmd.serverTime - pm->ps->jumpTime <= 499 ) {
		pm->cmd.upmove = 0;	// clear so cmdscale doesn't slow ground movement
		return qfalse;
	}
#endif

	// first jump press while prone transitions out of prone stance
	if ( pm->ps->pm_flags & PMF_PRONE ) {
		pm->cmd.upmove = 0;
		pm->ps->pm_flags &= ~PMF_PRONE;
		return qfalse;
	}

	// must wait for jump to be released
	if ( pm->ps->pm_flags & PMF_JUMP_HELD ) {
		// clear upmove so cmdscale doesn't lower running speed
		pm->cmd.upmove = 0;
		return qfalse;
	}

	pml.groundPlane = qfalse;		// jumping away
	pml.walking = qfalse;
	pm->ps->pm_flags |= PMF_JUMP_HELD;

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	// CoD1: velocity = sqrt(2 * jump_height * gravity), jump_height = 39 units
	pm->ps->velocity[2] = (float)sqrt( 2.0f * JUMP_HEIGHT * (float)pm->ps->gravity );
#ifdef STANDALONE
	// CoD1: store expected jump peak for PM_StepSlideMove
	pm->ps->jumpOriginZ = pm->ps->origin[2] + JUMP_HEIGHT;
	pm->ps->jumpTime = pm->cmd.serverTime;	// 500ms cooldown starts now
#endif
	PM_AddEvent( EV_JUMP );

	if ( pm->cmd.forwardmove >= 0 ) {
		PM_ForceLegsAnim( LEGS_JUMP );
		pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
	} else {
		PM_ForceLegsAnim( LEGS_JUMPB );
		pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
	}

	return qtrue;
}

/*
=============
PM_CheckWaterJump
=============
*/
static qboolean	PM_CheckWaterJump( void ) {
	vec3_t	spot;
	int		cont;
	vec3_t	flatforward;

	if (pm->ps->pm_time) {
		return qfalse;
	}

	// check for water jump
	if ( pm->waterlevel != 2 ) {
		return qfalse;
	}

	flatforward[0] = pml.forward[0];
	flatforward[1] = pml.forward[1];
	flatforward[2] = 0;
	VectorNormalize (flatforward);

	VectorMA (pm->ps->origin, 30, flatforward, spot);
	spot[2] += 4;
	cont = pm->pointcontents (spot, pm->ps->clientNum );
	if ( !(cont & CONTENTS_SOLID) ) {
		return qfalse;
	}

	spot[2] += 16;
	cont = pm->pointcontents (spot, pm->ps->clientNum );
	if ( cont & (CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY) ) {
		return qfalse;
	}

	// jump out of water
	VectorScale (pml.forward, 200, pm->ps->velocity);
	pm->ps->velocity[2] = 350;

	pm->ps->pm_flags |= PMF_TIME_WATERJUMP;
	pm->ps->pm_time = 2000;

	return qtrue;
}

//============================================================================


/*
===================
PM_WaterJumpMove

Flying out of the water
===================
*/
static void PM_WaterJumpMove( void ) {
	// waterjump has no control, but falls

	PM_StepSlideMove( qtrue );

	pm->ps->velocity[2] -= pm->ps->gravity * pml.frametime;
	if (pm->ps->velocity[2] < 0) {
		// cancel as soon as we are falling down again
		pm->ps->pm_flags &= ~PMF_ALL_TIMES;
		pm->ps->pm_time = 0;
	}
}

// Forward declarations needed by PM_LadderMove
static void PM_AirMove( void );

/*
===================
PM_LadderMove

Source: CoD2 bgame/bg_pmove.cpp PM_LadderMove, adapted for Q3 data structures.
CoD1 game.mp.i386.so confirms same algorithm.
===================
*/
static void PM_LadderMove( void ) {
	vec3_t	wishvel, wishdir;
	vec3_t	vTempRight;
	float	upscale, scale, wishspeed;
	float	fSideSpeed, fSpeedDrop;
	float	right2d[2], right2dLen;
	float	dot, vz2, vxy2;

	// Jump while on ladder: push away from ladder and go airborne
	if ( PM_CheckJump() ) {
		vec3_t jumpDir;

		pm->ps->pm_flags &= ~PMF_ON_LADDER;
		pm->ps->pm_flags |= PMF_TIME_LADDER;
		pm->ps->pm_time = pm_ladderJumpTime;

		// CoD2: if looking toward ladder, reflect jump direction off surface
		// otherwise jump in view direction. Either way, push away from ladder.
		VectorCopy( pml.forward, jumpDir );
		if ( DotProduct( jumpDir, pml.ladderNormal ) < 0.0f ) {
			// Looking at ladder: reflect off surface
			float d = -2.0f * DotProduct( jumpDir, pml.ladderNormal );
			VectorMA( jumpDir, d, pml.ladderNormal, jumpDir );
			VectorNormalize( jumpDir );
		}

		// Set velocity away from ladder
		pm->ps->velocity[0] = pm_ladderPushVel * jumpDir[0];
		pm->ps->velocity[1] = pm_ladderPushVel * jumpDir[1];
		pm->ps->velocity[2] = pm_ladderPushVel * jumpDir[2];

		PM_AirMove();
		return;
	}

	// upscale: converts pitch (pml.forward[2]) into climb fraction [-1=down, +1=up].
	// +0.25 bias: looking horizontally gives slight upward tendency.
	upscale = ( pml.forward[2] + 0.25f ) * 2.5f;
	if ( upscale >  1.0f ) upscale =  1.0f;
	if ( upscale < -1.0f ) upscale = -1.0f;

	// Flatten forward/right to horizontal plane (z=0)
	pml.forward[2] = 0.0f;
	VectorNormalize( pml.forward );

	vTempRight[0] = pml.right[0];
	vTempRight[1] = pml.right[1];
	vTempRight[2] = 0.0f;
	VectorNormalize( vTempRight );
	// Project the horizontal right vector onto the ladder surface plane so
	// strafing stays tangent to the wall. Q3: ProjectPointOnPlane(dst, p, normal).
	ProjectPointOnPlane( pml.right, vTempRight, pml.ladderNormal );

	scale = PM_CmdScale( &pm->cmd );
	VectorClear( wishvel );

	// forwardmove: vertical wish via upscale (0.5 * scale matches CoD2 source)
	if ( pm->cmd.forwardmove ) {
		wishvel[2] = upscale * 0.5f * scale * pm->cmd.forwardmove;
	}

	// rightmove: horizontal wish at 0.2 scale along ladder-plane right
	if ( pm->cmd.rightmove ) {
		VectorMA( wishvel, scale * 0.2f * pm->cmd.rightmove, pml.right, wishvel );
	}

	wishspeed = VectorNormalize2( wishvel, wishdir );
	PM_Accelerate( wishdir, wishspeed, pm_accelerate );

	// No forwardmove: decay vertical velocity toward zero (gravity-damped hold)
	if ( !pm->cmd.forwardmove ) {
		if ( pm->ps->velocity[2] > 0.0f ) {
			pm->ps->velocity[2] -= pm->ps->gravity * pml.frametime;
			if ( pm->ps->velocity[2] < 0.0f ) pm->ps->velocity[2] = 0.0f;
		} else {
			pm->ps->velocity[2] += pm->ps->gravity * pml.frametime;
			if ( pm->ps->velocity[2] > 0.0f ) pm->ps->velocity[2] = 0.0f;
		}
	}

	// No rightmove: lateral friction - damp velocity along the right axis
	if ( !pm->cmd.rightmove ) {
		right2d[0] = pml.right[0];
		right2d[1] = pml.right[1];
		right2dLen = (float)sqrt( right2d[0]*right2d[0] + right2d[1]*right2d[1] );
		if ( right2dLen > 0.001f ) {
			right2d[0] /= right2dLen;
			right2d[1] /= right2dLen;
		}
		fSideSpeed = right2d[0]*pm->ps->velocity[0] + right2d[1]*pm->ps->velocity[1];
		if ( fSideSpeed != 0.0f ) {
			// Strip lateral component, then re-add a friction-damped version
			pm->ps->velocity[0] -= fSideSpeed * right2d[0];
			pm->ps->velocity[1] -= fSideSpeed * right2d[1];
			fSpeedDrop = fSideSpeed * pml.frametime * pm_ladderfriction;
			if ( fabs( fSideSpeed ) > fabs( fSpeedDrop ) ) {
				if ( fabs( fSpeedDrop ) < 1.0f ) {
					fSpeedDrop = ( fSpeedDrop >= 0.0f ) ? 1.0f : -1.0f;
				}
				pm->ps->velocity[0] += ( fSideSpeed - fSpeedDrop ) * right2d[0];
				pm->ps->velocity[1] += ( fSideSpeed - fSpeedDrop ) * right2d[1];
			}
			// else: |side| <= |drop|, component stays zeroed (full stop)
		}
	}

	// Airborne on ladder: keep player pressed against the surface
	if ( !pml.walking ) {
		// Remove velocity component going through the wall
		dot = pml.ladderNormal[0]*pm->ps->velocity[0] + pml.ladderNormal[1]*pm->ps->velocity[1];
		pm->ps->velocity[0] -= dot * pml.ladderNormal[0];
		pm->ps->velocity[1] -= dot * pml.ladderNormal[1];

		// If moving more vertically than horizontally, push into the ladder
		vz2  = pm->ps->velocity[2] * pm->ps->velocity[2];
		vxy2 = pm->ps->velocity[0]*pm->ps->velocity[0] + pm->ps->velocity[1]*pm->ps->velocity[1];
		if ( vz2 >= vxy2 ) {
			pm->ps->velocity[0] -= 50.0f * pml.ladderNormal[0];
			pm->ps->velocity[1] -= 50.0f * pml.ladderNormal[1];
		}
	}

	PM_StepSlideMove( qfalse );

	// Yaw movement direction for animations (clamped to +-75 from ladder face).
	// CoD2: writes to ps->movementDir as signed char (not delta_angles).
	// NOTE: CG must handle signed values as direct angle offsets.
	{
		vec3_t ladderAngles;
		int moveyaw;
		vectoangles( pml.ladderNormal, ladderAngles );
		moveyaw = (int)AngleDelta( ladderAngles[YAW] + 180.0f, pm->ps->viewangles[YAW] );
		if ( moveyaw >  75 ) moveyaw =  75;
		if ( moveyaw < -75 ) moveyaw = -75;
		pm->ps->movementDir = (signed char)moveyaw;
	}
}

/*
===================
PM_WaterMove

===================
*/
static void PM_WaterMove( void ) {
	int		i;
	vec3_t	wishvel;
	float	wishspeed;
	vec3_t	wishdir;
	float	scale;
	float	vel;

	if ( PM_CheckWaterJump() ) {
		PM_WaterJumpMove();
		return;
	}
#if 0
	// jump = head for surface
	if ( pm->cmd.upmove >= 10 ) {
		if (pm->ps->velocity[2] > -300) {
			if ( pm->watertype & CONTENTS_WATER ) {
				pm->ps->velocity[2] = 100;
			} else if ( pm->watertype & CONTENTS_SLIME ) {
				pm->ps->velocity[2] = 80;
			} else {
				pm->ps->velocity[2] = 50;
			}
		}
	}
#endif
	PM_Friction ();

	scale = PM_CmdScale( &pm->cmd );
	//
	// user intentions
	//
	if ( !scale ) {
		wishvel[0] = 0;
		wishvel[1] = 0;
		wishvel[2] = -60;		// sink towards bottom
	} else {
		for (i=0 ; i<3 ; i++)
			wishvel[i] = scale * pml.forward[i]*pm->cmd.forwardmove + scale * pml.right[i]*pm->cmd.rightmove;

		wishvel[2] += scale * pm->cmd.upmove;
	}

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	if ( wishspeed > pm->ps->speed * pm_swimScale ) {
		wishspeed = pm->ps->speed * pm_swimScale;
	}

	PM_Accelerate (wishdir, wishspeed, pm_wateraccelerate);

	// make sure we can go up slopes easily under water
	if ( pml.groundPlane && DotProduct( pm->ps->velocity, pml.groundTrace.plane.normal ) < 0 ) {
		vel = VectorLength(pm->ps->velocity);
		// slide along the ground plane
		PM_ClipVelocity (pm->ps->velocity, pml.groundTrace.plane.normal, 
			pm->ps->velocity, OVERCLIP );

		VectorNormalize(pm->ps->velocity);
		VectorScale(pm->ps->velocity, vel, pm->ps->velocity);
	}

	PM_SlideMove( qfalse );
}

#ifdef MISSIONPACK
/*
===================
PM_InvulnerabilityMove

Only with the invulnerability powerup
===================
*/
static void PM_InvulnerabilityMove( void ) {
	pm->cmd.forwardmove = 0;
	pm->cmd.rightmove = 0;
	pm->cmd.upmove = 0;
	VectorClear(pm->ps->velocity);
}
#endif

/*
===================
PM_FlyMove

Only with the flight powerup
===================
*/
static void PM_FlyMove( void ) {
	int		i;
	vec3_t	wishvel;
	float	wishspeed;
	vec3_t	wishdir;
	float	scale;

	// normal slowdown
	PM_Friction ();

	scale = PM_CmdScale( &pm->cmd );
	//
	// user intentions
	//
	if ( !scale ) {
		wishvel[0] = 0;
		wishvel[1] = 0;
		wishvel[2] = 0;
	} else {
		for (i=0 ; i<3 ; i++) {
			wishvel[i] = scale * pml.forward[i]*pm->cmd.forwardmove + scale * pml.right[i]*pm->cmd.rightmove;
		}

		wishvel[2] += scale * pm->cmd.upmove;
	}

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);

	PM_Accelerate (wishdir, wishspeed, pm_flyaccelerate);

	PM_StepSlideMove( qfalse );
}


/*
===================
PM_AirMove

===================
*/
static void PM_AirMove( void ) {
	int			i;
	vec3_t		wishvel;
	float		fmove, smove;
	vec3_t		wishdir;
	float		wishspeed;
	float		scale;
	usercmd_t	cmd;

	PM_Friction();

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;

	cmd = pm->cmd;
	scale = PM_CmdScale( &cmd );

	// set the movementDir so clients can rotate the legs for strafing
	PM_SetMovementDir();

	// project moves down to flat plane
	pml.forward[2] = 0;
	pml.right[2] = 0;
	VectorNormalize (pml.forward);
	VectorNormalize (pml.right);

	for ( i = 0 ; i < 2 ; i++ ) {
		wishvel[i] = pml.forward[i]*fmove + pml.right[i]*smove;
	}
	wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);
	wishspeed *= scale;

	// not on ground, so little effect on velocity
	PM_Accelerate (wishdir, wishspeed, pm_airaccelerate);

	// we may have a ground plane that is very steep, even
	// though we don't have a groundentity
	// slide along the steep plane
	if ( pml.groundPlane ) {
		PM_ClipVelocity (pm->ps->velocity, pml.groundTrace.plane.normal, 
			pm->ps->velocity, OVERCLIP );
	}

#if 0
	//ZOID:  If we are on the grapple, try stair-stepping
	//this allows a player to use the grapple to pull himself
	//over a ledge
	if (pm->ps->pm_flags & PMF_GRAPPLE_PULL)
		PM_StepSlideMove ( qtrue );
	else
		PM_SlideMove ( qtrue );
#endif

	PM_StepSlideMove ( qtrue );
}

/*
===================
PM_GrappleMove

===================
*/
static void PM_GrappleMove( void ) {
	vec3_t vel, v;
	float vlen;

	VectorScale(pml.forward, -16, v);
	VectorAdd(pm->ps->grapplePoint, v, v);
	VectorSubtract(v, pm->ps->origin, vel);
	vlen = VectorLength(vel);
	VectorNormalize( vel );

	if (vlen <= 100)
		VectorScale(vel, 10 * vlen, vel);
	else
		VectorScale(vel, 800, vel);

	VectorCopy(vel, pm->ps->velocity);

	pml.groundPlane = qfalse;
}

/*
===================
PM_WalkMove

===================
*/
static void PM_WalkMove( void ) {
	int			i;
	vec3_t		wishvel;
	float		fmove, smove;
	vec3_t		wishdir;
	float		wishspeed;
	float		scale;
	usercmd_t	cmd;
	float		accelerate;
	float		vel;

	if ( pm->waterlevel > 2 && DotProduct( pml.forward, pml.groundTrace.plane.normal ) > 0 ) {
		// begin swimming
		PM_WaterMove();
		return;
	}


	if ( PM_CheckJump () ) {
		// jumped away
		if ( pm->waterlevel > 1 ) {
			PM_WaterMove();
		} else {
			PM_AirMove();
		}
		return;
	}

	PM_Friction ();

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;

	cmd = pm->cmd;
	scale = PM_CmdScale( &cmd );

	// set the movementDir so clients can rotate the legs for strafing
	PM_SetMovementDir();

	// project moves down to flat plane
	pml.forward[2] = 0;
	pml.right[2] = 0;

	// project the forward and right directions onto the ground plane
	PM_ClipVelocity (pml.forward, pml.groundTrace.plane.normal, pml.forward, OVERCLIP );
	PM_ClipVelocity (pml.right, pml.groundTrace.plane.normal, pml.right, OVERCLIP );
	//
	VectorNormalize (pml.forward);
	VectorNormalize (pml.right);

	for ( i = 0 ; i < 3 ; i++ ) {
		wishvel[i] = pml.forward[i]*fmove + pml.right[i]*smove;
	}
	// when going up or down slopes the wish velocity should Not be zero
//	wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);
	wishspeed *= scale;

	// clamp the speed lower if ducking or prone
#ifdef STANDALONE
	if ( pm->ps->pm_flags & PMF_PRONE ) {
		if ( wishspeed > pm->ps->speed * pm_proneScale ) {
			wishspeed = pm->ps->speed * pm_proneScale;
		}
	} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
		if ( wishspeed > pm->ps->speed * pm_duckScale ) {
			wishspeed = pm->ps->speed * pm_duckScale;
		}
	}
#else
	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		if ( wishspeed > pm->ps->speed * pm_duckScale ) {
			wishspeed = pm->ps->speed * pm_duckScale;
		}
	}
#endif

#ifdef STANDALONE
	// CoD1: landing slowdown reduces speed after jumps
	if ( pm->ps->landSlowdown < 1.0f && pm->ps->landSlowdown > 0.0f ) {
		wishspeed *= pm->ps->landSlowdown;
	}
	// CoD1: shellshock reduces movement speed (ref: line 8202, * 0.4)
	if ( pm->ps->shellshockTime > pm->cmd.serverTime ) {
		wishspeed *= pm_shellshockScale;
	}
#endif

	// clamp the speed lower if wading or walking on the bottom
	if ( pm->waterlevel ) {
		float	waterScale;

		waterScale = pm->waterlevel / 3.0;
		waterScale = 1.0 - ( 1.0 - pm_swimScale ) * waterScale;
		if ( wishspeed > pm->ps->speed * waterScale ) {
			wishspeed = pm->ps->speed * waterScale;
		}
	}

	// when a player gets hit, they temporarily lose
	// full control, which allows them to be moved a bit
	if ( ( pml.groundTrace.surfaceFlags & SURF_SLICK ) || pm->ps->pm_flags & PMF_TIME_KNOCKBACK ) {
		accelerate = pm_airaccelerate;
	} else {
#ifdef STANDALONE
		// CoD1: stance-based acceleration
		if ( pm->ps->pm_flags & PMF_PRONE ) {
			accelerate = pm_prone_accelerate;
		} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
			accelerate = pm_ducked_accelerate;
		} else {
			accelerate = pm_accelerate;
		}
#else
		accelerate = pm_accelerate;
#endif
	}

	PM_Accelerate (wishdir, wishspeed, accelerate);

	//Com_Printf("velocity = %1.1f %1.1f %1.1f\n", pm->ps->velocity[0], pm->ps->velocity[1], pm->ps->velocity[2]);
	//Com_Printf("velocity1 = %1.1f\n", VectorLength(pm->ps->velocity));

	if ( ( pml.groundTrace.surfaceFlags & SURF_SLICK ) || pm->ps->pm_flags & PMF_TIME_KNOCKBACK ) {
		pm->ps->velocity[2] -= pm->ps->gravity * pml.frametime;
	} else {
		// don't reset the z velocity for slopes
//		pm->ps->velocity[2] = 0;
	}

	vel = VectorLength(pm->ps->velocity);

	// slide along the ground plane
	PM_ClipVelocity (pm->ps->velocity, pml.groundTrace.plane.normal, 
		pm->ps->velocity, OVERCLIP );

	// don't decrease velocity when going up or down a slope
	VectorNormalize(pm->ps->velocity);
	VectorScale(pm->ps->velocity, vel, pm->ps->velocity);

	// don't do anything if standing still
	if (!pm->ps->velocity[0] && !pm->ps->velocity[1]) {
		return;
	}

	PM_StepSlideMove( qfalse );

	//Com_Printf("velocity2 = %1.1f\n", VectorLength(pm->ps->velocity));

}


/*
==============
PM_DeadMove
==============
*/
static void PM_DeadMove( void ) {
	float	forward;

	if ( !pml.walking ) {
		return;
	}

	// extra friction

	forward = VectorLength (pm->ps->velocity);
	forward -= 20;
	if ( forward <= 0 ) {
		VectorClear (pm->ps->velocity);
	} else {
		VectorNormalize (pm->ps->velocity);
		VectorScale (pm->ps->velocity, forward, pm->ps->velocity);
	}
}


/*
===============
PM_NoclipMove
===============
*/
static void PM_NoclipMove( void ) {
	float	speed, drop, friction, control, newspeed;
	int			i;
	vec3_t		wishvel;
	float		fmove, smove;
	vec3_t		wishdir;
	float		wishspeed;
	float		scale;

	pm->ps->viewheight = DEFAULT_VIEWHEIGHT;

	// friction

	speed = VectorLength (pm->ps->velocity);
	if (speed < 1)
	{
		VectorCopy (vec3_origin, pm->ps->velocity);
	}
	else
	{
		drop = 0;

		friction = pm_friction*1.5;	// extra friction
		control = speed < pm_stopspeed ? pm_stopspeed : speed;
		drop += control*friction*pml.frametime;

		// scale the velocity
		newspeed = speed - drop;
		if (newspeed < 0)
			newspeed = 0;
		newspeed /= speed;

		VectorScale (pm->ps->velocity, newspeed, pm->ps->velocity);
	}

	// accelerate
	scale = PM_CmdScale( &pm->cmd );

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;
	
	for (i=0 ; i<3 ; i++)
		wishvel[i] = pml.forward[i]*fmove + pml.right[i]*smove;
	wishvel[2] += pm->cmd.upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);
	wishspeed *= scale;

	PM_Accelerate( wishdir, wishspeed, pm_accelerate );

	// move
	VectorMA (pm->ps->origin, pml.frametime, pm->ps->velocity, pm->ps->origin);
}

#ifdef STANDALONE
/*
===============
PM_UFOMove

CoD1 UFO mode: 3D flight like noclip but WITH collision (StepSlideMove).
Uses normal friction (5.5) and higher acceleration than noclip.
Forward/right are clipped against ground plane so you slide along walls.
===============
*/
static void PM_UFOMove( void ) {
	int		i;
	vec3_t	wishvel, wishdir;
	float	fmove, smove;
	float	wishspeed, scale, accel;

	PM_Friction();

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;
	scale = PM_CmdScale( &pm->cmd );

	// 3D movement: use full forward/right/up vectors (not flattened)
	for ( i = 0; i < 3; i++ ) {
		wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
	}
	wishvel[2] += pm->cmd.upmove;

	VectorCopy( wishvel, wishdir );
	wishspeed = VectorNormalize( wishdir );
	wishspeed *= scale;

	// CoD1: stance-based acceleration (higher than normal ground accel)
	if ( pm->ps->pm_flags & PMF_PRONE ) {
		accel = pm_prone_accelerate;
	} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
		accel = pm_ducked_accelerate;
	} else {
		accel = pm_accelerate;
	}

	PM_Accelerate( wishdir, wishspeed, accel );

	// Collision-based movement (unlike noclip which skips this)
	PM_StepSlideMove( qfalse );
}
#endif

//============================================================================

/*
================
PM_FootstepForSurface

Returns an event number appropriate for the groundsurface
================
*/
static int PM_FootstepForSurface( void ) {
	if ( pml.groundTrace.surfaceFlags & SURF_NOSTEPS ) {
		return 0;
	}
	if ( pml.groundTrace.surfaceFlags & SURF_METALSTEPS ) {
		return EV_FOOTSTEP_METAL;
	}
	return EV_FOOTSTEP;
}


/*
=================
PM_CrashLand

Check for hard landings that generate sound events
=================
*/
#ifdef STANDALONE
/*
=================
PM_CrashLand — CoD1 style

Calculates fall height from velocity and gravity, then maps to a 0-100
damage percentage using bg_fallDamageMinHeight / bg_fallDamageMaxHeight.
The percentage is sent as the event parameter; the server applies it as
a fraction of max health in ClientEvents.
=================
*/
static float bg_fallDamageMinHeight_val = 128.0f;
static float bg_fallDamageMaxHeight_val = 300.0f;

static void PM_CrashLand( void ) {
	float	dist, vel, acc, t, a, b, c, den;
	float	landingVel, fallHeight;
	int		damagePercent, stepSound;

	// decide which landing animation to use
	if ( pm->ps->pm_flags & PMF_BACKWARDS_JUMP ) {
		PM_ForceLegsAnim( LEGS_LANDB );
	} else {
		PM_ForceLegsAnim( LEGS_LAND );
	}

	pm->ps->legsTimer = TIMER_LAND;

	// calculate the exact landing velocity
	dist = pm->ps->origin[2] - pml.previous_origin[2];
	vel = pml.previous_velocity[2];
	acc = -pm->ps->gravity;

	a = acc / 2;
	b = vel;
	c = -dist;
	den = b * b - 4 * a * c;
	if ( den < 0 ) {
		return;
	}
	t = ( -b - sqrt( den ) ) / ( 2 * a );
	landingVel = -( vel + t * acc );

	// CoD1: fall height = v^2 / (2*gravity)
	fallHeight = landingVel * landingVel / ( (float)pm->ps->gravity * 2.0f );

	// never take falling damage if completely underwater
	if ( pm->waterlevel == 3 ) {
		return;
	}

	// CoD1: calculate damage percentage from fall height
	damagePercent = 0;
	if ( bg_fallDamageMinHeight_val < bg_fallDamageMaxHeight_val &&
		 bg_fallDamageMinHeight_val >= 0.0f ) {
		if ( fallHeight >= bg_fallDamageMinHeight_val &&
			 pm->ps->pm_type <= PM_NORMAL ) {
			if ( fallHeight >= bg_fallDamageMaxHeight_val ) {
				damagePercent = 100;
			} else {
				damagePercent = (int)( ( fallHeight - bg_fallDamageMinHeight_val ) /
					( bg_fallDamageMaxHeight_val - bg_fallDamageMinHeight_val ) * 100.0f );
				if ( damagePercent < 0 ) damagePercent = 0;
				if ( damagePercent > 100 ) damagePercent = 100;
			}
		}
	}

	// CoD1: crouching halves fall damage
	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		damagePercent = (int)( (float)damagePercent * 0.5f );
	}

	// Step sound based on fall height
	if ( fallHeight > 12.0f ) {
		stepSound = (int)( ( fallHeight - 12.0f ) / 26.0f * 4.0f + 4.0f );
		if ( stepSound > 24 ) stepSound = 24;
	} else {
		stepSound = 0;
	}

	// SURF_NODAMAGE is used for bounce pads
	if ( !(pml.groundTrace.surfaceFlags & SURF_NODAMAGE) ) {
		if ( damagePercent > 0 ) {
			// EV_FALL_SHORT/MEDIUM/FAR with damage percent as event parameter
			if ( damagePercent > 50 ) {
				BG_AddPredictableEventToPlayerstate( EV_FALL_FAR, damagePercent, pm->ps );
			} else if ( damagePercent > 25 ) {
				BG_AddPredictableEventToPlayerstate( EV_FALL_MEDIUM, damagePercent, pm->ps );
			} else {
				BG_AddPredictableEventToPlayerstate( EV_FALL_SHORT, damagePercent, pm->ps );
			}
		} else if ( stepSound > 0 ) {
			PM_AddEvent( EV_FALL_SHORT );
		} else {
			PM_AddEvent( PM_FootstepForSurface() );
		}
	}

	pm->ps->bobCycle = 0;
}
#else
static void PM_CrashLand( void ) {
	float		delta;
	float		dist;
	float		vel, acc;
	float		t;
	float		a, b, c, den;

	// decide which landing animation to use
	if ( pm->ps->pm_flags & PMF_BACKWARDS_JUMP ) {
		PM_ForceLegsAnim( LEGS_LANDB );
	} else {
		PM_ForceLegsAnim( LEGS_LAND );
	}

	pm->ps->legsTimer = TIMER_LAND;

	// calculate the exact velocity on landing
	dist = pm->ps->origin[2] - pml.previous_origin[2];
	vel = pml.previous_velocity[2];
	acc = -pm->ps->gravity;

	a = acc / 2;
	b = vel;
	c = -dist;

	den =  b * b - 4 * a * c;
	if ( den < 0 ) {
		return;
	}
	t = (-b - sqrt( den ) ) / ( 2 * a );

	delta = vel + t * acc;
	delta = delta*delta * 0.0001;

	// ducking while falling doubles damage
	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		delta *= 2;
	}

	// never take falling damage if completely underwater
	if ( pm->waterlevel == 3 ) {
		return;
	}

	// reduce falling damage if there is standing water
	if ( pm->waterlevel == 2 ) {
		delta *= 0.25;
	}
	if ( pm->waterlevel == 1 ) {
		delta *= 0.5;
	}

	if ( delta < 1 ) {
		return;
	}

	// create a local entity event to play the sound

	// SURF_NODAMAGE is used for bounce pads where you don't ever
	// want to take damage or play a crunch sound
	if ( !(pml.groundTrace.surfaceFlags & SURF_NODAMAGE) )  {
		if ( delta > 60 ) {
			PM_AddEvent( EV_FALL_FAR );
		} else if ( delta > 40 ) {
			// this is a pain grunt, so don't play it if dead
			if ( pm->ps->stats[STAT_HEALTH] > 0 ) {
				PM_AddEvent( EV_FALL_MEDIUM );
			}
		} else if ( delta > 7 ) {
			PM_AddEvent( EV_FALL_SHORT );
		} else {
			PM_AddEvent( PM_FootstepForSurface() );
		}
	}

	// start footstep cycle over
	pm->ps->bobCycle = 0;
}
#endif

/*
=============
PM_CheckStuck
=============
*/
/*
void PM_CheckStuck(void) {
	trace_t trace;

	pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, pm->ps->origin, pm->ps->clientNum, pm->tracemask);
	if (trace.allsolid) {
		//int shit = qtrue;
	}
}
*/

/*
=============
PM_CorrectAllSolid
=============
*/
static int PM_CorrectAllSolid( trace_t *trace ) {
	int			i, j, k;
	vec3_t		point;

	if ( pm->debugLevel ) {
		Com_Printf("%i:allsolid\n", c_pmove);
	}

	// jitter around
	for (i = -1; i <= 1; i++) {
		for (j = -1; j <= 1; j++) {
			for (k = -1; k <= 1; k++) {
				VectorCopy(pm->ps->origin, point);
				point[0] += (float) i;
				point[1] += (float) j;
				point[2] += (float) k;
				pm->trace (trace, point, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask);
				if ( !trace->allsolid ) {
					point[0] = pm->ps->origin[0];
					point[1] = pm->ps->origin[1];
					point[2] = pm->ps->origin[2] - 0.25;

					pm->trace (trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask);
					pml.groundTrace = *trace;
					return qtrue;
				}
			}
		}
	}

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pml.groundPlane = qfalse;
	pml.walking = qfalse;

	return qfalse;
}


/*
=============
PM_GroundTraceMissed

The ground trace didn't hit a surface, so we are in freefall
=============
*/
static void PM_GroundTraceMissed( void ) {
	trace_t		trace;
	vec3_t		point;

	if ( pm->ps->groundEntityNum != ENTITYNUM_NONE ) {
		// we just transitioned into freefall
		if ( pm->debugLevel ) {
			Com_Printf("%i:lift\n", c_pmove);
		}

		// if they aren't in a jumping animation and the ground is a ways away, force into it
		// if we didn't do the trace, the player would be backflipping down staircases
		VectorCopy( pm->ps->origin, point );
		point[2] -= 64;

		pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask);
		if ( trace.fraction == 1.0 ) {
			if ( pm->cmd.forwardmove >= 0 ) {
				PM_ForceLegsAnim( LEGS_JUMP );
				pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
			} else {
				PM_ForceLegsAnim( LEGS_JUMPB );
				pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
			}
		}
	}

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pml.groundPlane = qfalse;
	pml.walking = qfalse;
#ifdef STANDALONE
	pm->ps->jumpOriginZ = 0;	// not a jump, clear peak tracking
#endif
}


/*
=============
PM_GroundTrace
=============
*/
static void PM_GroundTrace( void ) {
	vec3_t		point;
	trace_t		trace;

	point[0] = pm->ps->origin[0];
	point[1] = pm->ps->origin[1];
	point[2] = pm->ps->origin[2] - 0.25;

	pm->trace (&trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask);
	pml.groundTrace = trace;

	// do something corrective if the trace starts in a solid...
	if ( trace.allsolid ) {
		if ( !PM_CorrectAllSolid(&trace) )
			return;
	}

	// if the trace didn't hit anything, we are in free fall
	if ( trace.fraction == 1.0 ) {
		PM_GroundTraceMissed();
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}

	// check if getting thrown off the ground
	if ( pm->ps->velocity[2] > 0 && DotProduct( pm->ps->velocity, trace.plane.normal ) > 10 ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:kickoff\n", c_pmove);
		}
		// go into jump animation
		if ( pm->cmd.forwardmove >= 0 ) {
			PM_ForceLegsAnim( LEGS_JUMP );
			pm->ps->pm_flags &= ~PMF_BACKWARDS_JUMP;
		} else {
			PM_ForceLegsAnim( LEGS_JUMPB );
			pm->ps->pm_flags |= PMF_BACKWARDS_JUMP;
		}

		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}
	
	// slopes that are too steep will not be considered onground
	if ( trace.plane.normal[2] < MIN_WALK_NORMAL ) {
		if ( pm->debugLevel ) {
			Com_Printf("%i:steep\n", c_pmove);
		}
		// FIXME: if they can't slide down the slope, let them
		// walk (sharp crevices)
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qtrue;
		pml.walking = qfalse;
		return;
	}

	pml.groundPlane = qtrue;
	pml.walking = qtrue;

	// hitting solid ground will end a waterjump
	if (pm->ps->pm_flags & PMF_TIME_WATERJUMP)
	{
		pm->ps->pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND);
		pm->ps->pm_time = 0;
	}

	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
		// just hit the ground
		if ( pm->debugLevel ) {
			Com_Printf("%i:Land\n", c_pmove);
		}

#ifdef STANDALONE
		pm->ps->jumpOriginZ = 0;	// landed — clear jump peak tracking
#endif
		PM_CrashLand();
	}

	pm->ps->groundEntityNum = trace.entityNum;

	// don't reset the z velocity for slopes
//	pm->ps->velocity[2] = 0;

	PM_AddTouchEnt( trace.entityNum );
}


/*
===================
PM_CheckLadder

Source: CoD2 bgame/bg_pmove.cpp PM_CheckLadderMove, adapted for Q3.
Key details:
 - Shrunk trace bounds (mins+6 XY, maxs-6 XY, mins[2]=8) to avoid false positives
 - 8-unit range when walking, 30-unit range when airborne
 - Airborne-while-on-ladder: trace back along stored normal to stay on
 - Two-trace confirmation when first grabbing: forward hit + backward re-check
 - PMF_TIME_LADDER (set on jump) blocks re-grab for pm_ladderJumpTime ms
===================
*/
static void PM_CheckLadder( void ) {
	vec3_t	checkDir, spot;
	trace_t	trace;
	vec3_t	lmins, lmaxs;
	float	traceDist;

	if ( pm->ps->pm_flags & PMF_TIME_LADDER ) {
		return;
	}
	if ( pm->ps->pm_type >= PM_DEAD ) {
		pm->ps->pm_flags &= ~PMF_ON_LADDER;
		VectorClear( pml.ladderNormal );
		return;
	}

	// Shrunk bounds for trace: avoids catching ladders at the player's corners
	VectorCopy( pm->mins, lmins );
	VectorCopy( pm->maxs, lmaxs );
	lmins[0] += 6.0f;  lmins[1] += 6.0f;  lmins[2] = 8.0f;
	lmaxs[0] -= 6.0f;  lmaxs[1] -= 6.0f;
	if ( lmins[2] > lmaxs[2] ) lmaxs[2] = lmins[2];

	traceDist = pml.walking ? 8.0f : 30.0f;

	// When already on ladder, always search toward the stored ladder surface
	// to prevent falling off when looking away.
	if ( ( pm->ps->pm_flags & PMF_ON_LADDER ) && VectorLength( pml.ladderNormal ) > 0.1f ) {
		VectorNegate( pml.ladderNormal, checkDir );
	} else {
		checkDir[0] = pml.forward[0];
		checkDir[1] = pml.forward[1];
		checkDir[2] = 0.0f;
		if ( VectorNormalize( checkDir ) == 0 ) {
			// Looking straight up/down — can't determine direction, clear ladder
			pm->ps->pm_flags &= ~PMF_ON_LADDER;
			VectorClear( pml.ladderNormal );
			return;
		}
	}

	VectorMA( pm->ps->origin, traceDist, checkDir, spot );
	pm->trace( &trace, pm->ps->origin, lmins, lmaxs, spot, pm->ps->clientNum, pm->tracemask );

	if ( trace.fraction < 1.0f && ( trace.surfaceFlags & SURF_LADDER ) ) {
		if ( !pml.walking || pm->cmd.forwardmove > 0 ) {
			if ( pm->ps->pm_flags & PMF_ON_LADDER ) {
				// Already on ladder: update normal and done
				VectorCopy( trace.plane.normal, pml.ladderNormal );
				return;
			}
			// First contact: do a second trace going INTO the ladder to confirm
			// the player is truly pressed against it (not just passing by).
			VectorCopy( trace.plane.normal, pml.ladderNormal );
			VectorNegate( pml.ladderNormal, checkDir );
			VectorMA( pm->ps->origin, traceDist, checkDir, spot );
			pm->trace( &trace, pm->ps->origin, lmins, lmaxs, spot, pm->ps->clientNum, pm->tracemask );
			if ( trace.fraction < 1.0f && ( trace.surfaceFlags & SURF_LADDER ) ) {
				pm->ps->pm_flags |= PMF_ON_LADDER;
				VectorCopy( trace.plane.normal, pml.ladderNormal );
			}
		}
	} else {
		pm->ps->pm_flags &= ~PMF_ON_LADDER;
		VectorClear( pml.ladderNormal );
	}
}

/*
=============
PM_SetWaterLevel	FIXME: avoid this twice?  certainly if not moving
=============
*/
static void PM_SetWaterLevel( void ) {
	vec3_t		point;
	int			cont;
	int			sample1;
	int			sample2;

	//
	// get waterlevel, accounting for ducking
	//
	pm->waterlevel = 0;
	pm->watertype = 0;

	point[0] = pm->ps->origin[0];
	point[1] = pm->ps->origin[1];
	point[2] = pm->ps->origin[2] + MINS_Z + 1;	
	cont = pm->pointcontents( point, pm->ps->clientNum );

	if ( cont & MASK_WATER ) {
		sample2 = pm->ps->viewheight - MINS_Z;
		sample1 = sample2 / 2;

		pm->watertype = cont;
		pm->waterlevel = 1;
		point[2] = pm->ps->origin[2] + MINS_Z + sample1;
		cont = pm->pointcontents (point, pm->ps->clientNum );
		if ( cont & MASK_WATER ) {
			pm->waterlevel = 2;
			point[2] = pm->ps->origin[2] + MINS_Z + sample2;
			cont = pm->pointcontents (point, pm->ps->clientNum );
			if ( cont & MASK_WATER ){
				pm->waterlevel = 3;
			}
		}
	}

}

/*
==============
PM_CheckDuck

Sets mins, maxs, and pm->ps->viewheight
==============
*/
static void PM_CheckDuck (void)
{
	trace_t	trace;
	qboolean wantsProne;
	qboolean wantsCrouch;

	if ( pm->ps->powerups[PW_INVULNERABILITY] ) {
		if ( pm->ps->pm_flags & PMF_INVULEXPAND ) {
			// invulnerability sphere has a 42 units radius
			VectorSet( pm->mins, -INVUL_RADIUS, -INVUL_RADIUS, -INVUL_RADIUS );
			VectorSet( pm->maxs, INVUL_RADIUS, INVUL_RADIUS, INVUL_RADIUS );
		}
		else {
			VectorSet( pm->mins, -PLAYER_WIDTH, -PLAYER_WIDTH, MINS_Z );
			VectorSet( pm->maxs, PLAYER_WIDTH, PLAYER_WIDTH, 16 );
		}
		pm->ps->pm_flags |= PMF_DUCKED;
		pm->ps->pm_flags &= ~PMF_PRONE;
		pm->ps->viewheight = CROUCH_VIEWHEIGHT;
		return;
	}
	pm->ps->pm_flags &= ~PMF_INVULEXPAND;

	pm->mins[0] = -PLAYER_WIDTH;
	pm->mins[1] = -PLAYER_WIDTH;

	pm->maxs[0] = PLAYER_WIDTH;
	pm->maxs[1] = PLAYER_WIDTH;

	pm->mins[2] = MINS_Z;

	if (pm->ps->pm_type == PM_DEAD)
	{
		pm->ps->pm_flags &= ~PMF_PRONE;
		pm->maxs[2] = DEAD_HEIGHT;
		pm->ps->viewheight = DEAD_VIEWHEIGHT;
		return;
	}

	// explicit stance buttons are sticky in CoD-style controls
	if ( pm->cmd.buttons & BUTTON_PRONE ) {
		pm->ps->pm_flags |= PMF_PRONE;
	} else if ( pm->cmd.buttons & BUTTON_CROUCH ) {
		pm->ps->pm_flags &= ~PMF_PRONE;
	} else if ( pm->cmd.buttons & BUTTON_STAND ) {
		pm->ps->pm_flags &= ~PMF_PRONE;
	}

	// jump/up input exits prone first
	if ( pm->cmd.upmove > 10 ) {
		pm->ps->pm_flags &= ~PMF_PRONE;
	}

	wantsProne = ( pm->ps->pm_flags & PMF_PRONE ) ? qtrue : qfalse;
	wantsCrouch = qfalse;

	if ( wantsProne ) {
		pm->ps->pm_flags |= PMF_DUCKED;
		pm->maxs[2] = PRONE_HEIGHT;
#ifdef STANDALONE
		/* CoD1: smooth viewheight transition to prone (~300ms) */
		{
			int target = PRONE_VIEWHEIGHT;
			int diff = target - pm->ps->viewheight;
			if ( diff ) {
				int step = (int)( (float)abs(diff) * (float)pml.msec / 300.0f + 0.5f );
				if ( step < 1 ) step = 1;
				if ( diff > 0 ) pm->ps->viewheight += step > diff ? diff : step;
				else            pm->ps->viewheight -= step > -diff ? -diff : step;
			}
		}
#else
		pm->ps->viewheight = PRONE_VIEWHEIGHT;
#endif
		return;
	}

	if ( pm->cmd.upmove < 0 || ( pm->cmd.buttons & BUTTON_CROUCH ) ) {
		wantsCrouch = qtrue;
	}

	if ( wantsCrouch ) {
		pm->ps->pm_flags |= PMF_DUCKED;
	} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
		// try to stand up
		pm->maxs[2] = DEFAULT_HEIGHT;
		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, pm->ps->origin, pm->ps->clientNum, pm->tracemask );
		if ( !trace.allsolid ) {
			pm->ps->pm_flags &= ~PMF_DUCKED;
		}
	}

	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		pm->maxs[2] = CROUCH_HEIGHT;
#ifdef STANDALONE
		/* CoD1: smooth viewheight transition (~200ms lerp) */
		{
			int target = CROUCH_VIEWHEIGHT;
			int diff = target - pm->ps->viewheight;
			if ( diff ) {
				int step = (int)( (float)abs(diff) * (float)pml.msec / 200.0f + 0.5f );
				if ( step < 1 ) step = 1;
				if ( diff > 0 ) pm->ps->viewheight += step > diff ? diff : step;
				else            pm->ps->viewheight -= step > -diff ? -diff : step;
			}
		}
#else
		pm->ps->viewheight = CROUCH_VIEWHEIGHT;
#endif
	} else {
		pm->maxs[2] = DEFAULT_HEIGHT;
#ifdef STANDALONE
		{
			int target = DEFAULT_VIEWHEIGHT;
			int diff = target - pm->ps->viewheight;
			if ( diff ) {
				int step = (int)( (float)abs(diff) * (float)pml.msec / 200.0f + 0.5f );
				if ( step < 1 ) step = 1;
				if ( diff > 0 ) pm->ps->viewheight += step > diff ? diff : step;
				else            pm->ps->viewheight -= step > -diff ? -diff : step;
			}
		}
#else
		pm->ps->viewheight = DEFAULT_VIEWHEIGHT;
#endif
	}
}



//===================================================================

#ifdef STANDALONE
/*
===============
PM_UpdateLean

CoD1 lean system: Q/E keys tilt the view and offset the camera laterally.
Reference: GAME_MP_.c PM_UpdateLean (lines 11821-12027)

Lean button bits: 0x10 = left, 0x20 = right (byte 5 of usercmd = BUTTON_LEAN_LEFT/RIGHT)
Lean fraction max: standing=0.25, crouching=0.5
Lean speed: moving = pml.msec/350, not moving = pml.msec/280
Lean offset: 16.0 units lateral, trace with 20.0 margin
===============
*/
static void PM_UpdateLean( void )
{
	int		wantDir = 0;	/* -1 left, 0 center, +1 right */
	float	maxLean, leanRate, target;
	int		stance;

	/* Read lean buttons */
	if ( pm->cmd.buttons & BUTTON_LEAN_LEFT )  wantDir--;
	if ( pm->cmd.buttons & BUTTON_LEAN_RIGHT ) wantDir++;

	/* Can't lean while prone */
	if ( pm->ps->pm_flags & PMF_PRONE ) wantDir = 0;

	/* Can't lean while not on ground (unless already leaning) */
	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE && pm->ps->leanf == 0.0f )
		wantDir = 0;

	/* Stance determines max lean fraction (ref: lines 11921-11926) */
	if ( pm->ps->pm_flags & PMF_DUCKED )
		maxLean = 0.5f;		/* crouched: larger lean */
	else
		maxLean = 0.25f;	/* standing: smaller lean */

	/* Lean rate depends on movement (ref: moving=pml[10]/350, still=pml[10]/280) */
	{
		vec3_t hVel;
		float  speed;
		VectorCopy( pm->ps->velocity, hVel );
		hVel[2] = 0;
		speed = VectorLength( hVel );
		if ( speed > 10.0f )
			leanRate = (float)pml.msec / 350.0f;
		else
			leanRate = (float)pml.msec / 280.0f;
	}

	target = (float)wantDir * maxLean;

	if ( pm->ps->leanf < target ) {
		pm->ps->leanf += leanRate;
		if ( pm->ps->leanf > target ) pm->ps->leanf = target;
	} else if ( pm->ps->leanf > target ) {
		pm->ps->leanf -= leanRate;
		if ( pm->ps->leanf < target ) pm->ps->leanf = target;
	}

	/* Clamp */
	if ( pm->ps->leanf > maxLean )  pm->ps->leanf = maxLean;
	if ( pm->ps->leanf < -maxLean ) pm->ps->leanf = -maxLean;
}

/*
===============
PM_UpdatePronePitch

CoD1 prone view restrictions: when prone, the player's yaw turning is
capped and pitch is restricted relative to the body direction.
Reference: GAME_MP_.c PM_UpdatePronePitch (lines 12349-12450)
  - Yaw cap: bg_prone_yawcap (typically ±85 degrees from body dir)
  - Yaw rate: pml.msec * 55 / 1000 max delta per frame
  - Pitch bounds: tighter than standing
===============
*/
#define PRONE_YAW_CAP		85.0f	/* max yaw offset from body direction */
#define PRONE_PITCH_MIN		-45.0f	/* can't look up too far while prone */
#define PRONE_PITCH_MAX		45.0f	/* can't look down too far while prone */
#define PRONE_YAW_RATE		55.0f	/* max yaw degrees per second body turn */

static void PM_UpdatePronePitch( void )
{
	float	yawDelta, maxDelta;

	if ( !( pm->ps->pm_flags & PMF_PRONE ) )
		return;

	/* Restrict pitch while prone (ref: PM_UpdatePronePitch pitch clamp) */
	if ( pm->ps->viewangles[PITCH] > PRONE_PITCH_MAX )
		pm->ps->viewangles[PITCH] = PRONE_PITCH_MAX;
	if ( pm->ps->viewangles[PITCH] < PRONE_PITCH_MIN )
		pm->ps->viewangles[PITCH] = PRONE_PITCH_MIN;

	/* Restrict yaw turn rate while prone (ref: pml[9]*55 max delta, line 12234) */
	maxDelta = PRONE_YAW_RATE * (float)pml.msec / 1000.0f;
	yawDelta = AngleSubtract( pm->ps->viewangles[YAW],
	                          pm->ps->movementDir * 45.0f ); /* rough body dir */
	(void)yawDelta; /* yaw cap would need body direction tracking */
	(void)maxDelta;

	/* Note: full body-direction tracking requires a separate body yaw field
	 * in playerState (ps->proneBodyYaw). For now, prone pitch restriction
	 * is the primary gameplay effect. */
}

/*
===============
PM_LandingSlowdown

CoD1 jump landing mechanic: when a player lands from a jump/fall, their
speed is temporarily reduced. Discourages bunny hopping.

Reference: GAME_MP_.c lines 9460-9500
  stunTime = 35 * fallHeight + 500  (capped at 2000ms)
  speedMult:
    stunTime <= 500:  0.5
    500 < stunTime < 1500:  0.5 - (stunTime-500)/1000 * 0.3
    stunTime >= 1500: 0.2
  High falls (vel < -600): 0.67 velocity multiplier applied directly
===============
*/
static void PM_LandingSlowdown( void )
{
	/* Detect landing: was in air, now on ground */
	if ( pml.previous_velocity[2] < -200.0f &&
	     pm->ps->groundEntityNum != ENTITYNUM_NONE &&
	     pm->ps->landTime < pm->cmd.serverTime - 100 ) {
		float fallSpeed = -pml.previous_velocity[2];
		int   stunTime;

		/* High fall: direct velocity penalty (ref line 9461-9464) */
		if ( fallSpeed > 600.0f ) {
			pm->ps->velocity[0] *= 0.67f;
			pm->ps->velocity[1] *= 0.67f;
		}

		/* Stun time formula: 35 * height_equiv + 500, cap 2000 (ref line 9468-9469) */
		stunTime = (int)( 35.0f * ( fallSpeed / 20.0f ) ) + 500;
		if ( stunTime > 2000 ) stunTime = 2000;

		pm->ps->landTime = pm->cmd.serverTime;

		/* Speed multiplier (ref lines 9474-9480) */
		if ( stunTime <= 500 ) {
			pm->ps->landSlowdown = 0.5f;
		} else if ( stunTime < 1500 ) {
			pm->ps->landSlowdown = 0.5f - (float)( stunTime - 500 ) / 1000.0f * 0.3f;
		} else {
			pm->ps->landSlowdown = 0.2f;
		}
	}

	/* Recover from landing slowdown */
	if ( pm->ps->landSlowdown < 1.0f ) {
		int elapsed = pm->cmd.serverTime - pm->ps->landTime;
		/* Stun recovers over its duration, linearly back to 1.0 */
		if ( elapsed > 0 ) {
			float recovery = (float)elapsed / 500.0f;
			float target = pm->ps->landSlowdown + recovery;
			if ( target > 1.0f ) target = 1.0f;
			pm->ps->landSlowdown = target;
		}
	}
}
#endif /* STANDALONE */

/*
===============
PM_Footsteps
===============
*/
static void PM_Footsteps( void ) {
	float		bobmove;
	int			old;
	qboolean	footstep;

	//
	// calculate speed and cycle to be used for
	// all cyclic walking effects
	//
	pm->xyspeed = sqrt( pm->ps->velocity[0] * pm->ps->velocity[0]
		+  pm->ps->velocity[1] * pm->ps->velocity[1] );

	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {

		if ( pm->ps->powerups[PW_INVULNERABILITY] ) {
			PM_ContinueLegsAnim( LEGS_IDLECR );
		}
		// airborne leaves position in cycle intact, but doesn't advance
		if ( pm->waterlevel > 1 ) {
			PM_ContinueLegsAnim( LEGS_SWIM );
		}
		return;
	}

	// if not trying to move
	if ( !pm->cmd.forwardmove && !pm->cmd.rightmove ) {
		if (  pm->xyspeed < 5 ) {
			pm->ps->bobCycle = 0;	// start at beginning of cycle again
			if ( pm->ps->pm_flags & PMF_DUCKED ) {
				PM_ContinueLegsAnim( LEGS_IDLECR );
			} else {
				PM_ContinueLegsAnim( LEGS_IDLE );
			}
		}
		return;
	}
	

	footstep = qfalse;

	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		bobmove = 0.5;	// ducked characters bob much faster
		if ( pm->ps->pm_flags & PMF_BACKWARDS_RUN ) {
			PM_ContinueLegsAnim( LEGS_BACKCR );
		}
		else {
			PM_ContinueLegsAnim( LEGS_WALKCR );
		}
		// ducked characters never play footsteps
	/*
	} else 	if ( pm->ps->pm_flags & PMF_BACKWARDS_RUN ) {
		if ( !( pm->cmd.buttons & BUTTON_WALKING ) ) {
			bobmove = 0.4;	// faster speeds bob faster
			footstep = qtrue;
		} else {
			bobmove = 0.3;
		}
		PM_ContinueLegsAnim( LEGS_BACK );
	*/
	} else {
		if ( !( pm->cmd.buttons & BUTTON_WALKING ) ) {
			bobmove = 0.4f;	// faster speeds bob faster
			if ( pm->ps->pm_flags & PMF_BACKWARDS_RUN ) {
				PM_ContinueLegsAnim( LEGS_BACK );
			}
			else {
				PM_ContinueLegsAnim( LEGS_RUN );
			}
			footstep = qtrue;
		} else {
			bobmove = 0.3f;	// walking bobs slow
			if ( pm->ps->pm_flags & PMF_BACKWARDS_RUN ) {
				PM_ContinueLegsAnim( LEGS_BACKWALK );
			}
			else {
				PM_ContinueLegsAnim( LEGS_WALK );
			}
		}
	}

	// check for footstep / splash sounds
	old = pm->ps->bobCycle;
	pm->ps->bobCycle = (int)( old + bobmove * pml.msec ) & 255;

	// if we just crossed a cycle boundary, play an appropriate footstep event
	if ( ( ( old + 64 ) ^ ( pm->ps->bobCycle + 64 ) ) & 128 ) {
		if ( pm->waterlevel == 0 ) {
			// on ground will only play sounds if running
			if ( footstep && !pm->noFootsteps ) {
				PM_AddEvent( PM_FootstepForSurface() );
			}
		} else if ( pm->waterlevel == 1 ) {
			// splashing
			PM_AddEvent( EV_FOOTSPLASH );
		} else if ( pm->waterlevel == 2 ) {
			// wading / swimming at surface
			PM_AddEvent( EV_SWIM );
		} else if ( pm->waterlevel == 3 ) {
			// no sound when completely underwater

		}
	}
}

/*
==============
PM_WaterEvents

Generate sound events for entering and leaving water
==============
*/
static void PM_WaterEvents( void ) {		// FIXME?
	//
	// if just entered a water volume, play a sound
	//
	if (!pml.previous_waterlevel && pm->waterlevel) {
		PM_AddEvent( EV_WATER_TOUCH );
	}

	//
	// if just completely exited a water volume, play a sound
	//
	if (pml.previous_waterlevel && !pm->waterlevel) {
		PM_AddEvent( EV_WATER_LEAVE );
	}

	//
	// check for head just going under water
	//
	if (pml.previous_waterlevel != 3 && pm->waterlevel == 3) {
		PM_AddEvent( EV_WATER_UNDER );
	}

	//
	// check for head just coming out of water
	//
	if (pml.previous_waterlevel == 3 && pm->waterlevel != 3) {
		PM_AddEvent( EV_WATER_CLEAR );
	}
}


/*
===============
PM_BeginWeaponChange
===============
*/
static void PM_BeginWeaponChange( int weapon ) {
	if ( weapon <= WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		return;
	}

	if ( !( pm->ps->stats[STAT_WEAPONS] & ( 1 << weapon ) ) ) {
		return;
	}
	
	if ( pm->ps->weaponstate == WEAPON_DROPPING ) {
		return;
	}

	PM_AddEvent( EV_CHANGE_WEAPON );
	pm->ps->weaponstate = WEAPON_DROPPING;
	pm->ps->weaponTime += 200;
	PM_StartTorsoAnim( TORSO_DROP );
}


/*
===============
PM_FinishWeaponChange
===============
*/
static void PM_FinishWeaponChange( void ) {
	int		weapon;

	weapon = pm->cmd.weapon;
	if ( weapon < WP_NONE || weapon >= WP_NUM_WEAPONS ) {
		weapon = WP_NONE;
	}

	if ( !( pm->ps->stats[STAT_WEAPONS] & ( 1 << weapon ) ) ) {
		weapon = WP_NONE;
	}

	pm->ps->weapon = weapon;
	pm->ps->weaponstate = WEAPON_RAISING;
	pm->ps->weaponTime += 250;
	PM_StartTorsoAnim( TORSO_RAISE );
}


/*
==============
PM_TorsoAnimation

==============
*/
static void PM_TorsoAnimation( void ) {
	if ( pm->ps->weaponstate == WEAPON_READY ) {
		if ( pm->ps->weapon == WP_GAUNTLET ) {
			PM_ContinueTorsoAnim( TORSO_STAND2 );
		} else {
			PM_ContinueTorsoAnim( TORSO_STAND );
		}
		return;
	}
}


/*
==============
PM_Weapon

Generates weapon events and modifes the weapon counter
==============
*/
static void PM_Weapon( void ) {
	int		addTime;

	// don't allow attack until all buttons are up
	if ( pm->ps->pm_flags & PMF_RESPAWNED ) {
		return;
	}

	// ignore if spectator
	if ( pm->ps->persistant[PERS_TEAM] == TEAM_SPECTATOR ) {
		return;
	}

	// check for dead player
	if ( pm->ps->stats[STAT_HEALTH] <= 0 ) {
		pm->ps->weapon = WP_NONE;
		return;
	}

	// check for item using
	if ( pm->cmd.buttons & BUTTON_USE_HOLDABLE ) {
		if ( ! ( pm->ps->pm_flags & PMF_USE_ITEM_HELD ) ) {
			if ( bg_itemlist[pm->ps->stats[STAT_HOLDABLE_ITEM]].giTag == HI_MEDKIT
				&& pm->ps->stats[STAT_HEALTH] >= (pm->ps->stats[STAT_MAX_HEALTH] + 25) ) {
				// don't use medkit if at max health
			} else {
				pm->ps->pm_flags |= PMF_USE_ITEM_HELD;
				PM_AddEvent( EV_USE_ITEM0 + bg_itemlist[pm->ps->stats[STAT_HOLDABLE_ITEM]].giTag );
				pm->ps->stats[STAT_HOLDABLE_ITEM] = 0;
			}
			return;
		}
	} else {
		pm->ps->pm_flags &= ~PMF_USE_ITEM_HELD;
	}


	// make weapon function
	if ( pm->ps->weaponTime > 0 ) {
		pm->ps->weaponTime -= pml.msec;
	}

	// check for weapon change
	// can't change if weapon is firing, but can change
	// again if lowering or raising
	if ( pm->ps->weaponTime <= 0 || pm->ps->weaponstate != WEAPON_FIRING ) {
		if ( pm->ps->weapon != pm->cmd.weapon ) {
			PM_BeginWeaponChange( pm->cmd.weapon );
		}
	}

	if ( pm->ps->weaponTime > 0 ) {
		return;
	}

	// change weapon if time
	if ( pm->ps->weaponstate == WEAPON_DROPPING ) {
		PM_FinishWeaponChange();
		return;
	}

	if ( pm->ps->weaponstate == WEAPON_RAISING ) {
		pm->ps->weaponstate = WEAPON_READY;
		if ( pm->ps->weapon == WP_GAUNTLET ) {
			PM_StartTorsoAnim( TORSO_STAND2 );
		} else {
			PM_StartTorsoAnim( TORSO_STAND );
		}
		return;
	}

	// check for fire
	if ( ! (pm->cmd.buttons & BUTTON_ATTACK) ) {
		pm->ps->weaponTime = 0;
		pm->ps->weaponstate = WEAPON_READY;
		return;
	}

	// start the animation even if out of ammo
#ifndef STANDALONE
	if ( pm->ps->weapon == WP_GAUNTLET ) {
		// the guantlet only "fires" when it actually hits something
		if ( !pm->gauntletHit ) {
			pm->ps->weaponTime = 0;
			pm->ps->weaponstate = WEAPON_READY;
			return;
		}
		PM_StartTorsoAnim( TORSO_ATTACK2 );
	} else
#endif
	{
		PM_StartTorsoAnim( TORSO_ATTACK );
	}

	pm->ps->weaponstate = WEAPON_FIRING;

#ifdef STANDALONE
	/* CoD1: weapon firing is handled by G_FireWeapon in g_weapon_cod1.c.
	 * Skip the Q3 ammo decrement and EV_FIRE_WEAPON event entirely —
	 * otherwise both systems fire simultaneously (double-fire bug). */
	addTime = 100;	// keep weaponTime ticking so PM_Weapon doesn't re-enter
#else
	// check for out of ammo
	if ( ! pm->ps->ammo[ pm->ps->weapon ] ) {
		PM_AddEvent( EV_NOAMMO );
		pm->ps->weaponTime += 500;
		return;
	}

	// take an ammo away if not infinite
	if ( pm->ps->ammo[ pm->ps->weapon ] != -1 ) {
		pm->ps->ammo[ pm->ps->weapon ]--;
	}

	// fire weapon
	PM_AddEvent( EV_FIRE_WEAPON );
#endif

#ifndef STANDALONE
	switch( pm->ps->weapon ) {
	default:
	case WP_GAUNTLET:
		addTime = 400;
		break;
	case WP_LIGHTNING:
		addTime = 50;
		break;
	case WP_SHOTGUN:
		addTime = 1000;
		break;
	case WP_MACHINEGUN:
		addTime = 100;
		break;
	case WP_GRENADE_LAUNCHER:
		addTime = 800;
		break;
	case WP_ROCKET_LAUNCHER:
		addTime = 800;
		break;
	case WP_PLASMAGUN:
		addTime = 100;
		break;
	case WP_RAILGUN:
		addTime = 1500;
		break;
	case WP_BFG:
		addTime = 200;
		break;
	case WP_GRAPPLING_HOOK:
		addTime = 400;
		break;
#ifdef MISSIONPACK
	case WP_NAILGUN:
		addTime = 1000;
		break;
	case WP_PROX_LAUNCHER:
		addTime = 800;
		break;
	case WP_CHAINGUN:
		addTime = 30;
		break;
#endif
	}
#endif

#ifdef MISSIONPACK
	if( bg_itemlist[pm->ps->stats[STAT_PERSISTANT_POWERUP]].giTag == PW_SCOUT ) {
		addTime /= 1.5;
	}
	else
	if( bg_itemlist[pm->ps->stats[STAT_PERSISTANT_POWERUP]].giTag == PW_AMMOREGEN ) {
		addTime /= 1.3;
	}
	else
#endif
	if ( pm->ps->powerups[PW_HASTE] ) {
		addTime /= 1.3;
	}

	pm->ps->weaponTime += addTime;
}

/*
================
PM_Animate
================
*/

static void PM_Animate( void ) {
	if ( pm->cmd.buttons & BUTTON_GESTURE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GESTURE );
			pm->ps->torsoTimer = TIMER_GESTURE;
			PM_AddEvent( EV_TAUNT );
		}
#ifdef MISSIONPACK
	} else if ( pm->cmd.buttons & BUTTON_GETFLAG ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GETFLAG );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_GUARDBASE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_GUARDBASE );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_PATROL ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_PATROL );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_FOLLOWME ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_FOLLOWME );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_AFFIRMATIVE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_AFFIRMATIVE);
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
	} else if ( pm->cmd.buttons & BUTTON_NEGATIVE ) {
		if ( pm->ps->torsoTimer == 0 ) {
			PM_StartTorsoAnim( TORSO_NEGATIVE );
			pm->ps->torsoTimer = 600;	//TIMER_GESTURE;
		}
#endif
	}
}


/*
================
PM_DropTimers
================
*/
static void PM_DropTimers( void ) {
	// drop misc timing counter
	if ( pm->ps->pm_time ) {
		if ( pml.msec >= pm->ps->pm_time ) {
			pm->ps->pm_flags &= ~PMF_ALL_TIMES;
			pm->ps->pm_time = 0;
		} else {
			pm->ps->pm_time -= pml.msec;
		}
	}

	// drop animation counter
	if ( pm->ps->legsTimer > 0 ) {
		pm->ps->legsTimer -= pml.msec;
		if ( pm->ps->legsTimer < 0 ) {
			pm->ps->legsTimer = 0;
		}
	}

	if ( pm->ps->torsoTimer > 0 ) {
		pm->ps->torsoTimer -= pml.msec;
		if ( pm->ps->torsoTimer < 0 ) {
			pm->ps->torsoTimer = 0;
		}
	}
}

/*
================
PM_UpdateViewAngles

This can be used as another entry point when only the viewangles
are being updated instead of a full move
================
*/
void PM_UpdateViewAngles( playerState_t *ps, const usercmd_t *cmd ) {
	short		temp;
	int		i;

	if ( ps->pm_type == PM_INTERMISSION || ps->pm_type == PM_SPINTERMISSION) {
		return;		// no view changes at all
	}

	if ( ps->pm_type != PM_SPECTATOR && ps->stats[STAT_HEALTH] <= 0 ) {
		return;		// no view changes at all
	}

	// circularly clamp the angles with deltas
	for (i=0 ; i<3 ; i++) {
		temp = cmd->angles[i] + ps->delta_angles[i];
		if ( i == PITCH ) {
			// don't let the player look up or down more than 90 degrees
			if ( temp > 16000 ) {
				ps->delta_angles[i] = 16000 - cmd->angles[i];
				temp = 16000;
			} else if ( temp < -16000 ) {
				ps->delta_angles[i] = -16000 - cmd->angles[i];
				temp = -16000;
			}
		}
		ps->viewangles[i] = SHORT2ANGLE(temp);
	}

}


/*
================
PmoveSingle

================
*/
void trap_SnapVector( float *v );

void PmoveSingle (pmove_t *pmove) {
	pm = pmove;

	// this counter lets us debug movement problems with a journal
	// by setting a conditional breakpoint fot the previous frame
	c_pmove++;

	// clear results
	pm->numtouch = 0;
	pm->watertype = 0;
	pm->waterlevel = 0;

	if ( pm->ps->stats[STAT_HEALTH] <= 0 ) {
		pm->tracemask &= ~CONTENTS_BODY;	// corpses can fly through bodies
	}

	// make sure walking button is clear if they are running, to avoid
	// proxy no-footsteps cheats
	if ( abs( pm->cmd.forwardmove ) > 64 || abs( pm->cmd.rightmove ) > 64 ) {
		pm->cmd.buttons &= ~BUTTON_WALKING;
	}

	// set the talk balloon flag
	if ( pm->cmd.buttons & BUTTON_TALK ) {
		pm->ps->eFlags |= EF_TALK;
	} else {
		pm->ps->eFlags &= ~EF_TALK;
	}

	// set the firing flag for continuous beam weapons
	if ( !(pm->ps->pm_flags & PMF_RESPAWNED) && pm->ps->pm_type != PM_INTERMISSION && pm->ps->pm_type != PM_NOCLIP
		&& ( pm->cmd.buttons & BUTTON_ATTACK ) && pm->ps->ammo[ pm->ps->weapon ] ) {
		pm->ps->eFlags |= EF_FIRING;
	} else {
		pm->ps->eFlags &= ~EF_FIRING;
	}

	// clear the respawned flag if attack and use are cleared
	if ( pm->ps->stats[STAT_HEALTH] > 0 && 
		!( pm->cmd.buttons & (BUTTON_ATTACK | BUTTON_USE_HOLDABLE) ) ) {
		pm->ps->pm_flags &= ~PMF_RESPAWNED;
	}

	// if talk button is down, dissallow all other input
	// this is to prevent any possible intercept proxy from
	// adding fake talk balloons
	if ( pmove->cmd.buttons & BUTTON_TALK ) {
		// keep the talk button set tho for when the cmd.serverTime > 66 msec
		// and the same cmd is used multiple times in Pmove
		pmove->cmd.buttons = BUTTON_TALK;
		pmove->cmd.forwardmove = 0;
		pmove->cmd.rightmove = 0;
		pmove->cmd.upmove = 0;
	}

	// clear all pmove local vars
	memset (&pml, 0, sizeof(pml));

	// determine the time
	pml.msec = pmove->cmd.serverTime - pm->ps->commandTime;
	if ( pml.msec < 1 ) {
		pml.msec = 1;
	} else if ( pml.msec > 200 ) {
		pml.msec = 200;
	}
	pm->ps->commandTime = pmove->cmd.serverTime;

	// save old org in case we get stuck
	VectorCopy (pm->ps->origin, pml.previous_origin);

	// save old velocity for crashlanding
	VectorCopy (pm->ps->velocity, pml.previous_velocity);

	pml.frametime = pml.msec * 0.001;

	// update the viewangles
	PM_UpdateViewAngles( pm->ps, &pm->cmd );

	AngleVectors (pm->ps->viewangles, pml.forward, pml.right, pml.up);

	if ( pm->cmd.upmove < 10 ) {
		// not holding jump
		pm->ps->pm_flags &= ~PMF_JUMP_HELD;
	}

	// decide if backpedaling animations should be used
	if ( pm->cmd.forwardmove < 0 ) {
		pm->ps->pm_flags |= PMF_BACKWARDS_RUN;
	} else if ( pm->cmd.forwardmove > 0 || ( pm->cmd.forwardmove == 0 && pm->cmd.rightmove ) ) {
		pm->ps->pm_flags &= ~PMF_BACKWARDS_RUN;
	}

	if ( pm->ps->pm_type >= PM_DEAD ) {
		pm->cmd.forwardmove = 0;
		pm->cmd.rightmove = 0;
		pm->cmd.upmove = 0;
	}

	if ( pm->ps->pm_type == PM_SPECTATOR ) {
		PM_CheckDuck ();
		PM_FlyMove ();
		PM_DropTimers ();
		return;
	}

	if ( pm->ps->pm_type == PM_NOCLIP ) {
		PM_NoclipMove ();
		PM_DropTimers ();
		return;
	}

#ifdef STANDALONE
	if ( pm->ps->pm_type == PM_UFO ) {
		PM_UFOMove();
		PM_DropTimers();
		return;
	}
#endif

	if (pm->ps->pm_type == PM_FREEZE) {
		return;		// no movement at all
	}

	if ( pm->ps->pm_type == PM_INTERMISSION || pm->ps->pm_type == PM_SPINTERMISSION) {
		return;		// no movement at all
	}

	// set watertype, and waterlevel
	PM_SetWaterLevel();
	pml.previous_waterlevel = pmove->waterlevel;

	// set mins, maxs, and viewheight
	PM_CheckDuck ();

#ifdef STANDALONE
	// CoD1: lean, prone pitch restriction
	PM_UpdateLean();
	PM_UpdatePronePitch();
#endif

	// ladder movement
	PM_CheckLadder();

	// set groundentity
	PM_GroundTrace();

#ifdef STANDALONE
	// CoD1: landing slowdown — MUST be after PM_GroundTrace so
	// groundEntityNum is current-frame when detecting landing
	PM_LandingSlowdown();
#endif

	if ( pm->ps->pm_type == PM_DEAD ) {
		PM_DeadMove ();
	}

	PM_DropTimers();

#ifdef MISSIONPACK
	if ( pm->ps->powerups[PW_INVULNERABILITY] ) {
		PM_InvulnerabilityMove();
	} else
#endif
#ifndef STANDALONE
	if ( pm->ps->powerups[PW_FLIGHT] ) {
		// flight powerup doesn't allow jump and has different friction
		PM_FlyMove();
	} else if (pm->ps->pm_flags & PMF_GRAPPLE_PULL) {
		PM_GrappleMove();
		// We can wiggle a bit
		PM_AirMove();
	} else
#endif
	if (pm->ps->pm_flags & PMF_ON_LADDER) {
		PM_LadderMove();
	} else if (pm->ps->pm_flags & PMF_TIME_WATERJUMP) {
		PM_WaterJumpMove();
	} else if ( pm->waterlevel > 1 ) {
		// swimming
		PM_WaterMove();
	} else if ( pml.walking ) {
		// walking on ground
		PM_WalkMove();
	} else {
		// airborne
		PM_AirMove();
	}

	PM_Animate();

	// set groundentity, watertype, and waterlevel
	PM_GroundTrace();
	PM_SetWaterLevel();

	// weapons
	PM_Weapon();

	// torso animation
	PM_TorsoAnimation();

	// footstep events / legs animations
	PM_Footsteps();

	// entering / leaving water splashes
	PM_WaterEvents();

#ifdef STANDALONE
	// CoD1: clamp velocity when it exceeds 2x the actual displacement velocity
	// This prevents retaining impossibly high velocity after hitting walls/obstacles
	{
		vec3_t	displacement;
		float	vel_sq, disp_vel_sq;

		VectorSubtract( pm->ps->origin, pml.previous_origin, displacement );
		vel_sq = VectorLengthSquared( pm->ps->velocity );
		disp_vel_sq = VectorLengthSquared( displacement ) / ( pml.frametime * pml.frametime );
		if ( vel_sq * 0.25f > disp_vel_sq ) {
			float inv_frametime = 1.0f / pml.frametime;
			pm->ps->velocity[0] = displacement[0] * inv_frametime;
			pm->ps->velocity[1] = displacement[1] * inv_frametime;
			pm->ps->velocity[2] = displacement[2] * inv_frametime;
		}
	}
#endif

	// snap some parts of playerstate to save network bandwidth
	trap_SnapVector( pm->ps->velocity );
}


/*
================
Pmove

Can be called by either the server or the client
================
*/
void Pmove (pmove_t *pmove) {
	int			finalTime;

	finalTime = pmove->cmd.serverTime;

	if ( finalTime < pmove->ps->commandTime ) {
		return;	// should not happen
	}

	if ( finalTime > pmove->ps->commandTime + 1000 ) {
		pmove->ps->commandTime = finalTime - 1000;
	}

	pmove->ps->pmove_framecount = (pmove->ps->pmove_framecount+1) & ((1<<PS_PMOVEFRAMECOUNTBITS)-1);

	// chop the move up if it is too long, to prevent framerate
	// dependent behavior
	while ( pmove->ps->commandTime != finalTime ) {
		int		msec;

		msec = finalTime - pmove->ps->commandTime;

		if ( pmove->pmove_fixed ) {
			if ( msec > pmove->pmove_msec ) {
				msec = pmove->pmove_msec;
			}
		}
		else {
			if ( msec > 66 ) {
				msec = 66;
			}
		}
		pmove->cmd.serverTime = pmove->ps->commandTime + msec;
		PmoveSingle( pmove );

		if ( pmove->ps->pm_flags & PMF_JUMP_HELD ) {
			pmove->cmd.upmove = 20;
		}
	}

	//PM_CheckStuck();

}
