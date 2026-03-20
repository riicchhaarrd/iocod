/*
===========================================================================
GSC scripting integration for the game module.

In STANDALONE builds, the GSC library is compiled in and the functions
below are real implementations.  In non-standalone builds every call
expands to a no-op so no GSC code is ever pulled into the game module.
===========================================================================
*/
#pragma once

#ifdef STANDALONE

void G_Scr_Init( void );
void G_Scr_Shutdown( void );
void G_Scr_Frame( float dt );
qboolean G_Scr_IsActive( void );

/* Player lifecycle callbacks - mirror CoD's CodeCallback_* convention */
void G_Scr_PlayerConnect( int clientNum );
void G_Scr_PlayerBegin( int clientNum );
void G_Scr_PlayerMenuResponse( int clientNum, const char *menu, const char *response );
void G_Scr_PlayerDisconnect( int clientNum );
void G_Scr_PlayerSpawn( int clientNum );
void G_Scr_PlayerKilled( int clientNum, gentity_t *inflictor, gentity_t *attacker,
                         int damage, int mod, const char *weapon, vec3_t dir,
                         int hitLoc );

/* Called on damage so scripts can handle CodeCallback_PlayerDamage */
void G_Scr_PlayerDamage( int clientNum, gentity_t *inflictor, gentity_t *attacker,
                         int damage, int dflags, int mod, const char *weapon,
                         vec3_t point, vec3_t dir, int hitLoc );

/* Entity lifecycle hooks so script object handles stay stable */
void G_Scr_EntitySpawned( gentity_t *ent );
void G_Scr_EntityFreed( gentity_t *ent );

/* Set arbitrary map spawn vars on the entity's GSC object after spawning.
   Call after G_CallSpawn() so scripts can access e.g. entity.script_gameobjectname. */
void G_Scr_SetEntitySpawnVars( gentity_t *ent );

#else /* !STANDALONE */

#define G_Scr_Init()                  ((void)0)
#define G_Scr_Shutdown()              ((void)0)
#define G_Scr_Frame(dt)               ((void)0)
#define G_Scr_IsActive()              (qfalse)
#define G_Scr_PlayerConnect(n)        ((void)0)
#define G_Scr_PlayerBegin(n)          ((void)0)
#define G_Scr_PlayerMenuResponse(a,b,c) ((void)0)
#define G_Scr_PlayerDisconnect(n)     ((void)0)
#define G_Scr_PlayerSpawn(n)          ((void)0)
#define G_Scr_PlayerKilled(a,b,c,d,e,f,g,h)     ((void)0)
#define G_Scr_PlayerDamage(a,b,c,d,e,f,g,h,i,j) ((void)0)
#define G_Scr_EntitySpawned(e)        ((void)0)
#define G_Scr_EntityFreed(e)          ((void)0)
#define G_Scr_SetEntitySpawnVars(e)   ((void)0)

#endif /* STANDALONE */
