/*
===========================================================================
g_scr.c  —  GSC scripting integration for the game module.

Architecture
------------
One gsc_Context is created per map load (G_Scr_Init) and destroyed on
map unload (G_Scr_Shutdown).  All active script threads are advanced once
per game frame (G_Scr_Frame).

Script loading (CoD1 / CoD2 style):
  maps/mp/<mapname>.gsc             — map-specific script (optional)
  maps/mp/gametypes/_callbacksetup.gsc — defines CodeCallback_* hooks

Entity objects
--------------
Each connected client gets a tagged GSC object "#entity" that shares a
common proxy object.  The proxy's __call field is an object containing
native functions registered as method names.  When a script calls
  self getName()
the GSC VM looks up "getname" on the proxy and calls the C function.
The C function retrieves the gentity_t* from the object's userdata.

Globals set in the GSC namespace:
  level   — tagged object holding level-wide state
  game    — tagged object (CoD2 compat placeholder)
  anim    — tagged object (CoD2 compat placeholder)
  self    — swapped to the relevant entity before each callback

Player object globals: "player_N" (N = clientNum, 0-based).
===========================================================================
*/

#ifdef STANDALONE

#include "g_local.h"
#include "g_scr.h"
#include "g_hitloc.h"
#include "../qcommon/bg_weapon_cod1.h"
#include "../thirdparty/gsc/include/gsc.h"

extern char *modNames[];

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
   Module-level state
   ========================================================================= */

static gsc_Context *g_scrCtx;
static qboolean     g_scrActive;

/* Durable references to proxy/player objects via the GSC ref registry.
   The VM tracks ownership — objects stay alive as long as refs are held. */
static gsc_Ref      g_scrEntityProxyRef  = GSC_NOREF;
static gsc_Ref      g_scrPlayerProxyRef  = GSC_NOREF;
static gsc_Ref      g_scrHudElemProxyRef = GSC_NOREF;

/* Per-client ref handle into the GSC registry (GSC_NOREF = no object). */
static gsc_Ref      g_scrPlayerRefs[ MAX_CLIENTS ];
static qboolean     g_scrEntityObjAlive[ MAX_GENTITIES ];

#define G_SCR_MAX_HUDELEMS 128

typedef enum {
    G_SCR_HUD_SCOPE_ALL,
    G_SCR_HUD_SCOPE_CLIENT,
    G_SCR_HUD_SCOPE_TEAM
} G_ScrHudScope_t;

typedef enum {
    G_SCR_HUD_TIMER_NONE = 0,
    G_SCR_HUD_TIMER_DOWN,
    G_SCR_HUD_TIMER_UP,
    G_SCR_HUD_TIMER_TENTHS_DOWN
} G_ScrHudTimerMode_t;

typedef struct {
    qboolean      inuse;
    int           id;      /* 1..G_SCR_MAX_HUDELEMS */
    G_ScrHudScope_t scope;
    int           owner;   /* clientNum for CLIENT, team_t for TEAM */
    float         x;
    float         y;
    float         scale;
    float         color[ 4 ];
    int           width;
    int           height;
    char          text[ 128 ];
    char          shader[ MAX_QPATH ];
    G_ScrHudTimerMode_t timerMode;
    int           timerBaseMs;
    int           timerStartMs;
    int           timerLastValue;
} G_ScrHudElem_t;

static G_ScrHudElem_t g_scrHudElems[ G_SCR_MAX_HUDELEMS ];

#define G_SCR_MAX_ATTACHMENTS 16

typedef struct {
    qboolean inuse;
    char     model[ MAX_QPATH ];
    char     tag[ 64 ];
    int      ignoreCollision;
} G_ScrAttachSlot_t;

typedef struct {
    int              count;
    G_ScrAttachSlot_t slot[ G_SCR_MAX_ATTACHMENTS ];
} G_ScrAttachState_t;

static G_ScrAttachState_t g_scrAttachStates[ MAX_GENTITIES ];
static char               g_scrViewModels[ MAX_GENTITIES ][ MAX_QPATH ];

#define G_SCR_MAX_FX 256
static char g_scrFxNames[ G_SCR_MAX_FX ][ MAX_QPATH ];
static int  g_scrFxCount;

#define G_SCR_MAX_OBJECTIVES 32
typedef struct {
    qboolean inuse;
    vec3_t   origin;
    char     icon[ MAX_QPATH ];
    int      team;
    int      entNum;
} G_ScrObjective_t;

static G_ScrObjective_t g_scrObjectives[ G_SCR_MAX_OBJECTIVES ];

/* The GSC file namespace (path without .gsc) where CodeCallback_* live.   */
static char         g_scrCallbackFile[ MAX_QPATH ];

/* Deferred auto-menu-responses: openMenu queues here, G_Scr_Frame fires them.
   This ensures the response notification arrives after the script thread
   reaches its waittill("menuresponse") suspension point. */
#define G_SCR_MAX_DEFERRED_MENUS 8
typedef struct {
    int  clientNum;
    char menu[ MAX_QPATH ];
    char response[ 64 ];
} G_ScrDeferredMenu_t;

static G_ScrDeferredMenu_t g_scrDeferredMenus[ G_SCR_MAX_DEFERRED_MENUS ];
static int                 g_scrDeferredMenuCount;

/* Local forward declaration from g_spawn.c (not exposed in g_local.h). */
qboolean G_CallSpawn( gentity_t *ent );
static void G_Scr_CreatePlayerObj( int clientNum );
static void G_Scr_NotifyPlayer( int clientNum, const char *eventName, int numArgs );

/* =========================================================================
   Source-buffer tracking — freed after gsc_link completes
   ========================================================================= */

typedef struct ScrSrc_s { struct ScrSrc_s *next; } ScrSrc_t;
static ScrSrc_t *g_scrSources;

static void G_Scr_FreeSources( void )
{
    ScrSrc_t *s, *next;
    for ( s = g_scrSources; s; s = next ) {
        next = s->next;
        free( s );
    }
    g_scrSources = NULL;
}

/* =========================================================================
   GSC context callbacks
   ========================================================================= */

static void *G_Scr_AllocMem( void *ctx, int size )
{
    (void)ctx;
    return calloc( 1, (size_t)size );
}

static void G_Scr_FreeMem( void *ctx, void *ptr )
{
    (void)ctx;
    free( ptr );
}

static void G_Scr_ResetRefs( void )
{
    int i;
    g_scrEntityProxyRef  = GSC_NOREF;
    g_scrPlayerProxyRef  = GSC_NOREF;
    g_scrHudElemProxyRef = GSC_NOREF;
    for ( i = 0; i < MAX_CLIENTS; i++ ) {
        g_scrPlayerRefs[ i ] = GSC_NOREF;
    }
}

static void G_Scr_NormalizePathSlashes( char *path )
{
    int i;

    for ( i = 0; path[i]; i++ ) {
        if ( path[i] == '\\' ) {
            path[i] = '/';
        }
    }
}

static void G_Scr_NormalizeMapName( const char *rawMapName, char *out, int outSize )
{
    char        cleaned[ MAX_QPATH ];
    char        noExt[ MAX_QPATH ];
    const char *name;

    if ( !rawMapName ) {
        out[0] = '\0';
        return;
    }

    Q_strncpyz( cleaned, rawMapName, sizeof( cleaned ) );
    G_Scr_NormalizePathSlashes( cleaned );

    name = cleaned;
    if ( !Q_stricmpn( name, "maps/", 5 ) ) {
        name += 5;
    }
    if ( !Q_stricmpn( name, "mp/", 3 ) ) {
        name += 3;
    }

    Q_strncpyz( out, name, outSize );

    if ( !Q_stricmp( COM_GetExtension( out ), "bsp" ) ) {
        COM_StripExtension( out, noExt, sizeof( noExt ) );
        Q_strncpyz( out, noExt, outSize );
    }
}

/*
Reads a .gsc source file using the game module's file traps.
The filename received from the GSC library has no extension; we append
".gsc" automatically (same convention as cl_character_cod1.c).
Memory is malloc'd and tracked for batch-free after gsc_link().
*/
static const char *G_Scr_ReadFile( void *ctx, const char *filename, int *status )
{
    fileHandle_t fh;
    int          len;
    char         path[ MAX_QPATH ];
    char         altPath[ MAX_QPATH ];
    char        *buf;
    ScrSrc_t    *hdr;

    (void)ctx;

    /* Append .gsc if the caller didn't already include an extension */
    Q_strncpyz( path, filename, sizeof( path ) );
    G_Scr_NormalizePathSlashes( path );
    if ( !COM_GetExtension( path )[0] ) {
        Q_strcat( path, sizeof( path ), ".gsc" );
    }

    len = trap_FS_FOpenFile( path, &fh, FS_READ );

    if ( ( len <= 0 || !fh ) && !strncmp( path, "maps/mp/", 8 ) ) {
        Com_sprintf( altPath, sizeof( altPath ), "maps/MP/%s", path + 8 );
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    } else if ( ( len <= 0 || !fh ) && !strncmp( path, "maps/MP/", 8 ) ) {
        Com_sprintf( altPath, sizeof( altPath ), "maps/mp/%s", path + 8 );
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    }

    if ( len <= 0 || !fh ) {
        int i;
        Q_strncpyz( altPath, path, sizeof( altPath ) );
        for ( i = 0; altPath[i]; i++ ) {
            altPath[i] = (char)tolower( (unsigned char)altPath[i] );
        }
        len = trap_FS_FOpenFile( altPath, &fh, FS_READ );
        if ( len > 0 && fh ) {
            Q_strncpyz( path, altPath, sizeof( path ) );
        }
    }

    if ( len <= 0 || !fh ) {
        if ( fh ) {
            trap_FS_FCloseFile( fh );
        }
        *status = GSC_NOT_FOUND;
        return NULL;
    }

    /* Allocate: ScrSrc_t header (for tracking) + source + NUL */
    buf = (char *)malloc( sizeof( ScrSrc_t ) + (size_t)len + 1 );
    if ( !buf ) {
        trap_FS_FCloseFile( fh );
        *status = GSC_OUT_OF_MEMORY;
        return NULL;
    }

    trap_FS_Read( buf + sizeof( ScrSrc_t ), len, fh );
    trap_FS_FCloseFile( fh );
    buf[ sizeof( ScrSrc_t ) + len ] = '\0';

    /* Track for deferred free */
    hdr       = (ScrSrc_t *)buf;
    hdr->next = g_scrSources;
    g_scrSources = hdr;

    *status = GSC_OK;
    return buf + sizeof( ScrSrc_t );
}

/* =========================================================================
   Helper: compile one script (and all its #using dependencies)
   Returns qtrue if compilation succeeded.
   ========================================================================= */
static qboolean G_Scr_CompileScript( const char *nameSpace )
{
    int         status;
    const char *dep;
    const char *failedName;

    failedName = nameSpace;
    status = gsc_compile( g_scrCtx, nameSpace, 0 );

    while ( status == GSC_OK &&
            ( dep = gsc_next_compile_dependency( g_scrCtx ) ) != NULL ) {
        failedName = dep;
        status = gsc_compile( g_scrCtx, dep, 0 );
    }

    if ( status != GSC_OK ) {
        G_Printf( "GSC: compile failed for '%s' (status %d)\n",
                  failedName, status );
        return qfalse;
    }
    return qtrue;
}

/* =========================================================================
   Entity proxy — methods callable as  self methodName(args)
   ========================================================================= */

/* Retrieve the gentity_t* stored as userdata on the "self" entity object.
   self is at argument index -1 in the current call frame.                 */
static gentity_t *G_Scr_GetSelf( gsc_Context *ctx )
{
    int selfIdx = gsc_get_object( ctx, -1 );
    if ( selfIdx < 0 ) {
        return NULL;
    }
    return (gentity_t *)gsc_object_get_userdata( ctx, selfIdx );
}

static qboolean G_Scr_GetClientNumForEntity( const gentity_t *ent, int *outClientNum )
{
    int clientNum;

    if ( !ent || !ent->client ) {
        return qfalse;
    }

    clientNum = (int)( ent - g_entities );
    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return qfalse;
    }

    if ( outClientNum ) {
        *outClientNum = clientNum;
    }
    return qtrue;
}

static const char *G_Scr_TeamToString( team_t team )
{
    switch ( team ) {
        case TEAM_RED:       return "allies";
        case TEAM_BLUE:      return "axis";
        case TEAM_FREE:      return "none";
        case TEAM_SPECTATOR: return "spectator";
        default:             return "spectator";
    }
}

static int G_Scr_GetIntArg( gsc_Context *ctx, int arg, int defaultValue )
{
    int type;

    if ( !ctx || gsc_numargs( ctx ) <= arg ) {
        return defaultValue;
    }

    type = gsc_get_type( ctx, arg );
    if ( type == GSC_TYPE_INTEGER ) {
        return (int)gsc_get_int( ctx, arg );
    }
    if ( type == GSC_TYPE_FLOAT ) {
        return (int)gsc_get_float( ctx, arg );
    }
    return defaultValue;
}

static void G_Scr_SetClientSessionTeam( gentity_t *ent, const char *sessionTeam )
{
    if ( !ent || !ent->client || !sessionTeam ) {
        return;
    }

    if ( !Q_stricmp( sessionTeam, "spectator" ) ) {
        ent->client->sess.sessionTeam = TEAM_SPECTATOR;
        ent->client->sess.spectatorState = SPECTATOR_FREE;
    } else if ( !Q_stricmp( sessionTeam, "allies" ) ||
                !Q_stricmp( sessionTeam, "red" ) ) {
        ent->client->sess.sessionTeam = TEAM_RED;
        ent->client->sess.spectatorState = SPECTATOR_NOT;
    } else if ( !Q_stricmp( sessionTeam, "axis" ) ||
                !Q_stricmp( sessionTeam, "blue" ) ) {
        ent->client->sess.sessionTeam = TEAM_BLUE;
        ent->client->sess.spectatorState = SPECTATOR_NOT;
    } else {
        ent->client->sess.sessionTeam = TEAM_FREE;
        ent->client->sess.spectatorState = SPECTATOR_NOT;
    }
}

static gentity_t *G_Scr_GetEntityFromArg( gsc_Context *ctx, int arg )
{
    int type = gsc_get_type( ctx, arg );

    if ( type == GSC_TYPE_OBJECT ) {
        int obj = gsc_get_object( ctx, arg );
        if ( obj >= 0 ) {
            return (gentity_t *)gsc_object_get_userdata( ctx, obj );
        }
        return NULL;
    }

    if ( type == GSC_TYPE_INTEGER ) {
        int entNum = (int)gsc_get_int( ctx, arg );
        if ( entNum >= 0 && entNum < level.num_entities ) {
            return &g_entities[ entNum ];
        }
    }

    return NULL;
}

static char *G_Scr_CopyString( const char *src )
{
    size_t len;
    char  *dst;

    if ( !src ) {
        src = "";
    }

    len = strlen( src );
    dst = (char *)G_Alloc( (int)len + 1 );
    if ( !dst ) {
        return NULL;
    }

    memcpy( dst, src, len + 1 );
    return dst;
}

static qboolean G_Scr_GetEntityNum( const gentity_t *ent, int *outEntNum )
{
    int entNum;

    if ( !ent ) {
        return qfalse;
    }

    entNum = (int)( ent - g_entities );
    if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
        return qfalse;
    }

    if ( outEntNum ) {
        *outEntNum = entNum;
    }
    return qtrue;
}

static void G_Scr_ClearEntityRuntimeState( int entNum )
{
    if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
        return;
    }

    Com_Memset( &g_scrAttachStates[ entNum ], 0, sizeof( g_scrAttachStates[ entNum ] ) );
    g_scrViewModels[ entNum ][ 0 ] = '\0';
}

static G_ScrAttachState_t *G_Scr_GetAttachStateForSelf( gsc_Context *ctx, int *outEntNum )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int        entNum;

    if ( !G_Scr_GetEntityNum( ent, &entNum ) ) {
        return NULL;
    }

    if ( outEntNum ) {
        *outEntNum = entNum;
    }

    return &g_scrAttachStates[ entNum ];
}

static const char *G_Scr_GetOptionalStringArg( gsc_Context *ctx, int arg, const char *fallback )
{
    int type = gsc_get_type( ctx, arg );

    if ( type == GSC_TYPE_STRING || type == GSC_TYPE_INTERNED_STRING ) {
        return gsc_get_string( ctx, arg );
    }

    return fallback ? fallback : "";
}

static void G_Scr_GetEntityGlobalName( int entNum, char *name, int nameSize )
{
    if ( entNum >= 0 && entNum < MAX_CLIENTS ) {
        Com_sprintf( name, nameSize, "player_%d", entNum );
    } else {
        Com_sprintf( name, nameSize, "entity_%d", entNum );
    }
}

static void G_Scr_ClearGlobal( const char *name )
{
    gsc_add_int( g_scrCtx, 0 ); /* undefined-equivalent in this VM */
    gsc_set_global( g_scrCtx, name );
}

static qboolean G_Scr_SetObjectProxy( gsc_Context *ctx, int objIdx, gsc_Ref proxyRef )
{
    int topBefore;

    if ( !ctx || proxyRef == GSC_NOREF ) {
        return qfalse;
    }

    topBefore = gsc_top( ctx );
    gsc_push_ref( ctx, proxyRef );

    if ( gsc_top( ctx ) <= topBefore || gsc_type( ctx, -1 ) != GSC_TYPE_OBJECT ) {
        if ( gsc_top( ctx ) > topBefore ) {
            gsc_pop( ctx, 1 );
        }
        return qfalse;
    }

    gsc_object_set_proxy( ctx, objIdx, gsc_top( ctx ) - 1 );
    gsc_pop( ctx, 1 );
    return qtrue;
}

static void G_Scr_SetEntityObjectFields( gsc_Context *ctx, int entObj, gentity_t *ent )
{
    int entNum = -1;

    if ( !ent ) {
        return;
    }

    G_Scr_GetEntityNum( ent, &entNum );

    gsc_add_string( ctx, ent->classname ? ent->classname : "" );
    gsc_object_set_field( ctx, entObj, "classname" );
    gsc_add_string( ctx, ent->targetname ? ent->targetname : "" );
    gsc_object_set_field( ctx, entObj, "targetname" );
    gsc_add_string( ctx, ent->target ? ent->target : "" );
    gsc_object_set_field( ctx, entObj, "target" );
    gsc_add_string( ctx, ent->model ? ent->model : "" );
    gsc_object_set_field( ctx, entObj, "model" );

    gsc_add_vec3( ctx, ent->r.currentOrigin );
    gsc_object_set_field( ctx, entObj, "origin" );
    gsc_add_vec3( ctx, ent->s.angles );
    gsc_object_set_field( ctx, entObj, "angles" );

    gsc_add_int( ctx, entNum );
    gsc_object_set_field( ctx, entObj, "entitynum" );

    if ( ent->client ) {
        const char *sessionState;
        int         maxHealth;

        gsc_add_string( ctx, ent->client->pers.netname );
        gsc_object_set_field( ctx, entObj, "name" );

        gsc_object_get_field( ctx, entObj, "pers" );
        if ( gsc_type( ctx, -1 ) != GSC_TYPE_OBJECT ) {
            gsc_pop( ctx, 1 );
            gsc_add_object( ctx );
            gsc_object_set_field( ctx, entObj, "pers" );
        } else {
            gsc_pop( ctx, 1 );
        }

        sessionState = "dead";
        if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
            sessionState = "spectator";
        } else if ( ent->health > 0 ) {
            sessionState = "playing";
        }

        maxHealth = ent->client->pers.maxHealth;
        if ( maxHealth <= 0 ) {
            maxHealth = 100;
        }

        gsc_object_get_field( ctx, entObj, "sessionteam" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_string( ctx, G_Scr_TeamToString( ent->client->sess.sessionTeam ) );
            gsc_object_set_field( ctx, entObj, "sessionteam" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "sessionstate" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_string( ctx, sessionState );
            gsc_object_set_field( ctx, entObj, "sessionstate" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "spectatorclient" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_int( ctx, ent->client->sess.spectatorClient );
            gsc_object_set_field( ctx, entObj, "spectatorclient" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "score" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_int( ctx, ent->client->ps.persistant[ PERS_SCORE ] );
            gsc_object_set_field( ctx, entObj, "score" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "deaths" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_int( ctx, ent->client->ps.persistant[ PERS_KILLED ] );
            gsc_object_set_field( ctx, entObj, "deaths" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "maxhealth" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_int( ctx, maxHealth );
            gsc_object_set_field( ctx, entObj, "maxhealth" );
        } else {
            gsc_pop( ctx, 1 );
        }

        gsc_object_get_field( ctx, entObj, "health" );
        if ( gsc_type( ctx, -1 ) == GSC_TYPE_UNDEFINED ) {
            gsc_pop( ctx, 1 );
            gsc_add_int( ctx, ent->health );
            gsc_object_set_field( ctx, entObj, "health" );
        } else {
            gsc_pop( ctx, 1 );
        }
    }
}

static void G_Scr_CreateEntityObject( gentity_t *ent, qboolean keepOnStack )
{
    char globalName[ 32 ];
    int  entObj;
    int  entNum;

    if ( !g_scrCtx || !ent || !G_Scr_GetEntityNum( ent, &entNum ) ) {
        return;
    }

    entObj = gsc_add_tagged_object( g_scrCtx, "#entity" );
    if ( entNum < MAX_CLIENTS ) {
        G_Scr_SetObjectProxy( g_scrCtx, entObj, g_scrPlayerProxyRef );
    } else {
        G_Scr_SetObjectProxy( g_scrCtx, entObj, g_scrEntityProxyRef );
    }
    gsc_object_set_userdata( g_scrCtx, entObj, ent );
    G_Scr_SetEntityObjectFields( g_scrCtx, entObj, ent );

    if ( entNum < MAX_CLIENTS ) {
        /* release old ref if any, then take a new one */
        if ( g_scrPlayerRefs[ entNum ] != GSC_NOREF ) {
            gsc_unref( g_scrCtx, g_scrPlayerRefs[ entNum ] );
        }
        g_scrPlayerRefs[ entNum ] = gsc_ref( g_scrCtx, entObj );
    } else {
        g_scrEntityObjAlive[ entNum ] = qtrue;
    }

    G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
    gsc_set_global( g_scrCtx, globalName ); /* pops entObj */

    if ( keepOnStack ) {
        if ( entNum < MAX_CLIENTS && g_scrPlayerRefs[ entNum ] != GSC_NOREF ) {
            gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ entNum ] );
        } else {
            gsc_get_global( g_scrCtx, globalName );
        }
    }
}

static void G_Scr_PushEntityObject( gsc_Context *ctx, gentity_t *ent )
{
    char globalName[ 32 ];
    int  entObj;
    int  entNum;
    int clientNum;
    int topBefore;

    if ( !ent || !G_Scr_GetEntityNum( ent, &entNum ) ) {
        return;
    }

    if ( G_Scr_GetClientNumForEntity( ent, &clientNum ) &&
         g_scrPlayerRefs[ clientNum ] != GSC_NOREF ) {
        topBefore = gsc_top( ctx );
        gsc_push_ref( ctx, g_scrPlayerRefs[ clientNum ] );
        if ( gsc_top( ctx ) > topBefore && gsc_type( ctx, -1 ) == GSC_TYPE_OBJECT ) {
            entObj = gsc_top( ctx ) - 1;
            gsc_object_set_userdata( ctx, entObj, ent );
            G_Scr_SetEntityObjectFields( ctx, entObj, ent );
            return;
        }
    }

    if ( entNum >= MAX_CLIENTS && g_scrEntityObjAlive[ entNum ] ) {
        topBefore = gsc_top( ctx );
        G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
        gsc_get_global( ctx, globalName );
        if ( gsc_top( ctx ) > topBefore && gsc_type( ctx, -1 ) == GSC_TYPE_OBJECT ) {
            entObj = gsc_top( ctx ) - 1;
            gsc_object_set_userdata( ctx, entObj, ent );
            G_Scr_SetEntityObjectFields( ctx, entObj, ent );
            return;
        }
        if ( gsc_top( ctx ) > topBefore ) {
            gsc_pop( ctx, 1 );
        }
        g_scrEntityObjAlive[ entNum ] = qfalse;
    }

    G_Scr_CreateEntityObject( ent, qtrue );
}

static G_ScrHudElem_t *G_Scr_Hud_Alloc( void )
{
    int i;

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( hud->inuse ) {
            continue;
        }

        Com_Memset( hud, 0, sizeof( *hud ) );
        hud->inuse = qtrue;
        hud->id = i + 1;
        hud->scope = G_SCR_HUD_SCOPE_ALL;
        hud->owner = -1;
        hud->scale = 0.35f;
        hud->color[0] = 1.0f;
        hud->color[1] = 1.0f;
        hud->color[2] = 1.0f;
        hud->color[3] = 1.0f;
        return hud;
    }

    return NULL;
}

static void G_Scr_PushHudElemObject( gsc_Context *ctx, G_ScrHudScope_t scope, int owner )
{
    float          white[3] = { 1.0f, 1.0f, 1.0f };
    G_ScrHudElem_t *hud;
    int            obj;

    hud = G_Scr_Hud_Alloc();

    obj = gsc_add_tagged_object( ctx, "#hudelem" );
    G_Scr_SetObjectProxy( ctx, obj, g_scrHudElemProxyRef );

    if ( hud ) {
        hud->scope = scope;
        hud->owner = owner;
        gsc_object_set_userdata( ctx, obj, (void *)(intptr_t)hud->id );
    } else {
        /* Safe fallback: still return an object so scripts don't crash on field writes. */
        gsc_object_set_userdata( ctx, obj, NULL );
    }

    gsc_add_float( ctx, hud ? hud->x : 0.0f );
    gsc_object_set_field( ctx, obj, "x" );
    gsc_add_float( ctx, hud ? hud->y : 0.0f );
    gsc_object_set_field( ctx, obj, "y" );
    gsc_add_float( ctx, hud ? hud->scale : 0.35f );
    gsc_object_set_field( ctx, obj, "fontscale" );
    gsc_add_vec3( ctx, white );
    gsc_object_set_field( ctx, obj, "color" );
    gsc_add_float( ctx, hud ? hud->color[3] : 1.0f );
    gsc_object_set_field( ctx, obj, "alpha" );
}

static int G_Scr_NoopReturn0( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int G_Scr_NoopReturn1( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_int( ctx, 1 );
    return 1;
}

static int G_Scr_NoopFalse( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_bool( ctx, qfalse );
    return 1;
}

static int G_Scr_NoopNoneString( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_string( ctx, "none" );
    return 1;
}

static int G_Scr_NoopZero( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_int( ctx, 0 );
    return 1;
}

static qboolean G_Scr_ShouldAutoMenuResponse( void )
{
    char value[ 16 ];

    trap_Cvar_VariableStringBuffer( "scr_menu_autoresponse", value, sizeof( value ) );
    if ( !value[0] ) {
        trap_Cvar_Set( "scr_menu_autoresponse", "1" );
        return qtrue;
    }

    return atoi( value ) != 0;
}

static qboolean G_Scr_GetDefaultWeaponForMenu( const char *menu, char *out, int outSize )
{
    if ( !menu || !menu[0] ) {
        return qfalse;
    }

    if ( Q_stristr( menu, "american" ) ) {
        Q_strncpyz( out, "m1carbine_mp", outSize );
        return qtrue;
    }

    if ( Q_stristr( menu, "british" ) ) {
        Q_strncpyz( out, "enfield_mp", outSize );
        return qtrue;
    }

    if ( Q_stristr( menu, "russian" ) ) {
        Q_strncpyz( out, "mosin_nagant_mp", outSize );
        return qtrue;
    }

    if ( Q_stristr( menu, "german" ) ) {
        Q_strncpyz( out, "kar98k_mp", outSize );
        return qtrue;
    }

    return qfalse;
}

static qboolean G_Scr_GetAutoMenuResponse( const char *menu, char *out, int outSize )
{
    if ( !menu || !menu[0] ) {
        return qfalse;
    }

    if ( !Q_stricmpn( menu, "serverinfo_", 11 ) ) {
        Q_strncpyz( out, "close", outSize );
        return qtrue;
    }

    if ( !Q_stricmpn( menu, "team_", 5 ) ) {
        Q_strncpyz( out, "autoassign", outSize );
        return qtrue;
    }

    if ( !Q_stricmpn( menu, "weapon_", 7 ) ) {
        return G_Scr_GetDefaultWeaponForMenu( menu, out, outSize );
    }

    return qfalse;
}

static void G_Scr_ApplyPlayerSessionFromObject( gsc_Context *ctx, gentity_t *ent )
{
    const char *sessionTeam;
    const char *sessionState;
    int         selfObj;

    if ( !ctx || !ent || !ent->client ) {
        return;
    }

    selfObj = gsc_get_object( ctx, -1 );
    if ( selfObj < 0 ) {
        return;
    }

    gsc_object_get_field( ctx, selfObj, "sessionteam" );
    if ( gsc_type( ctx, -1 ) == GSC_TYPE_STRING ||
         gsc_type( ctx, -1 ) == GSC_TYPE_INTERNED_STRING ) {
        sessionTeam = gsc_to_string( ctx, -1 );
        if ( sessionTeam ) {
            G_Scr_SetClientSessionTeam( ent, sessionTeam );
        }
    }
    gsc_pop( ctx, 1 );

    /* sessionstate: CoD1 scripts set "playing"/"spectator"/"dead" to
       control the player's movement type and spectator state. */
    gsc_object_get_field( ctx, selfObj, "sessionstate" );
    if ( gsc_type( ctx, -1 ) == GSC_TYPE_STRING ||
         gsc_type( ctx, -1 ) == GSC_TYPE_INTERNED_STRING ) {
        sessionState = gsc_to_string( ctx, -1 );
        if ( sessionState ) {
            if ( !Q_stricmp( sessionState, "playing" ) ) {
                ent->client->ps.pm_type = PM_NORMAL;
                ent->client->sess.spectatorState = SPECTATOR_NOT;
                /* If team is still spectator, move to TEAM_FREE so they
                   can actually play (script should set sessionteam first). */
                if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
                    ent->client->sess.sessionTeam = TEAM_FREE;
                }
            } else if ( !Q_stricmp( sessionState, "spectator" ) ) {
                ent->client->ps.pm_type = PM_SPECTATOR;
                ent->client->sess.spectatorState = SPECTATOR_FREE;
                ent->client->sess.sessionTeam = TEAM_SPECTATOR;
            } else if ( !Q_stricmp( sessionState, "dead" ) ) {
                ent->client->ps.pm_type = PM_DEAD;
                ent->client->sess.spectatorState = SPECTATOR_NOT;
            }
        }
    }
    gsc_pop( ctx, 1 );

    gsc_object_get_field( ctx, selfObj, "spectatorclient" );
    if ( gsc_type( ctx, -1 ) == GSC_TYPE_INTEGER ||
         gsc_type( ctx, -1 ) == GSC_TYPE_FLOAT ) {
        ent->client->sess.spectatorClient = (int)gsc_to_int( ctx, -1 );
    }
    gsc_pop( ctx, 1 );
}

static qboolean G_Scr_ClassnameMatches( const char *wanted, const char *actual )
{
    if ( !wanted || !wanted[0] || !actual || !actual[0] ) {
        return qfalse;
    }

    if ( !Q_stricmp( wanted, actual ) ) {
        return qtrue;
    }

    /*
     * CoD MP spawn entities are remapped to Q3 spawn classes by the gamecode.
     * Keep script lookups working by aliasing CoD class requests.
     */
    if ( !Q_stricmp( actual, "info_player_deathmatch" ) &&
         !Q_stricmpn( wanted, "mp_", 3 ) &&
         Q_stristr( wanted, "_spawn" ) ) {
        return qtrue;
    }

    if ( !Q_stricmp( actual, "info_player_intermission" ) &&
         !Q_stricmpn( wanted, "mp_", 3 ) &&
         Q_stristr( wanted, "intermission" ) ) {
        return qtrue;
    }

    return qfalse;
}

static qboolean G_Scr_EntityMatchesFilter( gentity_t *ent, const char *value, const char *key )
{
    if ( !ent || !ent->inuse || !value || !value[0] ) {
        return qfalse;
    }

    if ( !key || !key[0] || !Q_stricmp( key, "classname" ) ) {
        return G_Scr_ClassnameMatches( value, ent->classname );
    }

    if ( !Q_stricmp( key, "targetname" ) ) {
        return ent->targetname && !Q_stricmp( ent->targetname, value );
    }

    if ( !Q_stricmp( key, "target" ) ) {
        return ent->target && !Q_stricmp( ent->target, value );
    }

    if ( !Q_stricmp( key, "model" ) ) {
        return ent->model && !Q_stricmp( ent->model, value );
    }

    /* Arbitrary field: look it up on the entity's GSC object */
    {
        int  entNum;
        char globalName[ 32 ];
        int  topBefore;
        qboolean match = qfalse;

        if ( g_scrCtx && G_Scr_GetEntityNum( ent, &entNum ) ) {
            G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
            topBefore = gsc_top( g_scrCtx );
            gsc_get_global( g_scrCtx, globalName );
            if ( gsc_top( g_scrCtx ) > topBefore &&
                 gsc_type( g_scrCtx, -1 ) == GSC_TYPE_OBJECT ) {
                int entObjIdx = gsc_top( g_scrCtx ) - 1;
                int top2 = gsc_top( g_scrCtx );
                gsc_object_get_field( g_scrCtx, entObjIdx, key );
                if ( gsc_top( g_scrCtx ) > top2 ) {
                    int t = gsc_type( g_scrCtx, -1 );
                    if ( t == GSC_TYPE_STRING || t == GSC_TYPE_INTERNED_STRING ) {
                        const char *fv = gsc_to_string( g_scrCtx, -1 );
                        match = ( fv && !Q_stricmp( fv, value ) );
                    }
                    gsc_pop( g_scrCtx, 1 );
                }
            }
            if ( gsc_top( g_scrCtx ) > topBefore )
                gsc_pop( g_scrCtx, gsc_top( g_scrCtx ) - topBefore );
        }
        return match;
    }
}

static int GScr_Meth_GetName( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        gsc_add_string( ctx, "" );
        return 1;
    }
    gsc_add_string( ctx, ent->client->pers.netname );
    return 1;
}

static int GScr_Meth_GetOrigin( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        gsc_add_vec3( ctx, zero );
        return 1;
    }
    gsc_add_vec3( ctx, ent->r.currentOrigin );
    return 1;
}

static int GScr_Meth_SetOrigin( gsc_Context *ctx )
{
    float     v[3];
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }
    gsc_get_vec3( ctx, 0, v );
    if ( ent->client ) {
        VectorCopy( v, ent->client->ps.origin );
    }
    G_SetOrigin( ent, v );
    return 0;
}

static int GScr_Meth_GetHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_int( ctx, ent ? ent->health : 0 );
    return 1;
}

static int GScr_Meth_SetHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }
    ent->health = (int)gsc_get_int( ctx, 0 );
    if ( ent->client ) {
        ent->client->ps.stats[ STAT_HEALTH ] = ent->health;
    }
    return 0;
}

static int GScr_Meth_GetTeam( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !ent->client ) {
        gsc_add_string( ctx, "spectator" );
        return 1;
    }
    gsc_add_string( ctx, G_Scr_TeamToString( ent->client->sess.sessionTeam ) );
    return 1;
}

static void G_Scr_SetSelfFieldInt( gsc_Context *ctx, const char *field, int value )
{
    int selfObj;

    if ( !ctx || !field || !field[0] ) {
        return;
    }

    selfObj = gsc_get_object( ctx, -1 );
    if ( selfObj < 0 ) {
        return;
    }

    gsc_add_int( ctx, value );
    gsc_object_set_field( ctx, selfObj, field );
}

static void G_Scr_SetSelfFieldString( gsc_Context *ctx, const char *field, const char *value )
{
    int selfObj;

    if ( !ctx || !field || !field[0] ) {
        return;
    }

    selfObj = gsc_get_object( ctx, -1 );
    if ( selfObj < 0 ) {
        return;
    }

    gsc_add_string( ctx, value ? value : "" );
    gsc_object_set_field( ctx, selfObj, field );
}

static void G_Scr_SetSelfFieldVec3( gsc_Context *ctx, const char *field, const float v[3] )
{
    int selfObj;
    float vec[3];

    if ( !ctx || !field || !field[0] || !v ) {
        return;
    }

    selfObj = gsc_get_object( ctx, -1 );
    if ( selfObj < 0 ) {
        return;
    }

    vec[0] = v[0];
    vec[1] = v[1];
    vec[2] = v[2];
    gsc_add_vec3( ctx, vec );
    gsc_object_set_field( ctx, selfObj, field );
}

static int GScr_Field_GetAngles( gsc_Context *ctx )
{
    float     zero[3] = { 0.0f, 0.0f, 0.0f };
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( !ent ) {
        gsc_add_vec3( ctx, zero );
        return 1;
    }

    gsc_add_vec3( ctx, ent->s.angles );
    return 1;
}

static int GScr_Field_SetAngles( gsc_Context *ctx )
{
    vec3_t     angles;
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( !ent ) {
        return 0;
    }

    gsc_get_vec3( ctx, 0, angles );
    if ( ent->client ) {
        SetClientViewAngle( ent, angles );
    } else {
        VectorCopy( angles, ent->s.angles );
        VectorCopy( angles, ent->r.currentAngles );
        trap_LinkEntity( ent );
    }

    G_Scr_SetSelfFieldVec3( ctx, "angles", angles );
    return 0;
}

static int GScr_Field_GetScore( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int score = 0;

    if ( ent && ent->client ) {
        score = ent->client->ps.persistant[ PERS_SCORE ];
    }
    gsc_add_int( ctx, score );
    return 1;
}

static int GScr_Field_SetScore( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int score = G_Scr_GetIntArg( ctx, 0, 0 );

    if ( ent && ent->client ) {
        ent->client->ps.persistant[ PERS_SCORE ] = score;
    }
    G_Scr_SetSelfFieldInt( ctx, "score", score );
    return 0;
}

static int GScr_Field_GetDeaths( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int deaths = 0;

    if ( ent && ent->client ) {
        deaths = ent->client->ps.persistant[ PERS_KILLED ];
    }
    gsc_add_int( ctx, deaths );
    return 1;
}

static int GScr_Field_SetDeaths( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int deaths = G_Scr_GetIntArg( ctx, 0, 0 );

    if ( ent && ent->client ) {
        ent->client->ps.persistant[ PERS_KILLED ] = deaths;
    }
    G_Scr_SetSelfFieldInt( ctx, "deaths", deaths );
    return 0;
}

static int GScr_Field_GetMaxHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int maxHealth = 100;

    if ( ent && ent->client && ent->client->pers.maxHealth > 0 ) {
        maxHealth = ent->client->pers.maxHealth;
    }
    gsc_add_int( ctx, maxHealth );
    return 1;
}

static int GScr_Field_SetMaxHealth( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int maxHealth = G_Scr_GetIntArg( ctx, 0, 100 );

    if ( maxHealth <= 0 ) {
        maxHealth = 100;
    }

    if ( ent && ent->client ) {
        ent->client->pers.maxHealth = maxHealth;
        ent->client->ps.stats[ STAT_MAX_HEALTH ] = maxHealth;
        if ( ent->health > maxHealth ) {
            ent->health = maxHealth;
            ent->client->ps.stats[ STAT_HEALTH ] = maxHealth;
        }
    }

    G_Scr_SetSelfFieldInt( ctx, "maxhealth", maxHealth );
    return 0;
}

static int GScr_Field_GetSessionTeam( gsc_Context *ctx )
{
    return GScr_Meth_GetTeam( ctx );
}

static int GScr_Field_SetSessionTeam( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    const char *sessionTeam = gsc_get_string( ctx, 0 );

    if ( !sessionTeam ) {
        sessionTeam = "none";
    }

    if ( ent && ent->client ) {
        G_Scr_SetClientSessionTeam( ent, sessionTeam );
        sessionTeam = G_Scr_TeamToString( ent->client->sess.sessionTeam );
    }

    G_Scr_SetSelfFieldString( ctx, "sessionteam", sessionTeam );
    return 0;
}

static int GScr_Field_GetSessionState( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    const char *state = "dead";

    if ( ent && ent->client ) {
        if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ||
             ent->client->ps.pm_type == PM_SPECTATOR ) {
            state = "spectator";
        } else if ( ent->health > 0 ) {
            state = "playing";
        }
    }

    gsc_add_string( ctx, state );
    return 1;
}

static int GScr_Field_SetSessionState( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    const char *sessionState = gsc_get_string( ctx, 0 );

    if ( !sessionState ) {
        sessionState = "spectator";
    }

    if ( ent && ent->client ) {
        if ( !Q_stricmp( sessionState, "playing" ) ) {
            ent->client->ps.pm_type = PM_NORMAL;
            ent->client->sess.spectatorState = SPECTATOR_NOT;
            if ( ent->client->sess.sessionTeam == TEAM_SPECTATOR ) {
                ent->client->sess.sessionTeam = TEAM_FREE;
            }
        } else if ( !Q_stricmp( sessionState, "spectator" ) ) {
            ent->client->ps.pm_type = PM_SPECTATOR;
            ent->client->sess.spectatorState = SPECTATOR_FREE;
            ent->client->sess.sessionTeam = TEAM_SPECTATOR;
        } else if ( !Q_stricmp( sessionState, "dead" ) ) {
            ent->client->ps.pm_type = PM_DEAD;
            ent->client->sess.spectatorState = SPECTATOR_NOT;
        }
    }

    G_Scr_SetSelfFieldString( ctx, "sessionstate", sessionState );
    return 0;
}

static int GScr_Field_GetSpectatorClient( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int spectatorClient = -1;

    if ( ent && ent->client ) {
        spectatorClient = ent->client->sess.spectatorClient;
    }

    gsc_add_int( ctx, spectatorClient );
    return 1;
}

static int GScr_Field_SetSpectatorClient( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int spectatorClient = G_Scr_GetIntArg( ctx, 0, -1 );

    if ( ent && ent->client ) {
        ent->client->sess.spectatorClient = spectatorClient;
    }

    G_Scr_SetSelfFieldInt( ctx, "spectatorclient", spectatorClient );
    return 0;
}

static int GScr_Meth_GetClientNum( gsc_Context *ctx )
{
    int clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        gsc_add_int( ctx, clientNum );
    } else {
        gsc_add_int( ctx, ent ? ent->s.number : -1 );
    }
    return 1;
}

static int GScr_Meth_GetEntityNumber( gsc_Context *ctx )
{
    return GScr_Meth_GetClientNum( ctx );
}

static int GScr_Meth_GetGuid( gsc_Context *ctx )
{
    int clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( !G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        gsc_add_int( ctx, -1 );
        return 1;
    }

    /*
     * ioq3 does not expose CoD-style integer GUIDs in gamecode.
     * Return stable client slot as best-effort compatibility value.
     */
    gsc_add_int( ctx, clientNum );
    return 1;
}

static int GScr_Meth_IsPlayer( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->pers.connected != CON_DISCONNECTED );
    return 1;
}

static int GScr_Meth_IsAlive( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->health > 0 );
    return 1;
}

/* self spawn() — respawn the player at a spawn point */
static int GScr_Meth_Spawn( gsc_Context *ctx )
{
    vec3_t spawnOrigin;
    vec3_t spawnAngles;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client &&
         ent->client->pers.connected == CON_CONNECTED ) {
        G_Scr_ApplyPlayerSessionFromObject( ctx, ent );
        ClientSpawn( ent );
        if ( gsc_numargs( ctx ) >= 2 &&
             gsc_get_type( ctx, 0 ) == GSC_TYPE_VECTOR &&
             gsc_get_type( ctx, 1 ) == GSC_TYPE_VECTOR ) {
            gsc_get_vec3( ctx, 0, spawnOrigin );
            gsc_get_vec3( ctx, 1, spawnAngles );
            TeleportPlayer( ent, spawnOrigin, spawnAngles );
        }
    }
    return 0;
}

/* self suicide() */
static int GScr_Meth_Suicide( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client && ent->health > 0 ) {
        G_Damage( ent, ent, ent, NULL, NULL,
                  ent->health + 1, DAMAGE_NO_PROTECTION, MOD_SUICIDE );
    }
    return 0;
}

/* CoD map setup helper used by gametype scripts on spawnpoint entities. */
static int GScr_Meth_PlaceSpawnpoint( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }

    VectorCopy( ent->s.origin, ent->s.pos.trBase );
    VectorCopy( ent->s.origin, ent->r.currentOrigin );
    trap_LinkEntity( ent );
    return 0;
}

static int GScr_Meth_Delete( gsc_Context *ctx )
{
    int entNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent ) {
        return 0;
    }

    entNum = (int)( ent - g_entities );
    if ( entNum >= MAX_CLIENTS && ent->inuse ) {
        G_FreeEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Hide( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.svFlags |= SVF_NOCLIENT;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Show( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.svFlags &= ~SVF_NOCLIENT;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Unlink( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        trap_UnlinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_NotSolid( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.contents = 0;
        ent->clipmask = 0;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_Solid( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent ) {
        ent->r.contents = CONTENTS_SOLID;
        ent->clipmask = MASK_SOLID;
        trap_LinkEntity( ent );
    }
    return 0;
}

static int GScr_Meth_SetModel( gsc_Context *ctx )
{
    const char *model = gsc_get_string( ctx, 0 );
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( !ent || !model || !model[0] ) {
        return 0;
    }

    ent->model = G_Scr_CopyString( model );
    if ( model[0] == '*' ) {
        trap_SetBrushModel( ent, model );
    }
    trap_LinkEntity( ent );
    return 0;
}

static int GScr_Meth_IsTouching( gsc_Context *ctx )
{
    gentity_t *self = G_Scr_GetSelf( ctx );
    gentity_t *other = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx,
                  self && other &&
                  BoundsIntersect( self->r.absmin, self->r.absmax,
                                   other->r.absmin, other->r.absmax ) );
    return 1;
}

static int GScr_Meth_SetClientCvar( gsc_Context *ctx )
{
    int         clientNum;
    const char *name;
    const char *value;
    gentity_t  *ent = G_Scr_GetSelf( ctx );

    if ( !ent || !ent->client || !G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    name = gsc_get_string( ctx, 0 );
    value = gsc_get_string( ctx, 1 );
    if ( !name || !name[0] || !value ) {
        return 0;
    }

    trap_SendServerCommand( clientNum,
                            va( "scr_setcvar \"%s\" \"%s\"", name, value ) );
    return 0;
}

#ifdef STANDALONE
/* Look up a precached menu name's configstring index (0-based).
   Returns -1 if the menu was not precached. */
static int G_Scr_GetMenuIndex( const char *menuName )
{
    char existing[ MAX_QPATH ];
    int i;

    if ( !menuName || !menuName[0] ) return -1;

    for ( i = 0; i < MAX_SCRIPT_MENUS; i++ ) {
        trap_GetConfigstring( CS_SCRIPT_MENUS + i, existing, sizeof( existing ) );
        if ( existing[0] && !Q_stricmp( existing, menuName ) ) {
            return i;
        }
    }
    return -1;
}
#endif

static int GScr_Meth_OpenMenu( gsc_Context *ctx )
{
    int         clientNum;
    const char *menuName;
    gentity_t  *ent = G_Scr_GetSelf( ctx );

    if ( !ent || !ent->client || !G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }

    if ( gsc_numargs( ctx ) < 1 ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }

    menuName = gsc_get_string( ctx, 0 );
    if ( !menuName || !menuName[0] ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }

#ifdef STANDALONE
    {
        int menuIdx = G_Scr_GetMenuIndex( menuName );
        if ( menuIdx >= 0 ) {
            trap_SendServerCommand( clientNum, va( "t %d", menuIdx ) );
        } else {
            trap_SendServerCommand( clientNum, va( "t %s", menuName ) );
        }
    }
#else
    trap_SendServerCommand( clientNum, va( "scr_menu open \"%s\"", menuName ) );
#endif

    gsc_add_int( ctx, 1 );
    return 1;
}

static int GScr_Meth_CloseMenu( gsc_Context *ctx )
{
    int        clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( ent && ent->client && G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        trap_SendServerCommand( clientNum, "u" );
    }
    return 0;
}

static int GScr_Meth_CloseInGameMenu( gsc_Context *ctx )
{
    int        clientNum;
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( ent && ent->client && G_Scr_GetClientNumForEntity( ent, &clientNum ) ) {
        trap_SendServerCommand( clientNum, "u" );
    }
    return 0;
}

static int GScr_Meth_AttackButtonPressed( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & BUTTON_ATTACK ) );
    return 1;
}

static int GScr_Meth_UseButtonPressed( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & BUTTON_USE_HOLDABLE ) );
    return 1;
}

static int GScr_Meth_MeleeButtonPressed( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & BUTTON_MELEE ) );
    return 1;
}

static int GScr_Meth_PlayerADS( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client && ( ent->client->buttons & ( BUTTON_ADS | BUTTON_WALKING ) ) );
    return 1;
}

static int GScr_Meth_IsOnGround( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->ps.groundEntityNum != ENTITYNUM_NONE );
    return 1;
}

/* =========================================================================
   CoD1 weapon slot helpers
   ========================================================================= */
static int G_WeaponSlotForName( const char *slotName )
{
    if ( !Q_stricmp( slotName, "primary" ) )       return 0;
    if ( !Q_stricmp( slotName, "primaryb" ) )      return 1;
    if ( !Q_stricmp( slotName, "pistol" ) )        return 2;
    if ( !Q_stricmp( slotName, "grenade" ) )       return 3;
    if ( !Q_stricmp( slotName, "smokegrenade" ) )  return 4;
    return 0; /* default to primary */
}

static int G_FindWeaponSlot( gclient_t *cl, const char *weapName )
{
    int i;
    for ( i = 0; i < COD1_WEAPON_SLOT_NUM; i++ ) {
        if ( cl->weaponSlots[i].name[0] &&
             !Q_stricmp( cl->weaponSlots[i].name, weapName ) )
            return i;
    }
    return -1;
}

static qboolean G_IsValidWeaponName( const char *name )
{
    if ( !name || !name[0] ) return qfalse;
    if ( !Q_stricmp( name, "undefined" ) ) return qfalse;
    if ( !Q_stricmp( name, "none" ) ) return qfalse;
    if ( name[0] >= '0' && name[0] <= '9' ) return qfalse; /* reject numbers like "2" */
    return qtrue;
}

static void G_SendWeaponToClient( int clientNum, const char *weapName,
                                  int clipAmmo, int reserveAmmo )
{
    trap_SendServerCommand( clientNum,
        va( "weapon %s %d %d", weapName, clipAmmo, reserveAmmo ) );
}

/* =========================================================================
   GSC weapon methods — called on player entity objects
   ========================================================================= */

/* self giveWeapon( weaponName ) */
static int GScr_Meth_GiveWeapon( gsc_Context *ctx )
{
    gentity_t   *ent = G_Scr_GetSelf( ctx );
    const char  *weapName;
    weaponDef_t wd;
    int         slotIdx;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    if ( !G_IsValidWeaponName( weapName ) ) return 0;

    /* already have it? */
    if ( G_FindWeaponSlot( ent->client, weapName ) >= 0 ) return 0;

    /* parse weapon def to get slot type and ammo */
    if ( !BG_ParseWeaponDef( weapName, &wd ) ) {
        G_Printf( "GSC giveWeapon: unknown weapon '%s'\n", weapName );
        return 0;
    }

    slotIdx = G_WeaponSlotForName( wd.weaponSlot );

    /* if slot occupied, try next available of same type or overflow slots */
    if ( ent->client->weaponSlots[slotIdx].name[0] ) {
        int i;
        int found = -1;
        /* try slot+1 (e.g. primaryb for primary) */
        if ( slotIdx == 0 && !ent->client->weaponSlots[1].name[0] ) {
            found = 1;
        } else {
            /* find any empty slot */
            for ( i = 5; i < COD1_WEAPON_SLOT_NUM; i++ ) {
                if ( !ent->client->weaponSlots[i].name[0] ) {
                    found = i;
                    break;
                }
            }
        }
        if ( found < 0 ) {
            G_Printf( "GSC giveWeapon: no empty slot for '%s'\n", weapName );
            return 0;
        }
        slotIdx = found;
    }

    Q_strncpyz( ent->client->weaponSlots[slotIdx].name, weapName,
                sizeof( ent->client->weaponSlots[slotIdx].name ) );
    ent->client->weaponSlots[slotIdx].clipAmmo    = wd.clipSize;
    ent->client->weaponSlots[slotIdx].reserveAmmo = wd.startAmmo;

    /* auto-switch if player has no current weapon */
    if ( ent->client->currentWeaponSlot < 0 ) {
        ent->client->currentWeaponSlot = slotIdx;
        G_SendWeaponToClient( ent->s.number, weapName,
            wd.clipSize, wd.startAmmo );
    }

    return 0;
}

/* self takeWeapon( weaponName ) */
static int GScr_Meth_TakeWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot >= 0 ) {
        ent->client->weaponSlots[slot].name[0] = '\0';
        ent->client->weaponSlots[slot].clipAmmo = 0;
        ent->client->weaponSlots[slot].reserveAmmo = 0;
        if ( ent->client->currentWeaponSlot == slot )
            ent->client->currentWeaponSlot = -1;
    }
    return 0;
}

/* self takeAllWeapons() — used by scripts before giving loadout */
static int GScr_Meth_TakeAllWeapons( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int i;

    if ( !ent || !ent->client ) return 0;
    for ( i = 0; i < COD1_WEAPON_SLOT_NUM; i++ ) {
        ent->client->weaponSlots[i].name[0] = '\0';
        ent->client->weaponSlots[i].clipAmmo = 0;
        ent->client->weaponSlots[i].reserveAmmo = 0;
    }
    ent->client->currentWeaponSlot = -1;
    ent->client->spawnWeapon[0] = '\0';
    /* tell client to drop weapon */
    trap_SendServerCommand( ent->s.number, "weapon none 0 0" );
    return 0;
}

/* self giveMaxAmmo( weaponName ) */
static int GScr_Meth_GiveMaxAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;
    weaponDef_t wd;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot >= 0 && BG_ParseWeaponDef( weapName, &wd ) ) {
        ent->client->weaponSlots[slot].clipAmmo    = wd.clipSize;
        ent->client->weaponSlots[slot].reserveAmmo = wd.maxAmmo;
        /* resend to client if this is the current weapon */
        if ( slot == ent->client->currentWeaponSlot ) {
            G_SendWeaponToClient( ent->s.number, weapName,
                wd.clipSize, wd.maxAmmo );
        }
    }
    return 0;
}

/* self giveStartAmmo( weaponName ) */
static int GScr_Meth_GiveStartAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;
    weaponDef_t wd;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot >= 0 && BG_ParseWeaponDef( weapName, &wd ) ) {
        ent->client->weaponSlots[slot].clipAmmo    = wd.clipSize;
        ent->client->weaponSlots[slot].reserveAmmo = wd.startAmmo;
    }
    return 0;
}

/* self getFractionStartAmmo( weaponName ) — returns current ammo / startAmmo as float */
static int GScr_Meth_GetFractionStartAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;
    weaponDef_t wd;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot < 0 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    if ( !BG_ParseWeaponDef( weapName, &wd ) || wd.startAmmo <= 0 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    gsc_add_float( ctx,
        (float)ent->client->weaponSlots[slot].reserveAmmo / (float)wd.startAmmo );
    return 1;
}

/* self getFractionMaxAmmo( weaponName ) — returns current ammo / maxAmmo as float */
static int GScr_Meth_GetFractionMaxAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;
    weaponDef_t wd;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot < 0 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    if ( !BG_ParseWeaponDef( weapName, &wd ) || wd.maxAmmo <= 0 ) {
        gsc_add_float( ctx, 1.0f );
        return 1;
    }
    gsc_add_float( ctx,
        (float)ent->client->weaponSlots[slot].reserveAmmo / (float)wd.maxAmmo );
    return 1;
}

/* self setSpawnWeapon( weaponName ) — weapon to auto-switch to after spawn */
static int GScr_Meth_SetSpawnWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    if ( !G_IsValidWeaponName( weapName ) ) return 0;
    Q_strncpyz( ent->client->spawnWeapon, weapName,
                sizeof( ent->client->spawnWeapon ) );

    /* auto-switch to spawn weapon */
    {
        int slot = G_FindWeaponSlot( ent->client, weapName );
        if ( slot >= 0 ) {
            ent->client->currentWeaponSlot = slot;
            G_SendWeaponToClient( ent->s.number, weapName,
                ent->client->weaponSlots[slot].clipAmmo,
                ent->client->weaponSlots[slot].reserveAmmo );
        }
    }
    return 0;
}

/* self switchToWeapon( weaponName ) */
static int GScr_Meth_SwitchToWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_bool( ctx, qfalse );
        return 1;
    }
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot < 0 ) {
        gsc_add_bool( ctx, qfalse );
        return 1;
    }

    ent->client->currentWeaponSlot = slot;
    G_SendWeaponToClient( ent->s.number, weapName,
        ent->client->weaponSlots[slot].clipAmmo,
        ent->client->weaponSlots[slot].reserveAmmo );
    gsc_add_bool( ctx, qtrue );
    return 1;
}

/* self getCurrentWeapon() */
static int GScr_Meth_GetCurrentWeapon( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    int slot;

    if ( !ent || !ent->client ||
         ( slot = ent->client->currentWeaponSlot ) < 0 ||
         !ent->client->weaponSlots[slot].name[0] ) {
        gsc_add_string( ctx, "none" );
        return 1;
    }
    gsc_add_string( ctx, ent->client->weaponSlots[slot].name );
    return 1;
}

/* self hasWeapon( weaponName ) */
static int GScr_Meth_HasWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_bool( ctx, qfalse );
        return 1;
    }
    weapName = gsc_to_string( ctx, 0 );
    gsc_add_bool( ctx, G_FindWeaponSlot( ent->client, weapName ) >= 0 );
    return 1;
}

/* self getWeaponSlotWeapon( slotName ) */
static int GScr_Meth_GetWeaponSlotWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_string( ctx, "none" );
        return 1;
    }
    slotName = gsc_to_string( ctx, 0 );
    slot = G_WeaponSlotForName( slotName );
    if ( ent->client->weaponSlots[slot].name[0] )
        gsc_add_string( ctx, ent->client->weaponSlots[slot].name );
    else
        gsc_add_string( ctx, "none" );
    return 1;
}

/* self setWeaponSlotWeapon( slotName, weaponName ) */
static int GScr_Meth_SetWeaponSlotWeapon( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName, *weapName;
    int         slot;
    weaponDef_t wd;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 2 ) return 0;
    slotName = gsc_to_string( ctx, 0 );
    weapName = gsc_to_string( ctx, 1 );
    slot = G_WeaponSlotForName( slotName );

    if ( !weapName[0] || !Q_stricmp( weapName, "none" ) ) {
        ent->client->weaponSlots[slot].name[0] = '\0';
        return 0;
    }
    Q_strncpyz( ent->client->weaponSlots[slot].name, weapName,
                sizeof( ent->client->weaponSlots[slot].name ) );
    if ( BG_ParseWeaponDef( weapName, &wd ) ) {
        ent->client->weaponSlots[slot].clipAmmo    = wd.clipSize;
        ent->client->weaponSlots[slot].reserveAmmo = wd.startAmmo;
    }
    return 0;
}

/* self setWeaponSlotAmmo( slotName, ammo ) */
static int GScr_Meth_SetWeaponSlotAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 2 ) return 0;
    slotName = gsc_to_string( ctx, 0 );
    slot = G_WeaponSlotForName( slotName );
    if ( ent->client->weaponSlots[slot].name[0] )
        ent->client->weaponSlots[slot].reserveAmmo = gsc_to_int( ctx, 1 );
    return 0;
}

/* self setWeaponSlotClipAmmo( slotName, ammo ) */
static int GScr_Meth_SetWeaponSlotClipAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 2 ) return 0;
    slotName = gsc_to_string( ctx, 0 );
    slot = G_WeaponSlotForName( slotName );
    if ( ent->client->weaponSlots[slot].name[0] )
        ent->client->weaponSlots[slot].clipAmmo = gsc_to_int( ctx, 1 );
    return 0;
}

/* self setWeaponClipAmmo( weaponName, ammo ) */
static int GScr_Meth_SetWeaponClipAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *weapName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 2 ) return 0;
    weapName = gsc_to_string( ctx, 0 );
    slot = G_FindWeaponSlot( ent->client, weapName );
    if ( slot >= 0 )
        ent->client->weaponSlots[slot].clipAmmo = gsc_to_int( ctx, 1 );
    return 0;
}

/* self getWeaponSlotAmmo( slotName ) */
static int GScr_Meth_GetWeaponSlotAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }
    slotName = gsc_to_string( ctx, 0 );
    slot = G_WeaponSlotForName( slotName );
    gsc_add_int( ctx, ent->client->weaponSlots[slot].reserveAmmo );
    return 1;
}

/* self getWeaponSlotClipAmmo( slotName ) */
static int GScr_Meth_GetWeaponSlotClipAmmo( gsc_Context *ctx )
{
    gentity_t  *ent = G_Scr_GetSelf( ctx );
    const char *slotName;
    int         slot;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 1 ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }
    slotName = gsc_to_string( ctx, 0 );
    slot = G_WeaponSlotForName( slotName );
    gsc_add_int( ctx, ent->client->weaponSlots[slot].clipAmmo );
    return 1;
}

static int GScr_Meth_GetCurrentOffhand( gsc_Context *ctx )   { return G_Scr_NoopNoneString( ctx ); }
static int GScr_Meth_SwitchToOffhand( gsc_Context *ctx )     { return G_Scr_NoopReturn1( ctx ); }
static int GScr_Meth_SetClientDvar( gsc_Context *ctx )       { return GScr_Meth_SetClientCvar( ctx ); }
static int GScr_Meth_FreezeControls( gsc_Context *ctx )      { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_Shellshock( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    float      duration;

    if ( !ent || !ent->client || gsc_numargs( ctx ) < 2 ) return 0;
    /* arg 0: shellshock name (ignored for now, CoD1 has named presets) */
    duration = gsc_to_float( ctx, 1 );
    if ( duration < 0.0f ) duration = 0.0f;
    ent->client->ps.shellshockDuration = (int)( duration * 1000.0f );
    ent->client->ps.shellshockTime = level.time + ent->client->ps.shellshockDuration;
    return 0;
}
static int GScr_Meth_DisableWeapon( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_EnableWeapon( gsc_Context *ctx )        { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetEnterTime( gsc_Context *ctx )        { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PingPlayer( gsc_Context *ctx )          { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SayAll( gsc_Context *ctx )              { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SayTeam( gsc_Context *ctx )             { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_SetViewModel( gsc_Context *ctx )
{
    int         entNum;
    const char *viewModel;

    if ( !G_Scr_GetAttachStateForSelf( ctx, &entNum ) || gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    viewModel = G_Scr_GetOptionalStringArg( ctx, 0, "" );
    Q_strncpyz( g_scrViewModels[ entNum ], viewModel, sizeof( g_scrViewModels[ entNum ] ) );
    return 0;
}

static int GScr_Meth_GetViewModel( gsc_Context *ctx )
{
    int entNum;

    if ( !G_Scr_GetAttachStateForSelf( ctx, &entNum ) ||
         !g_scrViewModels[ entNum ][ 0 ] ) {
        gsc_add_string( ctx, "none" );
        return 1;
    }

    gsc_add_string( ctx, g_scrViewModels[ entNum ] );
    return 1;
}

static int GScr_Meth_Attach( gsc_Context *ctx )
{
    G_ScrAttachState_t *state;
    const char         *model;
    const char         *tag;
    int                 ignoreCollision;
    int                 i;

    state = G_Scr_GetAttachStateForSelf( ctx, NULL );
    if ( !state || gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    model = G_Scr_GetOptionalStringArg( ctx, 0, "" );
    if ( !model[ 0 ] ) {
        return 0;
    }

    tag = G_Scr_GetOptionalStringArg( ctx, 1, "" );
    ignoreCollision = G_Scr_GetIntArg( ctx, 2, 0 );

    for ( i = 0; i < state->count; i++ ) {
        if ( state->slot[ i ].inuse &&
             !Q_stricmp( state->slot[ i ].model, model ) &&
             !Q_stricmp( state->slot[ i ].tag, tag ) ) {
            state->slot[ i ].ignoreCollision = ignoreCollision;
            return 0;
        }
    }

    if ( state->count >= G_SCR_MAX_ATTACHMENTS ) {
        return 0;
    }

    state->slot[ state->count ].inuse = qtrue;
    Q_strncpyz( state->slot[ state->count ].model, model, sizeof( state->slot[ state->count ].model ) );
    Q_strncpyz( state->slot[ state->count ].tag, tag, sizeof( state->slot[ state->count ].tag ) );
    state->slot[ state->count ].ignoreCollision = ignoreCollision;
    state->count++;
    return 0;
}

static int GScr_Meth_Detach( gsc_Context *ctx )
{
    G_ScrAttachState_t *state;
    const char         *model;
    const char         *tag;
    qboolean            useTag;
    int                 i, j;

    state = G_Scr_GetAttachStateForSelf( ctx, NULL );
    if ( !state || gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    model = G_Scr_GetOptionalStringArg( ctx, 0, "" );
    if ( !model[ 0 ] ) {
        return 0;
    }

    tag = G_Scr_GetOptionalStringArg( ctx, 1, "" );
    useTag = tag[ 0 ] ? qtrue : qfalse;

    for ( i = 0; i < state->count; ) {
        qboolean matchModel = !Q_stricmp( state->slot[ i ].model, model );
        qboolean matchTag = !useTag || !Q_stricmp( state->slot[ i ].tag, tag );

        if ( matchModel && matchTag ) {
            for ( j = i; j + 1 < state->count; j++ ) {
                state->slot[ j ] = state->slot[ j + 1 ];
            }
            state->count--;
            Com_Memset( &state->slot[ state->count ], 0, sizeof( state->slot[ state->count ] ) );
            continue;
        }
        i++;
    }

    return 0;
}

static int GScr_Meth_DetachAll( gsc_Context *ctx )
{
    G_ScrAttachState_t *state = G_Scr_GetAttachStateForSelf( ctx, NULL );

    if ( state ) {
        Com_Memset( state, 0, sizeof( *state ) );
    }

    return 0;
}

static int GScr_Meth_GetAttachSize( gsc_Context *ctx )
{
    G_ScrAttachState_t *state = G_Scr_GetAttachStateForSelf( ctx, NULL );

    gsc_add_int( ctx, state ? state->count : 0 );
    return 1;
}

static int GScr_Meth_GetAttachModelName( gsc_Context *ctx )
{
    G_ScrAttachState_t *state = G_Scr_GetAttachStateForSelf( ctx, NULL );
    int                 idx = G_Scr_GetIntArg( ctx, 0, -1 );

    if ( !state || idx < 0 || idx >= state->count ) {
        gsc_add_string( ctx, "" );
        return 1;
    }

    gsc_add_string( ctx, state->slot[ idx ].model );
    return 1;
}

static int GScr_Meth_GetAttachTagName( gsc_Context *ctx )
{
    G_ScrAttachState_t *state = G_Scr_GetAttachStateForSelf( ctx, NULL );
    int                 idx = G_Scr_GetIntArg( ctx, 0, -1 );

    if ( !state || idx < 0 || idx >= state->count ) {
        gsc_add_string( ctx, "" );
        return 1;
    }

    gsc_add_string( ctx, state->slot[ idx ].tag );
    return 1;
}

static int GScr_Meth_GetAttachIgnoreCollision( gsc_Context *ctx )
{
    G_ScrAttachState_t *state = G_Scr_GetAttachStateForSelf( ctx, NULL );
    int                 idx = G_Scr_GetIntArg( ctx, 0, -1 );

    if ( !state || idx < 0 || idx >= state->count ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }

    gsc_add_int( ctx, state->slot[ idx ].ignoreCollision );
    return 1;
}

static int GScr_Meth_PlaySound( gsc_Context *ctx )           { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PlayLocalSound( gsc_Context *ctx )      { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_PlayLoopSound( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }
static int GScr_Meth_StopLoopSound( gsc_Context *ctx )       { return G_Scr_NoopReturn0( ctx ); }

static int GScr_Meth_ClonePlayer( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    if ( ent && ent->client ) {
        CopyToBodyQue( ent );
    }
    return 0;
}

static int GScr_Meth_DropItem( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

/*
 * CoD callback scripts call finishPlayerDamage(), but ioq3 already applies
 * damage before G_Scr_PlayerDamage is fired. Keep this as a no-op so damage
 * does not get applied twice.
 */
static int GScr_Meth_FinishPlayerDamage( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Meth_AllowSpectateTeam( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static void G_Scr_Hud_SanitizeServerString( const char *src, char *dst, int dstSize )
{
    int i, j;

    if ( !dst || dstSize <= 0 ) {
        return;
    }

    if ( !src ) {
        dst[0] = '\0';
        return;
    }

    for ( i = 0, j = 0; src[i] && j < dstSize - 1; i++ ) {
        unsigned char ch = (unsigned char)src[i];
        if ( ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' ) {
            dst[j++] = ' ';
            continue;
        }
        if ( ch < 32 ) {
            continue;
        }
        dst[j++] = (char)ch;
    }
    dst[j] = '\0';
}

static qboolean G_Scr_Hud_ClientMatches( const G_ScrHudElem_t *hud, int clientNum )
{
    if ( !hud || !hud->inuse ) {
        return qfalse;
    }

    if ( clientNum < 0 || clientNum >= level.maxclients ) {
        return qfalse;
    }

    if ( level.clients[ clientNum ].pers.connected != CON_CONNECTED ) {
        return qfalse;
    }

    switch ( hud->scope ) {
        case G_SCR_HUD_SCOPE_ALL:
            return qtrue;
        case G_SCR_HUD_SCOPE_CLIENT:
            return hud->owner == clientNum;
        case G_SCR_HUD_SCOPE_TEAM:
            return (int)level.clients[ clientNum ].sess.sessionTeam == hud->owner;
        default:
            return qfalse;
    }
}

static void G_Scr_Hud_SendUpdateToClient( const G_ScrHudElem_t *hud, int clientNum )
{
    char safeText[ sizeof( hud->text ) ];
    char safeShader[ sizeof( hud->shader ) ];

    if ( !hud ) {
        return;
    }

    G_Scr_Hud_SanitizeServerString( hud->text, safeText, sizeof( safeText ) );
    G_Scr_Hud_SanitizeServerString( hud->shader, safeShader, sizeof( safeShader ) );

    trap_SendServerCommand(
        clientNum,
        va( "scr_hud set %d %.3f %.3f %.3f %.3f %.3f %.3f %.3f %d %d \"%s\" \"%s\"",
            hud->id,
            hud->x,
            hud->y,
            hud->scale,
            hud->color[0], hud->color[1], hud->color[2], hud->color[3],
            hud->width, hud->height,
            safeText, safeShader ) );
}

static void G_Scr_Hud_SendUpdate( const G_ScrHudElem_t *hud )
{
    int i;

    if ( !hud || !hud->inuse ) {
        return;
    }

    if ( hud->scope == G_SCR_HUD_SCOPE_ALL ) {
        G_Scr_Hud_SendUpdateToClient( hud, -1 );
        return;
    }

    for ( i = 0; i < level.maxclients; i++ ) {
        if ( G_Scr_Hud_ClientMatches( hud, i ) ) {
            G_Scr_Hud_SendUpdateToClient( hud, i );
        }
    }
}

static void G_Scr_Hud_SendDelete( const G_ScrHudElem_t *hud )
{
    int i;

    if ( !hud ) {
        return;
    }

    if ( hud->scope == G_SCR_HUD_SCOPE_ALL ) {
        trap_SendServerCommand( -1, va( "scr_hud del %d", hud->id ) );
        return;
    }

    for ( i = 0; i < level.maxclients; i++ ) {
        if ( G_Scr_Hud_ClientMatches( hud, i ) ) {
            trap_SendServerCommand( i, va( "scr_hud del %d", hud->id ) );
        }
    }
}

static void G_Scr_Hud_SendAllToClient( int clientNum )
{
    int i;

    if ( clientNum < 0 || clientNum >= level.maxclients ) {
        return;
    }

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( G_Scr_Hud_ClientMatches( hud, clientNum ) ) {
            G_Scr_Hud_SendUpdateToClient( hud, clientNum );
        }
    }
}

static void G_Scr_Hud_ResetAll( qboolean broadcast )
{
    Com_Memset( g_scrHudElems, 0, sizeof( g_scrHudElems ) );
    if ( broadcast ) {
        trap_SendServerCommand( -1, "scr_hud reset" );
    }
}

static G_ScrHudElem_t *G_Scr_Hud_FindById( int id )
{
    if ( id < 1 || id > G_SCR_MAX_HUDELEMS ) {
        return NULL;
    }

    if ( !g_scrHudElems[ id - 1 ].inuse ) {
        return NULL;
    }

    return &g_scrHudElems[ id - 1 ];
}

static G_ScrHudElem_t *G_Scr_Hud_FromSelf( gsc_Context *ctx, int *outSelfObjIdx )
{
    int             selfObjIdx;
    intptr_t        rawId;
    G_ScrHudElem_t *hud;

    selfObjIdx = gsc_get_object( ctx, -1 );
    if ( selfObjIdx < 0 ) {
        return NULL;
    }

    rawId = (intptr_t)gsc_object_get_userdata( ctx, selfObjIdx );
    if ( rawId <= 0 ) {
        return NULL;
    }

    hud = G_Scr_Hud_FindById( (int)rawId );
    if ( !hud ) {
        return NULL;
    }

    if ( outSelfObjIdx ) {
        *outSelfObjIdx = selfObjIdx;
    }

    return hud;
}

static void G_Scr_Hud_ReadFloatField( gsc_Context *ctx, int objIdx, const char *name, float *value )
{
    gsc_object_get_field( ctx, objIdx, name );
    if ( gsc_type( ctx, -1 ) != GSC_TYPE_UNDEFINED ) {
        *value = gsc_to_float( ctx, -1 );
    }
    gsc_pop( ctx, 1 );
}

static void G_Scr_Hud_ReadVec3Field( gsc_Context *ctx, int objIdx, const char *name, float out[3] )
{
    gsc_object_get_field( ctx, objIdx, name );
    if ( gsc_type( ctx, -1 ) == GSC_TYPE_VECTOR ) {
        float       x, y, z;
        const char *vecStr = gsc_to_string( ctx, -1 );
        if ( vecStr &&
             ( sscanf( vecStr, " ( %f , %f , %f ) ", &x, &y, &z ) == 3 ||
               sscanf( vecStr, "(%f,%f,%f)", &x, &y, &z ) == 3 ) ) {
            out[0] = x;
            out[1] = y;
            out[2] = z;
        }
    }
    gsc_pop( ctx, 1 );
}

static void G_Scr_Hud_ReadCommonFields( gsc_Context *ctx, int selfObjIdx, G_ScrHudElem_t *hud )
{
    float dim;

    if ( !hud ) {
        return;
    }

    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "x", &hud->x );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "y", &hud->y );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "fontscale", &hud->scale );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "scale", &hud->scale );
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "alpha", &hud->color[3] );
    G_Scr_Hud_ReadVec3Field( ctx, selfObjIdx, "color", hud->color );

    dim = (float)hud->width;
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "width", &dim );
    hud->width = (int)dim;
    dim = (float)hud->height;
    G_Scr_Hud_ReadFloatField( ctx, selfObjIdx, "height", &dim );
    hud->height = (int)dim;

    if ( hud->scale <= 0.0f ) {
        hud->scale = 0.35f;
    }
    if ( hud->color[3] < 0.0f ) {
        hud->color[3] = 0.0f;
    } else if ( hud->color[3] > 1.0f ) {
        hud->color[3] = 1.0f;
    }
}

static void G_Scr_Hud_ClearTimer( G_ScrHudElem_t *hud )
{
    if ( !hud ) {
        return;
    }
    hud->timerMode = G_SCR_HUD_TIMER_NONE;
    hud->timerBaseMs = 0;
    hud->timerStartMs = 0;
    hud->timerLastValue = -1;
}

static void G_Scr_Hud_FormatClockSeconds( int totalSeconds, char *out, int outSize )
{
    int minutes;
    int seconds;

    if ( totalSeconds < 0 ) {
        totalSeconds = 0;
    }

    minutes = totalSeconds / 60;
    seconds = totalSeconds % 60;
    Com_sprintf( out, outSize, "%d:%02d", minutes, seconds );
}

static void G_Scr_Hud_FormatClockTenths( int totalTenths, char *out, int outSize )
{
    int totalSeconds;
    int minutes;
    int seconds;
    int tenths;

    if ( totalTenths < 0 ) {
        totalTenths = 0;
    }

    totalSeconds = totalTenths / 10;
    tenths = totalTenths % 10;
    minutes = totalSeconds / 60;
    seconds = totalSeconds % 60;
    Com_sprintf( out, outSize, "%d:%02d.%d", minutes, seconds, tenths );
}

static qboolean G_Scr_Hud_UpdateTimerText( G_ScrHudElem_t *hud, qboolean forceSend )
{
    int elapsedMs;
    int displayValue;

    if ( !hud || !hud->inuse || hud->timerMode == G_SCR_HUD_TIMER_NONE ) {
        return qfalse;
    }

    elapsedMs = level.time - hud->timerStartMs;
    if ( elapsedMs < 0 ) {
        elapsedMs = 0;
    }

    switch ( hud->timerMode ) {
        case G_SCR_HUD_TIMER_DOWN:
        {
            int remainingMs = hud->timerBaseMs - elapsedMs;
            if ( remainingMs < 0 ) {
                remainingMs = 0;
            }
            displayValue = ( remainingMs + 999 ) / 1000; /* ceil */
            if ( !forceSend && displayValue == hud->timerLastValue ) {
                return qfalse;
            }
            hud->timerLastValue = displayValue;
            G_Scr_Hud_FormatClockSeconds( displayValue, hud->text, sizeof( hud->text ) );
            G_Scr_Hud_SendUpdate( hud );
            return qtrue;
        }

        case G_SCR_HUD_TIMER_UP:
            displayValue = ( hud->timerBaseMs + elapsedMs ) / 1000; /* floor */
            if ( !forceSend && displayValue == hud->timerLastValue ) {
                return qfalse;
            }
            hud->timerLastValue = displayValue;
            G_Scr_Hud_FormatClockSeconds( displayValue, hud->text, sizeof( hud->text ) );
            G_Scr_Hud_SendUpdate( hud );
            return qtrue;

        case G_SCR_HUD_TIMER_TENTHS_DOWN:
        {
            int remainingMs = hud->timerBaseMs - elapsedMs;
            if ( remainingMs < 0 ) {
                remainingMs = 0;
            }
            displayValue = ( remainingMs + 99 ) / 100; /* ceil to tenths */
            if ( !forceSend && displayValue == hud->timerLastValue ) {
                return qfalse;
            }
            hud->timerLastValue = displayValue;
            G_Scr_Hud_FormatClockTenths( displayValue, hud->text, sizeof( hud->text ) );
            G_Scr_Hud_SendUpdate( hud );
            return qtrue;
        }

        default:
            return qfalse;
    }
}

static void G_Scr_Hud_UpdateTimers( void )
{
    int i;

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( !hud->inuse || hud->timerMode == G_SCR_HUD_TIMER_NONE ) {
            continue;
        }
        G_Scr_Hud_UpdateTimerText( hud, qfalse );
    }
}

static int GScr_Meth_Hud_SetText( gsc_Context *ctx )
{
    int             selfObjIdx;
    const char     *text;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    text = ( gsc_numargs( ctx ) > 0 ) ? gsc_get_string( ctx, 0 ) : "";
    Q_strncpyz( hud->text, text ? text : "", sizeof( hud->text ) );
    G_Scr_Hud_ClearTimer( hud );
    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_SetShader( gsc_Context *ctx )
{
    int             selfObjIdx;
    const char     *shader;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    shader = ( gsc_numargs( ctx ) > 0 ) ? gsc_get_string( ctx, 0 ) : "";
    Q_strncpyz( hud->shader, shader ? shader : "", sizeof( hud->shader ) );

    if ( gsc_numargs( ctx ) > 1 ) {
        hud->width = (int)gsc_get_float( ctx, 1 );
    }
    if ( gsc_numargs( ctx ) > 2 ) {
        hud->height = (int)gsc_get_float( ctx, 2 );
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_SetTimer( gsc_Context *ctx )
{
    int             selfObjIdx;
    float           seconds = 0.0f;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 0 ) {
        seconds = gsc_get_float( ctx, 0 );
    }
    if ( seconds < 0.0f ) {
        seconds = 0.0f;
    }

    hud->timerMode = G_SCR_HUD_TIMER_DOWN;
    hud->timerBaseMs = (int)( seconds * 1000.0f + 0.5f );
    hud->timerStartMs = level.time;
    hud->timerLastValue = -1;

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_UpdateTimerText( hud, qtrue );
    return 0;
}

static int GScr_Meth_Hud_SetTimerUp( gsc_Context *ctx )
{
    int             selfObjIdx;
    float           seconds = 0.0f;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 0 ) {
        seconds = gsc_get_float( ctx, 0 );
    }
    if ( seconds < 0.0f ) {
        seconds = 0.0f;
    }

    hud->timerMode = G_SCR_HUD_TIMER_UP;
    hud->timerBaseMs = (int)( seconds * 1000.0f + 0.5f );
    hud->timerStartMs = level.time;
    hud->timerLastValue = -1;

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_UpdateTimerText( hud, qtrue );
    return 0;
}

static int GScr_Meth_Hud_SetTenthsTimer( gsc_Context *ctx )
{
    int             selfObjIdx;
    float           seconds = 0.0f;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 0 ) {
        seconds = gsc_get_float( ctx, 0 );
    }
    if ( seconds < 0.0f ) {
        seconds = 0.0f;
    }

    hud->timerMode = G_SCR_HUD_TIMER_TENTHS_DOWN;
    hud->timerBaseMs = (int)( seconds * 1000.0f + 0.5f );
    hud->timerStartMs = level.time;
    hud->timerLastValue = -1;

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_UpdateTimerText( hud, qtrue );
    return 0;
}

static int GScr_Meth_Hud_SetValue( gsc_Context *ctx )
{
    int             selfObjIdx;
    int             value = 0;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 0 ) {
        if ( gsc_get_type( ctx, 0 ) == GSC_TYPE_FLOAT ) {
            value = (int)gsc_get_float( ctx, 0 );
        } else {
            value = (int)gsc_get_int( ctx, 0 );
        }
        Com_sprintf( hud->text, sizeof( hud->text ), "%d", value );
    } else {
        hud->text[0] = '\0';
    }

    G_Scr_Hud_ClearTimer( hud );
    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_FadeOverTime( gsc_Context *ctx )
{
    int             selfObjIdx;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_MoveOverTime( gsc_Context *ctx )
{
    int             selfObjIdx;
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, &selfObjIdx );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_ReadCommonFields( ctx, selfObjIdx, hud );
    G_Scr_Hud_SendUpdate( hud );
    return 0;
}

static int GScr_Meth_Hud_Destroy( gsc_Context *ctx )
{
    G_ScrHudElem_t *hud = G_Scr_Hud_FromSelf( ctx, NULL );

    if ( !hud ) {
        return 0;
    }

    G_Scr_Hud_SendDelete( hud );
    Com_Memset( hud, 0, sizeof( *hud ) );
    return 0;
}

/* =========================================================================
   Global / free functions available to scripts
   ========================================================================= */

static const char *G_Scr_StringifyArg( gsc_Context *ctx, int arg, char *buf, int bufSize )
{
    switch ( gsc_get_type( ctx, arg ) ) {
        case GSC_TYPE_UNDEFINED:
            return "undefined";
        case GSC_TYPE_BOOLEAN:
            return gsc_get_bool( ctx, arg ) ? "1" : "0";
        case GSC_TYPE_FLOAT:
            Com_sprintf( buf, bufSize, "%.3f", gsc_get_float( ctx, arg ) );
            return buf;
        case GSC_TYPE_INTEGER:
            Com_sprintf( buf, bufSize, "%d", (int)gsc_get_int( ctx, arg ) );
            return buf;
        case GSC_TYPE_STRING:
        case GSC_TYPE_INTERNED_STRING:
            return gsc_get_string( ctx, arg );
        case GSC_TYPE_VECTOR:
        {
            float v[ 3 ];
            gsc_get_vec3( ctx, arg, v );
            Com_sprintf( buf, bufSize, "(%.3f, %.3f, %.3f)", v[ 0 ], v[ 1 ], v[ 2 ] );
            return buf;
        }
        case GSC_TYPE_FUNCTION:
            return "[function]";
        case GSC_TYPE_OBJECT:
            return "[object]";
        case GSC_TYPE_REFERENCE:
            return "[reference]";
        case GSC_TYPE_THREAD:
            return "[thread]";
        default:
            break;
    }

    return "<unknown>";
}

static int GScr_Fn_Print( gsc_Context *ctx )
{
    int  i, n = gsc_numargs( ctx );
    char buf[ 128 ];

    for ( i = 0; i < n; i++ ) {
        G_Printf( "%s", G_Scr_StringifyArg( ctx, i, buf, sizeof( buf ) ) );
    }
    G_Printf( "\n" );
    return 0;
}

/* Helper: build message from GSC args */
static void G_Scr_BuildMessage( gsc_Context *ctx, char *buffer, int bufferSize )
{
    int  i, n = gsc_numargs( ctx );
    char buf[ 256 ];
    int  offset = 0;

    buffer[ 0 ] = '\0';
    for ( i = 0; i < n && offset < bufferSize - 1; i++ ) {
        const char *arg = G_Scr_StringifyArg( ctx, i, buf, sizeof( buf ) );
        Q_strncpyz( buffer + offset, arg, bufferSize - offset );
        offset += strlen( buffer + offset );
    }
}

/* iprintln — display on screen left bottom for all clients (CoD1) */
static int GScr_Fn_IPrintLn( gsc_Context *ctx )
{
    char msg[ MAX_STRING_CHARS ];

    G_Scr_BuildMessage( ctx, msg, sizeof( msg ) );
    /* CoD1 uses 'f' command for left-bottom messages */
    trap_SendServerCommand( -1, va( "f \"%s\"\n", msg ) );
    return 0;
}

/* iprintlnbold — display on screen center for all clients (CoD1) */
static int GScr_Fn_IPrintLnBold( gsc_Context *ctx )
{
    char msg[ MAX_STRING_CHARS ];

    G_Scr_BuildMessage( ctx, msg, sizeof( msg ) );
    /* CoD1 uses 'g' command for center messages */
    trap_SendServerCommand( -1, va( "g \"%s\"\n", msg ) );
    return 0;
}

/* Player method: iprintln — send message to specific player (CoD1) */
static int GScr_Meth_Player_IPrintLn( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    char      msg[ MAX_STRING_CHARS ];

    if ( !ent || !ent->client ) {
        return 0;
    }

    G_Scr_BuildMessage( ctx, msg, sizeof( msg ) );
    trap_SendServerCommand( ent - g_entities, va( "f \"%s\"\n", msg ) );
    return 0;
}

/* Player method: iprintlnbold — send bold message to specific player (CoD1) */
static int GScr_Meth_Player_IPrintLnBold( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );
    char      msg[ MAX_STRING_CHARS ];

    if ( !ent || !ent->client ) {
        return 0;
    }

    G_Scr_BuildMessage( ctx, msg, sizeof( msg ) );
    trap_SendServerCommand( ent - g_entities, va( "g \"%s\"\n", msg ) );
    return 0;
}

static int GScr_Fn_IsDefined( gsc_Context *ctx )
{
    gsc_add_bool( ctx, gsc_get_type( ctx, 0 ) != GSC_TYPE_UNDEFINED );
    return 1;
}

static int GScr_Fn_RandomInt( gsc_Context *ctx )
{
    int maxVal = (int)gsc_get_int( ctx, 0 );
    if ( maxVal <= 0 ) {
        gsc_add_int( ctx, 0 );
        return 1;
    }
    gsc_add_int( ctx, rand() % maxVal );
    return 1;
}

static int GScr_Fn_RandomFloat( gsc_Context *ctx )
{
    float maxVal = gsc_get_float( ctx, 0 );
    if ( maxVal <= 0.0f ) {
        gsc_add_float( ctx, 0.0f );
        return 1;
    }
    gsc_add_float( ctx, ( (float)rand() / (float)RAND_MAX ) * maxVal );
    return 1;
}

static int GScr_Fn_GetDvar( gsc_Context *ctx )
{
    char        buf[ 256 ];
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    trap_Cvar_VariableStringBuffer( name, buf, sizeof( buf ) );
    gsc_add_string( ctx, buf );
    return 1;
}

static int GScr_Fn_GetDvarInt( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    gsc_add_int( ctx, trap_Cvar_VariableIntegerValue( name ) );
    return 1;
}

static int GScr_Fn_SetDvar( gsc_Context *ctx )
{
    const char *name  = gsc_get_string( ctx, 0 );
    const char *value = gsc_get_string( ctx, 1 );
    if ( name && value ) {
        trap_Cvar_Set( name, value );
    }
    return 0;
}

static int GScr_Fn_GetDvarFloat( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    if ( !name ) name = "";
    gsc_add_float( ctx, trap_Cvar_VariableValue( name ) );
    return 1;
}

static int GScr_Fn_MakeCvarServerInfo( gsc_Context *ctx )
{
    const char *name = gsc_get_string( ctx, 0 );
    const char *defaultValue = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "";
    char        cur[ 256 ];

    if ( name && name[0] ) {
        trap_Cvar_VariableStringBuffer( name, cur, sizeof( cur ) );
        if ( !cur[0] ) {
            trap_Cvar_Set( name, defaultValue ? defaultValue : "" );
        }
    }
    return 0;
}

static int GScr_Fn_SetArchive( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

/*
 * resetTimeout() — CoD builtin used by MP gametypes during spawn state changes.
 * In this gamecode, map it to refreshing the inactivity drop timer for self.
 */
static int GScr_Fn_ResetTimeout( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetSelf( ctx );

    if ( ent && ent->client ) {
        if ( g_inactivity.integer > 0 ) {
            ent->client->inactivityTime = level.time + g_inactivity.integer * 1000;
        } else {
            ent->client->inactivityTime = level.time + 60 * 1000;
        }
        ent->client->inactivityWarning = qfalse;
    }

    gsc_add_int( ctx, level.time );
    return 1;
}

static int GScr_Fn_LogPrint( gsc_Context *ctx )
{
    int  i, n = gsc_numargs( ctx );
    char buf[ 128 ];

    for ( i = 0; i < n; i++ ) {
        G_LogPrintf( "%s", G_Scr_StringifyArg( ctx, i, buf, sizeof( buf ) ) );
    }
    return 0;
}

static int GScr_Fn_IsPlayerGlobal( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx, ent && ent->client &&
                  ent->client->pers.connected != CON_DISCONNECTED );
    return 1;
}

static int GScr_Fn_IsAliveGlobal( gsc_Context *ctx )
{
    gentity_t *ent = G_Scr_GetEntityFromArg( ctx, 0 );
    gsc_add_bool( ctx, ent && ent->health > 0 );
    return 1;
}

static int G_Scr_Hud_ParseTeamArg( gsc_Context *ctx, int arg )
{
    int         type;
    const char *team;

    if ( gsc_numargs( ctx ) <= arg ) {
        return TEAM_FREE;
    }

    type = gsc_get_type( ctx, arg );
    if ( type == GSC_TYPE_INTEGER ) {
        int teamNum = (int)gsc_get_int( ctx, arg );
        if ( teamNum >= TEAM_FREE && teamNum <= TEAM_SPECTATOR ) {
            return teamNum;
        }
        return TEAM_FREE;
    }

    team = gsc_get_string( ctx, arg );
    if ( !team || !team[0] ) {
        return TEAM_FREE;
    }

    if ( !Q_stricmp( team, "allies" ) || !Q_stricmp( team, "red" ) ) {
        return TEAM_RED;
    }
    if ( !Q_stricmp( team, "axis" ) || !Q_stricmp( team, "blue" ) ) {
        return TEAM_BLUE;
    }
    if ( !Q_stricmp( team, "spectator" ) ) {
        return TEAM_SPECTATOR;
    }
    return TEAM_FREE;
}

static int GScr_Fn_NewHudElem( gsc_Context *ctx )
{
    int topBefore = gsc_top( ctx );
    G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_ALL, -1 );
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_NewClientHudElem( gsc_Context *ctx )
{
    int       topBefore = gsc_top( ctx );
    int       clientNum = -1;
    gentity_t *ownerEnt = NULL;

    if ( gsc_numargs( ctx ) > 0 ) {
        ownerEnt = G_Scr_GetEntityFromArg( ctx, 0 );
    }
    if ( !ownerEnt ) {
        ownerEnt = G_Scr_GetSelf( ctx );
    }

    if ( G_Scr_GetClientNumForEntity( ownerEnt, &clientNum ) ) {
        G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_CLIENT, clientNum );
    } else {
        /* CoD scripts may call this without owner during setup; keep it non-fatal. */
        G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_ALL, -1 );
    }
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_NewTeamHudElem( gsc_Context *ctx )
{
    int topBefore = gsc_top( ctx );
    int team = G_Scr_Hud_ParseTeamArg( ctx, 0 );
    G_Scr_PushHudElemObject( ctx, G_SCR_HUD_SCOPE_TEAM, team );
    return ( gsc_top( ctx ) > topBefore ) ? 1 : 0;
}

static int GScr_Fn_PositionWouldTelefrag( gsc_Context *ctx )
{
    trace_t tr;
    vec3_t  origin;
    vec3_t  mins = { -15.0f, -15.0f, -24.0f };
    vec3_t  maxs = {  15.0f,  15.0f,  32.0f };

    gsc_get_vec3( ctx, 0, origin );
    trap_Trace( &tr, origin, mins, maxs, origin, ENTITYNUM_NONE, MASK_PLAYERSOLID );
    gsc_add_bool( ctx, tr.startsolid || tr.allsolid );
    return 1;
}

static int GScr_Fn_Distance( gsc_Context *ctx )
{
    vec3_t a, b, d;
    gsc_get_vec3( ctx, 0, a );
    gsc_get_vec3( ctx, 1, b );
    VectorSubtract( a, b, d );
    gsc_add_float( ctx, VectorLength( d ) );
    return 1;
}

static int GScr_Fn_Length( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    gsc_add_float( ctx, VectorLength( v ) );
    return 1;
}

static int GScr_Fn_LengthSquared( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    gsc_add_float( ctx, DotProduct( v, v ) );
    return 1;
}

static int GScr_Fn_VectorNormalize( gsc_Context *ctx )
{
    vec3_t v;
    gsc_get_vec3( ctx, 0, v );
    VectorNormalize( v );
    gsc_add_vec3( ctx, v );
    return 1;
}

static int GScr_Fn_VectorScale( gsc_Context *ctx )
{
    vec3_t v, out;
    float  s = gsc_get_float( ctx, 1 );
    gsc_get_vec3( ctx, 0, v );
    VectorScale( v, s, out );
    gsc_add_vec3( ctx, out );
    return 1;
}

static int GScr_Fn_VectorToAngles( gsc_Context *ctx )
{
    vec3_t v, a;
    gsc_get_vec3( ctx, 0, v );
    vectoangles( v, a );
    gsc_add_vec3( ctx, a );
    return 1;
}

static int GScr_Fn_AnglesToForward( gsc_Context *ctx )
{
    vec3_t a, fwd;
    gsc_get_vec3( ctx, 0, a );
    AngleVectors( a, fwd, NULL, NULL );
    gsc_add_vec3( ctx, fwd );
    return 1;
}

static int GScr_Fn_SpawnStruct( gsc_Context *ctx )
{
    (void)ctx;
    gsc_add_object( ctx );
    return 1;
}

static int GScr_Fn_Spawn( gsc_Context *ctx )
{
    const char *classname = gsc_get_string( ctx, 0 );
    vec3_t      origin = { 0.0f, 0.0f, 0.0f };
    gentity_t  *ent = G_Spawn();

    if ( !ent ) {
        return 0;
    }

    if ( gsc_numargs( ctx ) > 1 && gsc_get_type( ctx, 1 ) == GSC_TYPE_VECTOR ) {
        gsc_get_vec3( ctx, 1, origin );
    }

    if ( classname && classname[0] ) {
        ent->classname = G_Scr_CopyString( classname );
    } else {
        ent->classname = G_Scr_CopyString( "script_model" );
    }

    VectorCopy( origin, ent->s.origin );
    VectorCopy( origin, ent->s.pos.trBase );
    VectorCopy( origin, ent->r.currentOrigin );

    if ( !G_CallSpawn( ent ) ) {
        trap_LinkEntity( ent );
    }

    if ( !ent->inuse ) {
        return 0;
    }

    G_Scr_PushEntityObject( ctx, ent );
    return 1;
}

static int GScr_Fn_MapRestart( gsc_Context *ctx )
{
    (void)ctx;
    trap_SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
    return 0;
}

static int GScr_Fn_SetClientNameMode( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Fn_Obituary( gsc_Context *ctx )
{
    (void)ctx;
    return 0;
}

static int GScr_Fn_MapExists( gsc_Context *ctx )
{
    char        path[ MAX_QPATH ];
    char        mapName[ MAX_QPATH ];
    fileHandle_t fh;
    int          len;
    const char  *name = gsc_get_string( ctx, 0 );

    if ( !name || !name[0] ) {
        gsc_add_bool( ctx, qfalse );
        return 1;
    }

    G_Scr_NormalizeMapName( name, mapName, sizeof( mapName ) );
    Com_sprintf( path, sizeof( path ), "maps/%s.bsp", mapName );

    len = trap_FS_FOpenFile( path, &fh, FS_READ );
    if ( fh ) {
        trap_FS_FCloseFile( fh );
    }

    if ( len <= 0 ) {
        Com_sprintf( path, sizeof( path ), "maps/mp/%s.bsp", mapName );
        len = trap_FS_FOpenFile( path, &fh, FS_READ );
        if ( fh ) {
            trap_FS_FCloseFile( fh );
        }
    }

    gsc_add_bool( ctx, len > 0 );
    return 1;
}

static int GScr_Fn_Announcement( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_ClientAnnouncement( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_UpdateScores( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_AddTestClient( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheMenu( gsc_Context *ctx )
{
#ifdef STANDALONE
    const char *menuName = gsc_get_string( ctx, 0 );
    char existing[ MAX_QPATH ];
    int i;

    if ( !menuName || !menuName[0] ) return 0;

    /* Check if already precached */
    for ( i = 0; i < MAX_SCRIPT_MENUS; i++ ) {
        trap_GetConfigstring( CS_SCRIPT_MENUS + i, existing, sizeof( existing ) );
        if ( !Q_stricmp( existing, menuName ) ) return 0;
    }

    /* Find first empty slot */
    for ( i = 0; i < MAX_SCRIPT_MENUS; i++ ) {
        trap_GetConfigstring( CS_SCRIPT_MENUS + i, existing, sizeof( existing ) );
        if ( !existing[0] ) {
            trap_SetConfigstring( CS_SCRIPT_MENUS + i, menuName );
            return 0;
        }
    }

    G_Printf( "GSC: precacheMenu overflow (max %d)\n", MAX_SCRIPT_MENUS );
#else
    (void)ctx;
#endif
    return 0;
}
static int GScr_Fn_PrecacheStatusIcon( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheHeadIcon( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheItem( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheModel( gsc_Context *ctx )
{
    const char *model = gsc_get_string( ctx, 0 );

    if ( model && model[0] ) {
        G_ModelIndex( (char *)model );
    }
    return 0;
}
static int GScr_Fn_PrecacheShader( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheString( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PrecacheTurret( gsc_Context *ctx ) { (void)ctx; return 0; }

static int G_Scr_FindFxHandle( const char *path )
{
    int i;

    if ( !path || !path[0] ) {
        return 0;
    }

    for ( i = 0; i < g_scrFxCount; i++ ) {
        if ( !Q_stricmp( g_scrFxNames[ i ], path ) ) {
            return i + 1;
        }
    }

    if ( g_scrFxCount >= G_SCR_MAX_FX ) {
        return 0;
    }

    Q_strncpyz( g_scrFxNames[ g_scrFxCount ], path, sizeof( g_scrFxNames[ g_scrFxCount ] ) );
    g_scrFxCount++;
    return g_scrFxCount;
}

/* CoD FX compatibility: keep stable effect handles for script logic. */
static int GScr_Fn_LoadFx( gsc_Context *ctx )
{
    const char *path = ( gsc_numargs( ctx ) > 0 ) ? gsc_get_string( ctx, 0 ) : "";
    int         fxId = G_Scr_FindFxHandle( path );

    gsc_add_int( ctx, fxId );
    return 1;
}

/* Server has no renderer-side FX playback; keep as non-fatal no-op. */
static int GScr_Fn_PlayFx( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_PlayFxOnTag( gsc_Context *ctx ) { (void)ctx; return 0; }

/* gettime() — returns level time in seconds */
static int GScr_Fn_GetTime( gsc_Context *ctx )
{
    gsc_add_float( ctx, (float)level.time * 0.001f );
    return 1;
}

/* getent() — supports getent(clientNum) and getent(value, key) */
static int GScr_Fn_GetEnt( gsc_Context *ctx )
{
    char globalName[ 32 ];
    int  clientNum;
    int  topBefore;
    int  i;
    const char *value;
    const char *key;

    if ( gsc_get_type( ctx, 0 ) == GSC_TYPE_INTEGER ) {
        clientNum = (int)gsc_get_int( ctx, 0 );
        if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
            return 0; /* undefined */
        }

        if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
            G_Scr_CreatePlayerObj( clientNum );
        }

        topBefore = gsc_top( g_scrCtx );
        if ( g_scrPlayerRefs[ clientNum ] != GSC_NOREF ) {
            gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ clientNum ] );
        }
        if ( gsc_top( g_scrCtx ) <= topBefore || gsc_type( g_scrCtx, -1 ) != GSC_TYPE_OBJECT ) {
            /* Fallback: retrieve via the named global */
            Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
            gsc_get_global( g_scrCtx, globalName );
            if ( gsc_type( g_scrCtx, -1 ) != GSC_TYPE_OBJECT ) {
                gsc_pop( g_scrCtx, 1 );
                return 0;
            }
        }
        return 1;
    }

    if ( gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    value = gsc_get_string( ctx, 0 );
    key = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "targetname";

    for ( i = 0; i < level.num_entities; i++ ) {
        gentity_t *ent = &g_entities[ i ];
        if ( G_Scr_EntityMatchesFilter( ent, value, key ) ) {
            G_Scr_PushEntityObject( ctx, ent );
            return 1;
        }
    }

    return 0; /* undefined */
}

/* getentarray(value, key) — returns array-like object of matching entities */
static int GScr_Fn_GetEntArray( gsc_Context *ctx )
{
    int arrObj;
    int i;
    int count = 0;
    char idxKey[ 32 ];
    const char *value = NULL;
    const char *key = "classname";
    qboolean filter = qfalse;

    arrObj = gsc_add_object( ctx );

    if ( gsc_numargs( ctx ) > 0 ) {
        value = gsc_get_string( ctx, 0 );
        key = ( gsc_numargs( ctx ) > 1 ) ? gsc_get_string( ctx, 1 ) : "classname";
        filter = ( value && value[0] );
    }

    for ( i = 0; i < level.num_entities; i++ ) {
        gentity_t *ent = &g_entities[ i ];
        if ( !ent->inuse ) {
            continue;
        }
        if ( filter && !G_Scr_EntityMatchesFilter( ent, value, key ) ) {
            continue;
        }

        G_Scr_PushEntityObject( ctx, ent );
        Com_sprintf( idxKey, sizeof( idxKey ), "%d", count );
        gsc_object_set_field( ctx, arrObj, idxKey );
        count++;
    }
    return 1;
}

/* exitLevel(bool) — CoD1: end the map; we just trigger a vote_nextmap */
static int GScr_Fn_ExitLevel( gsc_Context *ctx )
{
    (void)ctx;
    trap_SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
    return 0;
}

/* setcvar(name, value) — alias for setDvar, used in _callbacksetup */
static int GScr_Fn_SetCvar( gsc_Context *ctx )
{
    const char *name  = gsc_get_string( ctx, 0 );
    const char *value = gsc_get_string( ctx, 1 );
    if ( name && value ) {
        trap_Cvar_Set( name, value );
    }
    return 0;
}

/* getCvar(name) — alias for getDvar */
static int GScr_Fn_GetCvar( gsc_Context *ctx )
{
    return GScr_Fn_GetDvar( ctx );
}

/* ambientPlay / ambientStop — sound stubs (not yet implemented) */
static int GScr_Fn_AmbientPlay( gsc_Context *ctx ) { (void)ctx; return 0; }
static int GScr_Fn_AmbientStop( gsc_Context *ctx ) { (void)ctx; return 0; }

/* setCullFog — client-side, no-op on server */
static int GScr_Fn_SetCullFog( gsc_Context *ctx ) { (void)ctx; return 0; }

/* =========================================================================
   Team score functions for CoD1 GSC compatibility
   ========================================================================= */

/* Helper: convert team string to team_t (reuses existing G_Scr_StringToTeam logic) */
static team_t G_Scr_ParseTeamArg( gsc_Context *ctx, int argIdx )
{
    const char *team;
    if ( gsc_numargs( ctx ) <= argIdx ) {
        return TEAM_FREE;
    }
    team = gsc_get_string( ctx, argIdx );
    if ( !Q_stricmp( team, "allies" ) || !Q_stricmp( team, "red" ) ) {
        return TEAM_RED;
    }
    if ( !Q_stricmp( team, "axis" ) || !Q_stricmp( team, "blue" ) ) {
        return TEAM_BLUE;
    }
    return TEAM_FREE;
}

/* setTeamScore(team, score) — directly set team score (CoD1) */
static int GScr_Fn_SetTeamScore( gsc_Context *ctx )
{
    team_t team;
    int    score;

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    team  = G_Scr_ParseTeamArg( ctx, 0 );
    score = (int)gsc_get_int( ctx, 1 );

    if ( team >= TEAM_RED && team < TEAM_NUM_TEAMS ) {
        level.teamScores[ team ] = score;
        CalculateRanks();
    }
    return 0;
}

/* getTeamScore(team) — returns current team score (CoD1) */
static int GScr_Fn_GetTeamScore( gsc_Context *ctx )
{
    team_t team = G_Scr_ParseTeamArg( ctx, 0 );

    if ( team >= TEAM_RED && team < TEAM_NUM_TEAMS ) {
        gsc_add_int( ctx, level.teamScores[ team ] );
    } else {
        gsc_add_int( ctx, 0 );
    }
    return 1;
}

/* =========================================================================
   Objective system for CoD1 GSC compatibility
   ========================================================================= */

/* objective_add(id, type, ...) — stub: objective tracking not fully implemented */
static int GScr_Fn_Objective_Add( gsc_Context *ctx )
{
    int objId;

    if ( gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    objId = (int)gsc_get_int( ctx, 0 );
    if ( objId < 0 || objId >= G_SCR_MAX_OBJECTIVES ) {
        return 0;
    }

    /* Mark objective as active (further state tracking can be added later) */
    g_scrObjectives[ objId ].inuse = qtrue;

    return 0;
}

/* objective_delete(id) — deactivates an objective */
static int GScr_Fn_Objective_Delete( gsc_Context *ctx )
{
    int objId;

    if ( gsc_numargs( ctx ) < 1 ) {
        return 0;
    }

    objId = (int)gsc_get_int( ctx, 0 );
    if ( objId >= 0 && objId < G_SCR_MAX_OBJECTIVES ) {
        g_scrObjectives[ objId ].inuse = qfalse;
    }
    return 0;
}

/* objective_state(id, state) — stub: state tracking (0=hidden, 1=active, 2=done) */
static int GScr_Fn_Objective_State( gsc_Context *ctx )
{
    (void)ctx;
    /* Objective state tracking can be added if needed by scripts */
    return 0;
}

/* objective_position(id, origin) — stores objective position */
static int GScr_Fn_Objective_Position( gsc_Context *ctx )
{
    int objId;

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    objId = (int)gsc_get_int( ctx, 0 );
    if ( objId < 0 || objId >= G_SCR_MAX_OBJECTIVES ) {
        return 0;
    }

    /* Use gsc_get_vec3 to directly parse the vector argument */
    gsc_get_vec3( ctx, 1, g_scrObjectives[ objId ].origin );

    return 0;
}

/* objective_icon(id, shader) — stores objective icon shader name */
static int GScr_Fn_Objective_Icon( gsc_Context *ctx )
{
    int         objId;
    const char *shader;

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    objId  = (int)gsc_get_int( ctx, 0 );
    shader = gsc_get_string( ctx, 1 );

    if ( objId >= 0 && objId < G_SCR_MAX_OBJECTIVES ) {
        Q_strncpyz( g_scrObjectives[ objId ].icon, shader, MAX_QPATH );
    }
    return 0;
}

/* objective_team(id, team) — stores objective team */
static int GScr_Fn_Objective_Team( gsc_Context *ctx )
{
    int    objId;
    team_t team;

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    objId = (int)gsc_get_int( ctx, 0 );
    team  = G_Scr_ParseTeamArg( ctx, 1 );

    if ( objId >= 0 && objId < G_SCR_MAX_OBJECTIVES ) {
        g_scrObjectives[ objId ].team = (int)team;
    }
    return 0;
}

/* objective_onentity(id, entity) — attaches objective to an entity */
static int GScr_Fn_Objective_OnEntity( gsc_Context *ctx )
{
    int objId;
    int entNum;

    if ( gsc_numargs( ctx ) < 2 ) {
        return 0;
    }

    objId = (int)gsc_get_int( ctx, 0 );

    if ( gsc_get_type( ctx, 1 ) == GSC_TYPE_OBJECT ) {
        int obj = gsc_get_object( ctx, 1 );
        gsc_object_get_field( ctx, obj, "entitynum" );
        if ( gsc_type( ctx, -1 ) != GSC_TYPE_UNDEFINED ) {
            entNum = (int)gsc_get_int( ctx, -1 );
            if ( objId >= 0 && objId < G_SCR_MAX_OBJECTIVES ) {
                g_scrObjectives[ objId ].entNum = entNum;
            }
        }
        gsc_pop( ctx, 1 );
    }

    return 0;
}

/* maps_mp_gametypes_callbacksetup compatibility stubs */
static int GScr_Fn_GetMaxPlayers( gsc_Context *ctx )
{
    gsc_add_int( ctx, level.maxclients );
    return 1;
}

static int GScr_Fn_GetNumPlayers( gsc_Context *ctx )
{
    gsc_add_int( ctx, level.numConnectedClients );
    return 1;
}

/* =========================================================================
   Register all built-in functions.
   The GSC hash trie is case-insensitive, so one registration per function
   covers all capitalisation variants.
   ========================================================================= */
static void G_Scr_RegisterFunctions( void )
{
    /* Output */
    gsc_register_function( g_scrCtx, NULL, "print",        GScr_Fn_Print );
    gsc_register_function( g_scrCtx, NULL, "println",      GScr_Fn_Print );
    gsc_register_function( g_scrCtx, NULL, "iprintln",     GScr_Fn_IPrintLn );
    gsc_register_function( g_scrCtx, NULL, "iprintlnbold", GScr_Fn_IPrintLnBold );

    /* Type checking */
    gsc_register_function( g_scrCtx, NULL, "isdefined",    GScr_Fn_IsDefined );

    /* Math / random */
    gsc_register_function( g_scrCtx, NULL, "randomint",    GScr_Fn_RandomInt );
    gsc_register_function( g_scrCtx, NULL, "randomfloat",  GScr_Fn_RandomFloat );
    gsc_register_function( g_scrCtx, NULL, "distance",     GScr_Fn_Distance );
    gsc_register_function( g_scrCtx, NULL, "length",       GScr_Fn_Length );
    gsc_register_function( g_scrCtx, NULL, "lengthsquared", GScr_Fn_LengthSquared );
    gsc_register_function( g_scrCtx, NULL, "vectornormalize", GScr_Fn_VectorNormalize );
    gsc_register_function( g_scrCtx, NULL, "vectorscale",  GScr_Fn_VectorScale );
    gsc_register_function( g_scrCtx, NULL, "vectortoangles", GScr_Fn_VectorToAngles );
    gsc_register_function( g_scrCtx, NULL, "anglestoforward", GScr_Fn_AnglesToForward );

    /* Cvar / dvar — register both spellings since they're different names */
    gsc_register_function( g_scrCtx, NULL, "getdvar",      GScr_Fn_GetDvar );
    gsc_register_function( g_scrCtx, NULL, "getdvarint",   GScr_Fn_GetDvarInt );
    gsc_register_function( g_scrCtx, NULL, "getdvarfloat", GScr_Fn_GetDvarFloat );
    gsc_register_function( g_scrCtx, NULL, "setdvar",      GScr_Fn_SetDvar );
    gsc_register_function( g_scrCtx, NULL, "getcvar",      GScr_Fn_GetCvar );   /* CoD1 alias */
    gsc_register_function( g_scrCtx, NULL, "getcvarint",   GScr_Fn_GetDvarInt );
    gsc_register_function( g_scrCtx, NULL, "getcvarfloat", GScr_Fn_GetDvarFloat );
    gsc_register_function( g_scrCtx, NULL, "setcvar",      GScr_Fn_SetCvar );   /* CoD1 alias */
    gsc_register_function( g_scrCtx, NULL, "makecvarserverinfo", GScr_Fn_MakeCvarServerInfo );

    /* Time */
    gsc_register_function( g_scrCtx, NULL, "gettime",      GScr_Fn_GetTime );

    /* Entity access */
    gsc_register_function( g_scrCtx, NULL, "getent",       GScr_Fn_GetEnt );
    gsc_register_function( g_scrCtx, NULL, "getentarray",  GScr_Fn_GetEntArray );
    gsc_register_function( g_scrCtx, NULL, "spawn",        GScr_Fn_Spawn );
    gsc_register_function( g_scrCtx, NULL, "spawnstruct",  GScr_Fn_SpawnStruct );
    gsc_register_function( g_scrCtx, NULL, "positionwouldtelefrag", GScr_Fn_PositionWouldTelefrag );

    /* Player counts */
    gsc_register_function( g_scrCtx, NULL, "getmaxplayers", GScr_Fn_GetMaxPlayers );
    gsc_register_function( g_scrCtx, NULL, "getnumplayers", GScr_Fn_GetNumPlayers );
    gsc_register_function( g_scrCtx, NULL, "isplayer",     GScr_Fn_IsPlayerGlobal );
    gsc_register_function( g_scrCtx, NULL, "isalive",      GScr_Fn_IsAliveGlobal );

    /* Level control */
    gsc_register_function( g_scrCtx, NULL, "exitlevel",    GScr_Fn_ExitLevel );
    gsc_register_function( g_scrCtx, NULL, "maprestart",   GScr_Fn_MapRestart );
    gsc_register_function( g_scrCtx, NULL, "mapexists",    GScr_Fn_MapExists );
    gsc_register_function( g_scrCtx, NULL, "setarchive",   GScr_Fn_SetArchive );
    gsc_register_function( g_scrCtx, NULL, "resettimeout", GScr_Fn_ResetTimeout );
    gsc_register_function( g_scrCtx, NULL, "setclientnamemode", GScr_Fn_SetClientNameMode );
    gsc_register_function( g_scrCtx, NULL, "obituary",     GScr_Fn_Obituary );
    gsc_register_function( g_scrCtx, NULL, "logprint",     GScr_Fn_LogPrint );
    gsc_register_function( g_scrCtx, NULL, "announcement", GScr_Fn_Announcement );
    gsc_register_function( g_scrCtx, NULL, "clientannouncement", GScr_Fn_ClientAnnouncement );
    gsc_register_function( g_scrCtx, NULL, "updatescores", GScr_Fn_UpdateScores );
    gsc_register_function( g_scrCtx, NULL, "addtestclient", GScr_Fn_AddTestClient );

    /* Client-side / audio stubs (no-ops on the server) */
    gsc_register_function( g_scrCtx, NULL, "ambientplay",  GScr_Fn_AmbientPlay );
    gsc_register_function( g_scrCtx, NULL, "ambientstop",  GScr_Fn_AmbientStop );
    gsc_register_function( g_scrCtx, NULL, "setcullfog",   GScr_Fn_SetCullFog );
    gsc_register_function( g_scrCtx, NULL, "loadfx",       GScr_Fn_LoadFx );
    gsc_register_function( g_scrCtx, NULL, "playfx",       GScr_Fn_PlayFx );
    gsc_register_function( g_scrCtx, NULL, "playfxontag",  GScr_Fn_PlayFxOnTag );
    gsc_register_function( g_scrCtx, NULL, "precachemenu", GScr_Fn_PrecacheMenu );

    /* Team score functions (CoD1) */
    gsc_register_function( g_scrCtx, NULL, "setteamscore", GScr_Fn_SetTeamScore );
    gsc_register_function( g_scrCtx, NULL, "getteamscore", GScr_Fn_GetTeamScore );

    /* Objective functions (CoD1) */
    gsc_register_function( g_scrCtx, NULL, "objective_add",        GScr_Fn_Objective_Add );
    gsc_register_function( g_scrCtx, NULL, "objective_delete",     GScr_Fn_Objective_Delete );
    gsc_register_function( g_scrCtx, NULL, "objective_state",      GScr_Fn_Objective_State );
    gsc_register_function( g_scrCtx, NULL, "objective_position",   GScr_Fn_Objective_Position );
    gsc_register_function( g_scrCtx, NULL, "objective_icon",       GScr_Fn_Objective_Icon );
    gsc_register_function( g_scrCtx, NULL, "objective_team",       GScr_Fn_Objective_Team );
    gsc_register_function( g_scrCtx, NULL, "objective_onentity",   GScr_Fn_Objective_OnEntity );
    gsc_register_function( g_scrCtx, NULL, "precachestatusicon", GScr_Fn_PrecacheStatusIcon );
    gsc_register_function( g_scrCtx, NULL, "precacheheadicon", GScr_Fn_PrecacheHeadIcon );
    gsc_register_function( g_scrCtx, NULL, "precacheitem", GScr_Fn_PrecacheItem );
    gsc_register_function( g_scrCtx, NULL, "precachemodel", GScr_Fn_PrecacheModel );
    gsc_register_function( g_scrCtx, NULL, "precacheshader", GScr_Fn_PrecacheShader );
    gsc_register_function( g_scrCtx, NULL, "precachestring", GScr_Fn_PrecacheString );
    gsc_register_function( g_scrCtx, NULL, "precacheturret", GScr_Fn_PrecacheTurret );

    /* HUD element constructors */
    gsc_register_function( g_scrCtx, NULL, "newhudelem",        GScr_Fn_NewHudElem );
    gsc_register_function( g_scrCtx, NULL, "newclienthudelem",  GScr_Fn_NewClientHudElem );
    gsc_register_function( g_scrCtx, NULL, "newteamhudelem",    GScr_Fn_NewTeamHudElem );
}

/* =========================================================================
   Create the shared entity proxy and the level/game/anim globals.

   Globals set:
     level, game, anim  (tagged empty objects, CoD compat)
     self               (initially = level object)
   ========================================================================= */
static void G_Scr_AddMethod( int methodsObj, const char *name, gsc_Function fn )
{
    gsc_add_function( g_scrCtx, fn );
    gsc_object_set_field( g_scrCtx, methodsObj, name );
}

static void G_Scr_CreateGlobals( void )
{
    int entProxyObj;
    int entMethodsObj;
    int entGetObj;
    int entSetObj;
    int playerProxyObj;
    int playerMethodsObj;
    int playerGetObj;
    int playerSetObj;
    int hudProxyObj;
    int hudMethodsObj;
    int levelObj;

    /* ---- base entity proxy ---- */
    entProxyObj = gsc_add_tagged_object( g_scrCtx, "#ent_proxy" );
    g_scrEntityProxyRef = gsc_ref( g_scrCtx, entProxyObj );

    entMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( entMethodsObj, "getname",         GScr_Meth_GetName );
    G_Scr_AddMethod( entMethodsObj, "getorigin",       GScr_Meth_GetOrigin );
    G_Scr_AddMethod( entMethodsObj, "setorigin",       GScr_Meth_SetOrigin );
    G_Scr_AddMethod( entMethodsObj, "gethealth",       GScr_Meth_GetHealth );
    G_Scr_AddMethod( entMethodsObj, "sethealth",       GScr_Meth_SetHealth );
    G_Scr_AddMethod( entMethodsObj, "getteam",         GScr_Meth_GetTeam );
    G_Scr_AddMethod( entMethodsObj, "getclientnum",    GScr_Meth_GetClientNum );
    G_Scr_AddMethod( entMethodsObj, "getentitynumber", GScr_Meth_GetEntityNumber );
    G_Scr_AddMethod( entMethodsObj, "isplayer",        GScr_Meth_IsPlayer );
    G_Scr_AddMethod( entMethodsObj, "isalive",         GScr_Meth_IsAlive );
    G_Scr_AddMethod( entMethodsObj, "spawn",           GScr_Meth_Spawn );
    G_Scr_AddMethod( entMethodsObj, "suicide",         GScr_Meth_Suicide );
    G_Scr_AddMethod( entMethodsObj, "iprintln",        GScr_Meth_Player_IPrintLn );
    G_Scr_AddMethod( entMethodsObj, "iprintlnbold",    GScr_Meth_Player_IPrintLnBold );
    G_Scr_AddMethod( entMethodsObj, "placespawnpoint", GScr_Meth_PlaceSpawnpoint );
    G_Scr_AddMethod( entMethodsObj, "delete",          GScr_Meth_Delete );
    G_Scr_AddMethod( entMethodsObj, "hide",            GScr_Meth_Hide );
    G_Scr_AddMethod( entMethodsObj, "show",            GScr_Meth_Show );
    G_Scr_AddMethod( entMethodsObj, "unlink",          GScr_Meth_Unlink );
    G_Scr_AddMethod( entMethodsObj, "notsolid",        GScr_Meth_NotSolid );
    G_Scr_AddMethod( entMethodsObj, "solid",           GScr_Meth_Solid );
    G_Scr_AddMethod( entMethodsObj, "setmodel",        GScr_Meth_SetModel );
    G_Scr_AddMethod( entMethodsObj, "istouching",      GScr_Meth_IsTouching );
    G_Scr_AddMethod( entMethodsObj, "attach",          GScr_Meth_Attach );
    G_Scr_AddMethod( entMethodsObj, "detach",          GScr_Meth_Detach );
    G_Scr_AddMethod( entMethodsObj, "detachall",       GScr_Meth_DetachAll );
    G_Scr_AddMethod( entMethodsObj, "getattachsize",   GScr_Meth_GetAttachSize );
    G_Scr_AddMethod( entMethodsObj, "getattachmodelname", GScr_Meth_GetAttachModelName );
    G_Scr_AddMethod( entMethodsObj, "getattachtagname", GScr_Meth_GetAttachTagName );
    G_Scr_AddMethod( entMethodsObj, "getattachignorecollision", GScr_Meth_GetAttachIgnoreCollision );
    G_Scr_AddMethod( entMethodsObj, "playsound",       GScr_Meth_PlaySound );
    G_Scr_AddMethod( entMethodsObj, "playlocalsound",  GScr_Meth_PlayLocalSound );
    G_Scr_AddMethod( entMethodsObj, "playloopsound",   GScr_Meth_PlayLoopSound );
    G_Scr_AddMethod( entMethodsObj, "stoploopsound",   GScr_Meth_StopLoopSound );
    gsc_object_set_field( g_scrCtx, entProxyObj, "__call" );

    entGetObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( entGetObj, "name",      GScr_Meth_GetName );
    G_Scr_AddMethod( entGetObj, "origin",    GScr_Meth_GetOrigin );
    G_Scr_AddMethod( entGetObj, "angles",    GScr_Field_GetAngles );
    G_Scr_AddMethod( entGetObj, "health",    GScr_Meth_GetHealth );
    G_Scr_AddMethod( entGetObj, "entitynum", GScr_Meth_GetEntityNumber );
    gsc_object_set_field( g_scrCtx, entProxyObj, "__get" );

    entSetObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( entSetObj, "origin", GScr_Meth_SetOrigin );
    G_Scr_AddMethod( entSetObj, "angles", GScr_Field_SetAngles );
    G_Scr_AddMethod( entSetObj, "health", GScr_Meth_SetHealth );
    gsc_object_set_field( g_scrCtx, entProxyObj, "__set" );

    /* ---- player proxy (inherits from ent proxy) ---- */
    playerProxyObj = gsc_add_tagged_object( g_scrCtx, "#player_proxy" );
    g_scrPlayerProxyRef = gsc_ref( g_scrCtx, playerProxyObj );
    gsc_object_set_proxy( g_scrCtx, playerProxyObj, entProxyObj );

    playerMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( playerMethodsObj, "getguid",               GScr_Meth_GetGuid );
    G_Scr_AddMethod( playerMethodsObj, "setclientcvar",         GScr_Meth_SetClientCvar );
    G_Scr_AddMethod( playerMethodsObj, "openmenu",              GScr_Meth_OpenMenu );
    G_Scr_AddMethod( playerMethodsObj, "closemenu",             GScr_Meth_CloseMenu );
    G_Scr_AddMethod( playerMethodsObj, "closeingamemenu",       GScr_Meth_CloseInGameMenu );
    G_Scr_AddMethod( playerMethodsObj, "attackbuttonpressed",   GScr_Meth_AttackButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "usebuttonpressed",      GScr_Meth_UseButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "meleebuttonpressed",    GScr_Meth_MeleeButtonPressed );
    G_Scr_AddMethod( playerMethodsObj, "playerads",             GScr_Meth_PlayerADS );
    G_Scr_AddMethod( playerMethodsObj, "isonground",            GScr_Meth_IsOnGround );
    G_Scr_AddMethod( playerMethodsObj, "giveweapon",            GScr_Meth_GiveWeapon );
    G_Scr_AddMethod( playerMethodsObj, "takeweapon",            GScr_Meth_TakeWeapon );
    G_Scr_AddMethod( playerMethodsObj, "takeallweapons",        GScr_Meth_TakeAllWeapons );
    G_Scr_AddMethod( playerMethodsObj, "givemaxammo",           GScr_Meth_GiveMaxAmmo );
    G_Scr_AddMethod( playerMethodsObj, "givestartammo",         GScr_Meth_GiveStartAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getfractionstartammo",  GScr_Meth_GetFractionStartAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getfractionmaxammo",    GScr_Meth_GetFractionMaxAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setspawnweapon",        GScr_Meth_SetSpawnWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotweapon",   GScr_Meth_SetWeaponSlotWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotammo",     GScr_Meth_SetWeaponSlotAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setweaponslotclipammo", GScr_Meth_SetWeaponSlotClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "setweaponclipammo",     GScr_Meth_SetWeaponClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotweapon",   GScr_Meth_GetWeaponSlotWeapon );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotammo",     GScr_Meth_GetWeaponSlotAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getweaponslotclipammo", GScr_Meth_GetWeaponSlotClipAmmo );
    G_Scr_AddMethod( playerMethodsObj, "getcurrentweapon",      GScr_Meth_GetCurrentWeapon );
    G_Scr_AddMethod( playerMethodsObj, "getcurrentoffhand",     GScr_Meth_GetCurrentOffhand );
    G_Scr_AddMethod( playerMethodsObj, "hasweapon",             GScr_Meth_HasWeapon );
    G_Scr_AddMethod( playerMethodsObj, "switchtoweapon",        GScr_Meth_SwitchToWeapon );
    G_Scr_AddMethod( playerMethodsObj, "switchtooffhand",       GScr_Meth_SwitchToOffhand );
    G_Scr_AddMethod( playerMethodsObj, "setclientdvar",         GScr_Meth_SetClientDvar );
    G_Scr_AddMethod( playerMethodsObj, "freezecontrols",        GScr_Meth_FreezeControls );
    G_Scr_AddMethod( playerMethodsObj, "shellshock",            GScr_Meth_Shellshock );
    G_Scr_AddMethod( playerMethodsObj, "disableweapon",         GScr_Meth_DisableWeapon );
    G_Scr_AddMethod( playerMethodsObj, "enableweapon",          GScr_Meth_EnableWeapon );
    G_Scr_AddMethod( playerMethodsObj, "setentertime",          GScr_Meth_SetEnterTime );
    G_Scr_AddMethod( playerMethodsObj, "pingplayer",            GScr_Meth_PingPlayer );
    G_Scr_AddMethod( playerMethodsObj, "sayall",                GScr_Meth_SayAll );
    G_Scr_AddMethod( playerMethodsObj, "sayteam",               GScr_Meth_SayTeam );
    G_Scr_AddMethod( playerMethodsObj, "setviewmodel",          GScr_Meth_SetViewModel );
    G_Scr_AddMethod( playerMethodsObj, "getviewmodel",          GScr_Meth_GetViewModel );
    G_Scr_AddMethod( playerMethodsObj, "cloneplayer",           GScr_Meth_ClonePlayer );
    G_Scr_AddMethod( playerMethodsObj, "dropitem",              GScr_Meth_DropItem );
    G_Scr_AddMethod( playerMethodsObj, "finishplayerdamage",    GScr_Meth_FinishPlayerDamage );
    G_Scr_AddMethod( playerMethodsObj, "allowspectateteam",     GScr_Meth_AllowSpectateTeam );
    gsc_object_set_field( g_scrCtx, playerProxyObj, "__call" );

    playerGetObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( playerGetObj, "score",           GScr_Field_GetScore );
    G_Scr_AddMethod( playerGetObj, "deaths",          GScr_Field_GetDeaths );
    G_Scr_AddMethod( playerGetObj, "maxhealth",       GScr_Field_GetMaxHealth );
    G_Scr_AddMethod( playerGetObj, "sessionteam",     GScr_Field_GetSessionTeam );
    G_Scr_AddMethod( playerGetObj, "sessionstate",    GScr_Field_GetSessionState );
    G_Scr_AddMethod( playerGetObj, "spectatorclient", GScr_Field_GetSpectatorClient );
    gsc_object_set_field( g_scrCtx, playerProxyObj, "__get" );

    playerSetObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( playerSetObj, "score",           GScr_Field_SetScore );
    G_Scr_AddMethod( playerSetObj, "deaths",          GScr_Field_SetDeaths );
    G_Scr_AddMethod( playerSetObj, "maxhealth",       GScr_Field_SetMaxHealth );
    G_Scr_AddMethod( playerSetObj, "sessionteam",     GScr_Field_SetSessionTeam );
    G_Scr_AddMethod( playerSetObj, "sessionstate",    GScr_Field_SetSessionState );
    G_Scr_AddMethod( playerSetObj, "spectatorclient", GScr_Field_SetSpectatorClient );
    gsc_object_set_field( g_scrCtx, playerProxyObj, "__set" );

    /* ---- HUD element proxy ---- */
    hudProxyObj = gsc_add_tagged_object( g_scrCtx, "#hudelem_proxy" );
    g_scrHudElemProxyRef = gsc_ref( g_scrCtx, hudProxyObj );

    hudMethodsObj = gsc_add_object( g_scrCtx );
    G_Scr_AddMethod( hudMethodsObj, "settext",       GScr_Meth_Hud_SetText );
    G_Scr_AddMethod( hudMethodsObj, "setshader",     GScr_Meth_Hud_SetShader );
    G_Scr_AddMethod( hudMethodsObj, "settimer",      GScr_Meth_Hud_SetTimer );
    G_Scr_AddMethod( hudMethodsObj, "settimerup",    GScr_Meth_Hud_SetTimerUp );
    G_Scr_AddMethod( hudMethodsObj, "settenthstimer", GScr_Meth_Hud_SetTenthsTimer );
    G_Scr_AddMethod( hudMethodsObj, "setvalue",      GScr_Meth_Hud_SetValue );
    G_Scr_AddMethod( hudMethodsObj, "fadeovertime",  GScr_Meth_Hud_FadeOverTime );
    G_Scr_AddMethod( hudMethodsObj, "moveovertime",  GScr_Meth_Hud_MoveOverTime );
    G_Scr_AddMethod( hudMethodsObj, "destroy",       GScr_Meth_Hud_Destroy );
    G_Scr_AddMethod( hudMethodsObj, "delete",        GScr_Meth_Hud_Destroy );
    gsc_object_set_field( g_scrCtx, hudProxyObj, "__call" );

    /* ---- level object (CoD compat global) ---- */
    levelObj = gsc_add_tagged_object( g_scrCtx, "#level" );
    gsc_object_set_proxy( g_scrCtx, levelObj, entProxyObj );
    gsc_set_global( g_scrCtx, "level" );

    /* ---- game object ---- */
    gsc_add_tagged_object( g_scrCtx, "#game" );
    gsc_set_global( g_scrCtx, "game" );

    /* ---- anim object ---- */
    gsc_add_tagged_object( g_scrCtx, "#anim" );
    gsc_set_global( g_scrCtx, "anim" );

    /* ---- default self = level ---- */
    gsc_get_global( g_scrCtx, "level" );
    gsc_set_global( g_scrCtx, "self" );
}

/* =========================================================================
   Create and register a GSC entity object for a connected client.
   ========================================================================= */
static void G_Scr_CreatePlayerObj( int clientNum )
{
    if ( !g_scrCtx || clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }
    G_Scr_CreateEntityObject( &g_entities[ clientNum ], qfalse );
}

/* =========================================================================
   Swap the GSC "self" global to the given player entity object.
   Must be called before any callback that needs self = player.
   ========================================================================= */
static void G_Scr_SetSelfToPlayer( int clientNum )
{
    char globalName[ 32 ];
    int  topBefore;

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }

    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        G_Scr_CreatePlayerObj( clientNum );
    }
    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        return;
    }

    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );

    topBefore = gsc_top( g_scrCtx );
    gsc_get_global( g_scrCtx, globalName );

    if ( gsc_top( g_scrCtx ) > topBefore ) {
        if ( gsc_type( g_scrCtx, -1 ) == GSC_TYPE_OBJECT ) {
            gsc_set_global( g_scrCtx, "self" );
        } else {
            gsc_pop( g_scrCtx, 1 );
        }
    }
}

/* =========================================================================
   Try to call a function in g_scrCallbackFile.
   Silently ignores GSC_NOT_FOUND (callback not in this script).
   ========================================================================= */
static void G_Scr_ExecCallback( const char *fnName )
{
    int status;
    if ( !g_scrActive || !g_scrCtx || !g_scrCallbackFile[0] ) {
        return;
    }
    status = gsc_call( g_scrCtx, g_scrCallbackFile, fnName, 0 );
    if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
        G_Printf( "GSC: error in %s::%s (status %d)\n",
                  g_scrCallbackFile, fnName, status );
    }
}

/* Same but sets self = player before calling */
static void G_Scr_ExecPlayerCallback( int clientNum, const char *fnName )
{
    if ( !g_scrActive || !g_scrCtx || !g_scrCallbackFile[0] ) {
        return;
    }
    G_Scr_SetSelfToPlayer( clientNum );
    G_Scr_ExecCallback( fnName );
}

/* =========================================================================
   G_Scr_GetGametypeString
   Returns the CoD1 MP gametype name ("dm", "tdm", "hq", "sd", "re", "bel").
   g_gametype in Q3 is an integer (0=FFA, 3=TDM, 4=CTF…).
   In CoD1 MP it is a string cvar set by the server: +set g_gametype dm
   We read the raw string; if it looks like a CoD1 name we use it directly,
   otherwise we map Q3 integers to CoD1 names.
   ========================================================================= */
static void G_Scr_GetGametypeString( char *out, int outSize )
{
    char buf[ 64 ];
    trap_Cvar_VariableStringBuffer( "g_gametype", buf, sizeof( buf ) );

    /* If already a known CoD1 MP gametype name, use it */
    if ( Q_stricmp( buf, "dm"  ) == 0 || Q_stricmp( buf, "tdm" ) == 0 ||
         Q_stricmp( buf, "hq"  ) == 0 || Q_stricmp( buf, "sd"  ) == 0 ||
         Q_stricmp( buf, "re"  ) == 0 || Q_stricmp( buf, "bel" ) == 0 ) {
        Q_strncpyz( out, buf, outSize );
        return;
    }

    /* Q3 integer → CoD1 name fallback */
    switch ( atoi( buf ) ) {
        case 3: /* GT_TEAM */ Q_strncpyz( out, "tdm", outSize ); break;
        case 4: /* GT_CTF  */ Q_strncpyz( out, "sd",  outSize ); break;
        default:              Q_strncpyz( out, "dm",  outSize ); break;
    }
}

/* =========================================================================
   G_Scr_Init  —  called at the end of G_InitGame
   ========================================================================= */
void G_Scr_Init( void )
{
    gsc_CreateOptions opts;
    char              serverinfo[ MAX_INFO_STRING ];
    const char       *mapnameRaw;
    char              mapname[ MAX_QPATH ];
    char              gametype[ 16 ];
    char              gametypeScript[ MAX_QPATH ];  /* maps/mp/gametypes/<gt>    */
    char              callbackScript[ MAX_QPATH ];  /* maps/mp/gametypes/_cbsetup */
    char              mapScript[ MAX_QPATH ];       /* maps/mp/<mapname>          */
    char              mapScriptSP[ MAX_QPATH ];     /* maps/<mapname> (SP fallback) */
    int               i;
    int               status;
    qboolean          gametypeOk;
    qboolean          callbackOk;
    qboolean          mapOk;

    /* Reset state */
    g_scrCtx     = NULL;
    g_scrActive  = qfalse;
    g_scrSources = NULL;
    G_Scr_ResetRefs();
    Com_Memset( g_scrEntityObjAlive, 0, sizeof( g_scrEntityObjAlive ) );
    Com_Memset( g_scrAttachStates, 0, sizeof( g_scrAttachStates ) );
    Com_Memset( g_scrViewModels, 0, sizeof( g_scrViewModels ) );
    Com_Memset( g_scrFxNames, 0, sizeof( g_scrFxNames ) );
    g_scrFxCount = 0;
    g_scrDeferredMenuCount = 0;
#ifdef STANDALONE
    /* Clear script menu configstrings from previous map */
    for ( i = 0; i < MAX_SCRIPT_MENUS; i++ ) {
        trap_SetConfigstring( CS_SCRIPT_MENUS + i, "" );
    }
#endif
    Com_Memset( g_scrObjectives, 0, sizeof( g_scrObjectives ) );
    G_Scr_Hud_ResetAll( qfalse );
    g_scrCallbackFile[0] = '\0';

    /* Determine map name and gametype */
    trap_GetServerinfo( serverinfo, sizeof( serverinfo ) );
    mapnameRaw = Info_ValueForKey( serverinfo, "mapname" );
    G_Scr_NormalizeMapName( mapnameRaw, mapname, sizeof( mapname ) );
    if ( !mapname[0] ) {
        G_Printf( "GSC: no mapname in serverinfo, scripting disabled\n" );
        return;
    }

    G_Scr_GetGametypeString( gametype, sizeof( gametype ) );

    /*
     Script namespaces (no .gsc extension — the read_file callback adds it).
     CoD1 MP maps live under  maps/mp/<mapname>.gsc
     CoD1 SP maps live under  maps/<mapname>.gsc
     Gametypes live under     maps/mp/gametypes/<gametype>.gsc
     _callbacksetup is at     maps/mp/gametypes/_callbacksetup.gsc
                                                                             */
    Com_sprintf( gametypeScript, sizeof( gametypeScript ),
                 "maps/mp/gametypes/%s", gametype );
    Com_sprintf( callbackScript, sizeof( callbackScript ),
                 "maps/mp/gametypes/_callbacksetup" );
    Com_sprintf( mapScript,      sizeof( mapScript ),
                 "maps/mp/%s",   mapname );
    Com_sprintf( mapScriptSP,    sizeof( mapScriptSP ),
                 "maps/%s",      mapname );

    G_Printf( "GSC: map '%s' (raw '%s')  gametype '%s'\n",
              mapname, mapnameRaw ? mapnameRaw : "", gametype );

    /* Create context */
    Com_Memset( &opts, 0, sizeof( opts ) );
    opts.allocate_memory          = G_Scr_AllocMem;
    opts.free_memory              = G_Scr_FreeMem;
    opts.read_file                = G_Scr_ReadFile;
    opts.verbose                  = 0;
    opts.main_memory_size         = 96 * 1024 * 1024; /* 96 MB */
    opts.temp_memory_size         = 16 * 1024 * 1024; /* 16 MB */
    opts.string_table_memory_size = 8  * 1024 * 1024; /*  8 MB */
    opts.default_self             = "self";
    opts.max_threads              = 128;

    g_scrCtx = gsc_create( opts );
    if ( !g_scrCtx ) {
        G_Printf( "GSC: failed to create context\n" );
        return;
    }

    G_Scr_RegisterFunctions();
    G_Scr_CreateGlobals();

    /*
     CoD1 MP loading order (matches GScr_LoadGameTypeScript in CoD2):
       1. Gametype script  — sets level.callback* and registers game logic
          (auto-loads _callbacksetup and _teams/_spawnlogic as dependencies
           via maps\mp\gametypes\... :: cross-file call syntax)
       2. _callbacksetup   — always compile explicitly too, as the GSC library
          may not yet resolve the CoD-style :: cross-file dependency notation
       3. Map script       — sets game["allies"]/["axis"], loads _load/_fx etc.
          Try maps/MP/<name>.gsc first (MP), fall back to maps/<name>.gsc (SP)
    */
    gametypeOk = G_Scr_CompileScript( gametypeScript );
    callbackOk = G_Scr_CompileScript( callbackScript );  /* explicit, safe to dup */

    mapOk      = G_Scr_CompileScript( mapScript );
    if ( !mapOk ) {
        mapOk = G_Scr_CompileScript( mapScriptSP );      /* SP fallback */
        if ( mapOk ) {
            Q_strncpyz( mapScript, mapScriptSP, sizeof( mapScript ) );
        }
    }

    if ( !gametypeOk && !callbackOk && !mapOk ) {
        G_Printf( "GSC: no scripts found for map '%s' gametype '%s', "
                  "scripting disabled\n", mapname, gametype );
        gsc_destroy( g_scrCtx );
        G_Scr_FreeSources();
        g_scrCtx = NULL;
        return;
    }

    /* Link all compiled files together */
    status = gsc_link( g_scrCtx );
    G_Scr_FreeSources(); /* source text no longer needed after link */

    if ( status != GSC_OK ) {
        G_Printf( "GSC: link failed (status %d)\n", status );
        gsc_destroy( g_scrCtx );
        g_scrCtx = NULL;
        return;
    }

    /*
     Choose which file holds the CodeCallback_* entry points.
     _callbacksetup is the canonical location; fall back to gametype or map.
    */
    if ( gametypeOk && callbackOk ) {
        Q_strncpyz( g_scrCallbackFile, callbackScript,
                    sizeof( g_scrCallbackFile ) );
    } else if ( gametypeOk ) {
        Q_strncpyz( g_scrCallbackFile, gametypeScript,
                    sizeof( g_scrCallbackFile ) );
    } else if ( mapOk ) {
        Q_strncpyz( g_scrCallbackFile, mapScript,
                    sizeof( g_scrCallbackFile ) );
    } else {
        g_scrCallbackFile[0] = '\0';
    }

    if ( !gametypeOk ) {
        G_Printf( "GSC: gametype '%s' unavailable; CodeCallback_* hooks disabled\n",
                  gametypeScript );
        g_scrCallbackFile[0] = '\0';
    }

    g_scrActive = qtrue;
    if ( g_scrCallbackFile[0] ) {
        G_Printf( "GSC: scripting active  callbacks in '%s'\n", g_scrCallbackFile );
    } else {
        G_Printf( "GSC: scripting active  callbacks disabled\n" );
    }

    /* Fresh HUD state per map and stable object handles for all live entities. */
    G_Scr_Hud_ResetAll( qtrue );
    for ( i = 0; i < level.num_entities; i++ ) {
        if ( g_entities[ i ].inuse ) {
            G_Scr_CreateEntityObject( &g_entities[ i ], qfalse );
        }
    }

    /*
     Execution order:
       1. Gametype main()  — registers level.callbackStartGameType etc.
       2. Map main()       — sets game["allies"], game["axis"], loads FX etc.
       3. CodeCallback_StartGameType() — calls level.callbackStartGameType()
    */
    if ( gametypeOk ) {
        status = gsc_call( g_scrCtx, gametypeScript, "main", 0 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in %s::main (status %d)\n",
                      gametypeScript, status );
        }
        /*
         * Do a single startup tick.
         * Gametype scripts spawn persistent threads (score/time updates, etc.);
         * draining until !YIELD here deadlocks map init.
         */
        gsc_update( g_scrCtx, 0.0f );
    }

    if ( mapOk ) {
        status = gsc_call( g_scrCtx, mapScript, "main", 0 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in %s::main (status %d)\n",
                      mapScript, status );
        }
        gsc_update( g_scrCtx, 0.0f );
    }

    if ( g_scrCallbackFile[0] ) {
        G_Scr_ExecCallback( "CodeCallback_StartGameType" );
        gsc_update( g_scrCtx, 0.0f );
    }

}

/* =========================================================================
   G_Scr_Shutdown  —  called at the start of G_ShutdownGame
   ========================================================================= */
void G_Scr_Shutdown( void )
{
    if ( g_scrActive ) {
        G_Scr_Hud_ResetAll( qtrue );
    }

    if ( g_scrCtx ) {
        gsc_destroy( g_scrCtx );
        g_scrCtx = NULL;
    }
    G_Scr_FreeSources();
    G_Scr_ResetRefs();
    Com_Memset( g_scrEntityObjAlive, 0, sizeof( g_scrEntityObjAlive ) );
    Com_Memset( g_scrHudElems, 0, sizeof( g_scrHudElems ) );
    Com_Memset( g_scrAttachStates, 0, sizeof( g_scrAttachStates ) );
    Com_Memset( g_scrViewModels, 0, sizeof( g_scrViewModels ) );
    Com_Memset( g_scrFxNames, 0, sizeof( g_scrFxNames ) );
    g_scrFxCount = 0;
    Com_Memset( g_scrObjectives, 0, sizeof( g_scrObjectives ) );
    g_scrCallbackFile[0] = '\0';
    g_scrActive = qfalse;
}

/* =========================================================================
   G_Scr_Frame  —  called once per game frame from G_RunFrame
   ========================================================================= */
void G_Scr_Frame( float dt )
{
    int i;

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    /* Fire deferred auto-menu-responses before the VM tick so the
       notification arrives in the same frame the thread resumes. */
    if ( g_scrDeferredMenuCount > 0 ) {
        int count = g_scrDeferredMenuCount;
        G_ScrDeferredMenu_t pending[ G_SCR_MAX_DEFERRED_MENUS ];
        Com_Memcpy( pending, g_scrDeferredMenus, sizeof( G_ScrDeferredMenu_t ) * count );
        g_scrDeferredMenuCount = 0;

        for ( i = 0; i < count; i++ ) {
            G_Scr_PlayerMenuResponse( pending[ i ].clientNum,
                                      pending[ i ].menu,
                                      pending[ i ].response );
        }
    }

    gsc_update( g_scrCtx, dt );
    G_Scr_Hud_UpdateTimers();
}

qboolean G_Scr_IsActive( void )
{
    return g_scrActive && g_scrCtx;
}

/* =========================================================================
   Entity lifecycle callbacks
   ========================================================================= */

void G_Scr_EntitySpawned( gentity_t *ent )
{
    int entNum;

    if ( !g_scrActive || !g_scrCtx || !ent ) {
        return;
    }

    if ( !G_Scr_GetEntityNum( ent, &entNum ) || entNum < MAX_CLIENTS ) {
        return;
    }

    G_Scr_ClearEntityRuntimeState( entNum );
    G_Scr_CreateEntityObject( ent, qfalse );
}

/* Set arbitrary map entity key-value pairs (e.g. script_gameobjectname) as
   string fields on the entity's GSC object.  Called from g_spawn.c after the
   spawn function runs, while level.spawnVars[] is still populated.
   Skips fields already reflected by G_Scr_SetEntityObjectFields. */
void G_Scr_SetEntitySpawnVars( gentity_t *ent )
{
    char globalName[ 32 ];
    int  topBefore, entObjIdx, entNum;
    int  i;

    /* Skip fields set by G_Scr_SetEntityObjectFields; proxy handles the rest */
    static const char *knownFields[] = {
        "classname", "origin", "angles", "model", "targetname", "target", NULL
    };

    if ( !g_scrActive || !g_scrCtx || !ent ) {
        return;
    }
    if ( !G_Scr_GetEntityNum( ent, &entNum ) || entNum < MAX_CLIENTS ) {
        return;
    }
    if ( !g_scrEntityObjAlive[ entNum ] ) {
        return;
    }

    G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
    topBefore = gsc_top( g_scrCtx );
    gsc_get_global( g_scrCtx, globalName );
    if ( gsc_top( g_scrCtx ) <= topBefore ||
         gsc_type( g_scrCtx, -1 ) != GSC_TYPE_OBJECT ) {
        if ( gsc_top( g_scrCtx ) > topBefore )
            gsc_pop( g_scrCtx, 1 );
        return;
    }
    entObjIdx = gsc_top( g_scrCtx ) - 1;

    for ( i = 0; i < level.numSpawnVars; i++ ) {
        const char *key   = level.spawnVars[i][0];
        const char *value = level.spawnVars[i][1];
        int k;
        qboolean skip = qfalse;

        if ( !key || !key[0] || !value ) {
            continue;
        }
        for ( k = 0; knownFields[k]; k++ ) {
            if ( !Q_stricmp( key, knownFields[k] ) ) {
                skip = qtrue;
                break;
            }
        }
        if ( skip ) continue;

        gsc_add_string( g_scrCtx, value );
        gsc_object_set_field( g_scrCtx, entObjIdx, key );
    }

    gsc_pop( g_scrCtx, 1 );
}

void G_Scr_EntityFreed( gentity_t *ent )
{
    char globalName[ 32 ];
    int  entNum;

    if ( !g_scrCtx || !ent ) {
        return;
    }

    if ( !G_Scr_GetEntityNum( ent, &entNum ) || entNum < MAX_CLIENTS ) {
        return;
    }

    G_Scr_ClearEntityRuntimeState( entNum );

    if ( !g_scrEntityObjAlive[ entNum ] ) {
        return;
    }

    G_Scr_GetEntityGlobalName( entNum, globalName, sizeof( globalName ) );
    G_Scr_ClearGlobal( globalName );
    g_scrEntityObjAlive[ entNum ] = qfalse;
}

/* =========================================================================
   Player lifecycle callbacks
   ========================================================================= */

static void G_Scr_NotifyPlayer( int clientNum, const char *eventName, int numArgs )
{
    if ( !g_scrActive || !g_scrCtx || !eventName || !eventName[0] ) {
        return;
    }

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }

    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        G_Scr_CreatePlayerObj( clientNum );
    }

    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        return;
    }

    gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ clientNum ] );
    gsc_notify( g_scrCtx, gsc_top( g_scrCtx ) - 1, eventName, numArgs );
    gsc_pop( g_scrCtx, 1 );
}

void G_Scr_PlayerConnect( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    G_Scr_ClearEntityRuntimeState( clientNum );
    G_Scr_CreatePlayerObj( clientNum );
    G_Scr_Hud_SendAllToClient( clientNum );
    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerConnect" );
}

void G_Scr_PlayerBegin( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }

    G_Scr_Hud_SendAllToClient( clientNum );
    G_Scr_NotifyPlayer( clientNum, "begin", 0 );

    /* Run woken threads immediately so the script can open menus
       before the client's first frame renders. */
    gsc_update( g_scrCtx, 0.0f );
}

/* Read a string field from the game[] GSC object.
   Returns a static buffer — copy if you need to keep it. */
static const char *G_Scr_GetGameField( const char *field )
{
    static char buf[ MAX_QPATH ];
    int topB, top;
    const char *val = NULL;

    buf[0] = '\0';
    if ( !g_scrCtx ) return buf;

    topB = gsc_top( g_scrCtx );
    gsc_get_global( g_scrCtx, "game" );
    top = gsc_top( g_scrCtx );
    G_Printf( "  GetGameField('%s'): topB=%d top=%d type=%d\n",
              field, topB, top,
              top > topB ? gsc_type( g_scrCtx, top - 1 ) : -1 );
    if ( top > topB &&
         gsc_type( g_scrCtx, top - 1 ) == GSC_TYPE_OBJECT ) {
        int gObj = top - 1;
        gsc_object_get_field( g_scrCtx, gObj, field );
        top = gsc_top( g_scrCtx );
        G_Printf( "  field type=%d\n",
                  gsc_type( g_scrCtx, top - 1 ) );
        if ( gsc_type( g_scrCtx, top - 1 ) == GSC_TYPE_STRING ||
             gsc_type( g_scrCtx, top - 1 ) == GSC_TYPE_INTERNED_STRING )
            val = gsc_to_string( g_scrCtx, top - 1 );
        if ( val )
            Q_strncpyz( buf, val, sizeof( buf ) );
        gsc_pop( g_scrCtx, 1 );
    }
    if ( gsc_top( g_scrCtx ) > topB )
        gsc_pop( g_scrCtx, gsc_top( g_scrCtx ) - topB );

    return buf;
}

/* After certain menu responses, open the next menu on the client.
   This works around a VM bug where game["menu_team"] evaluates to
   the wrong value inside the script but reads correctly from C. */
/* Open the next script menu on the client by looking up the correct
   menu name from game[] fields (C-side, bypassing the VM arg bug)
   and sending the configstring index. */
static void G_Scr_OpenNextMenu( int clientNum, const char *menu, const char *response )
{
    const char *nextMenu = NULL;
    int idx;

    if ( !menu || !response ) return;

    /* serverinfo closed → open team menu */
    if ( !Q_stricmpn( menu, "serverinfo_", 11 ) &&
         !Q_stricmp( response, "close" ) ) {
        nextMenu = G_Scr_GetGameField( "menu_team" );
    }
    /* team selected → open weapon menu */
    else if ( !Q_stricmpn( menu, "team_", 5 ) &&
              Q_stricmp( response, "close" ) &&
              Q_stricmp( response, "open" ) ) {
        if ( !Q_stricmp( response, "allies" ) ||
             !Q_stricmp( response, "autoassign" ) ) {
            nextMenu = G_Scr_GetGameField( "menu_weapon_allies" );
        }
        if ( !nextMenu || !nextMenu[0] ) {
            nextMenu = G_Scr_GetGameField( "menu_weapon_axis" );
        }
    }

    if ( nextMenu && nextMenu[0] ) {
        idx = G_Scr_GetMenuIndex( nextMenu );
        if ( idx >= 0 ) {
            trap_SendServerCommand( clientNum, va( "t %d", idx ) );
        } else {
            /* Not precached — send name directly */
            trap_SendServerCommand( clientNum, va( "t %s", nextMenu ) );
        }
    }
}

void G_Scr_PlayerMenuResponse( int clientNum, const char *menu, const char *response )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
        return;
    }

    /* Push args then use standard G_Scr_NotifyPlayer which pushes object
       and calls gsc_notify(numArgs). gsc_notify reads numArgs items from
       the top of the stack — but the object is also on top. To avoid
       the object being read as an arg, push args AFTER the object. */
    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        G_Scr_CreatePlayerObj( clientNum );
    }
    if ( g_scrPlayerRefs[ clientNum ] != GSC_NOREF ) {
        gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ clientNum ] );
        gsc_add_string( g_scrCtx, menu ? menu : "" );
        gsc_add_string( g_scrCtx, response ? response : "" );
        /* Object is at sp-3, args at sp-2 and sp-1.
           gsc_notify reads nargs=2 from top = the 2 strings. Correct. */
        gsc_notify( g_scrCtx, gsc_top( g_scrCtx ) - 3, "menuresponse", 2 );
        gsc_pop( g_scrCtx, 1 );  /* pop object */
    }
}

void G_Scr_PlayerDisconnect( int clientNum )
{
    char globalName[ 32 ];
    int  i;

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    G_Scr_ClearEntityRuntimeState( clientNum );

    G_Scr_PlayerMenuResponse( clientNum, "disconnect", "-1" );
    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerDisconnect" );

    /* Advance any disconnect-triggered threads */
    gsc_update( g_scrCtx, 0.0f );

    /* Clear the player global so the object can be GC'd */
    Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
    G_Scr_ClearGlobal( globalName );

    if ( g_scrPlayerRefs[ clientNum ] != GSC_NOREF ) {
        gsc_unref( g_scrCtx, g_scrPlayerRefs[ clientNum ] );
        g_scrPlayerRefs[ clientNum ] = GSC_NOREF;
    }

    for ( i = 0; i < G_SCR_MAX_HUDELEMS; i++ ) {
        G_ScrHudElem_t *hud = &g_scrHudElems[ i ];
        if ( hud->inuse &&
             hud->scope == G_SCR_HUD_SCOPE_CLIENT &&
             hud->owner == clientNum ) {
            Com_Memset( hud, 0, sizeof( *hud ) );
        }
    }
}

void G_Scr_PlayerSpawn( int clientNum )
{
    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }

    if ( g_scrPlayerRefs[ clientNum ] == GSC_NOREF ) {
        G_Scr_CreatePlayerObj( clientNum );
    }

    /* Update the userdata pointer/fields in case the entity was re-initialised. */
    if ( g_scrPlayerRefs[ clientNum ] != GSC_NOREF ) {
        char globalName[ 32 ];
        int  topBefore, entObjIdx;

        Com_sprintf( globalName, sizeof( globalName ), "player_%d", clientNum );
        topBefore = gsc_top( g_scrCtx );
        gsc_get_global( g_scrCtx, globalName );
        if ( gsc_top( g_scrCtx ) > topBefore ) {
            if ( gsc_type( g_scrCtx, -1 ) == GSC_TYPE_OBJECT ) {
                entObjIdx = gsc_top( g_scrCtx ) - 1;
                gsc_object_set_userdata( g_scrCtx, entObjIdx,
                                         &g_entities[ clientNum ] );
                G_Scr_SetEntityObjectFields( g_scrCtx, entObjIdx, &g_entities[ clientNum ] );
            }
            gsc_pop( g_scrCtx, 1 );
        }
    }

    G_Scr_Hud_SendAllToClient( clientNum );
    G_Scr_ExecPlayerCallback( clientNum, "CodeCallback_PlayerSpawn" );
}

/*
 * CoD1 CodeCallback_PlayerKilled(eInflictor, eAttacker, iDamage, sMeansOfDeath,
 *                                sWeapon, vDir, sHitLoc)
 * 7 args
 */
void G_Scr_PlayerKilled( int clientNum, gentity_t *inflictor, gentity_t *attacker,
                         int damage, int mod, const char *weapon, vec3_t dir,
                         int hitLoc )
{
    int attackerNum;
    float vdir[3] = { 0, 0, 0 };

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    if ( !g_scrCallbackFile[0] ) {
        return;
    }

    attackerNum = ( attacker && attacker->client )
                  ? attacker->s.number : ENTITYNUM_WORLD;

    G_Scr_SetSelfToPlayer( clientNum );

    /* eInflictor */
    if ( inflictor && g_scrPlayerRefs[ inflictor->s.number ] != GSC_NOREF ) {
        gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ inflictor->s.number ] );
    } else {
        gsc_add_int( g_scrCtx, inflictor ? inflictor->s.number : ENTITYNUM_WORLD );
    }
    /* eAttacker */
    if ( attacker && attacker->client
         && g_scrPlayerRefs[ attackerNum ] != GSC_NOREF ) {
        gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ attackerNum ] );
    } else {
        gsc_add_int( g_scrCtx, attackerNum );
    }
    /* iDamage */
    gsc_add_int( g_scrCtx, damage );
    /* sMeansOfDeath */
    if ( mod >= 0 && mod < MOD_NUM ) {
        gsc_add_string( g_scrCtx, modNames[mod] );
    } else {
        gsc_add_string( g_scrCtx, "MOD_UNKNOWN" );
    }
    /* sWeapon */
    gsc_add_string( g_scrCtx, weapon ? weapon : "" );
    /* vDir */
    if ( dir ) { VectorCopy( dir, vdir ); }
    gsc_add_vec3( g_scrCtx, vdir );
    /* sHitLoc */
    gsc_add_string( g_scrCtx, G_GetHitLocationString( hitLoc ) );

    {
        int status = gsc_call( g_scrCtx, g_scrCallbackFile,
                               "CodeCallback_PlayerKilled", 7 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in CodeCallback_PlayerKilled (status %d)\n",
                      status );
        }
    }
}

/*
 * CoD1 CodeCallback_PlayerDamage(eInflictor, eAttacker, iDamage, iDFlags,
 *                                sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc)
 * 9 args
 */
void G_Scr_PlayerDamage( int clientNum, gentity_t *inflictor, gentity_t *attacker,
                         int damage, int dflags, int mod, const char *weapon,
                         vec3_t point, vec3_t dir, int hitLoc )
{
    int attackerNum;
    float vpoint[3] = { 0, 0, 0 };
    float vdir[3] = { 0, 0, 0 };

    if ( !g_scrActive || !g_scrCtx ) {
        return;
    }
    if ( !g_scrCallbackFile[0] ) {
        return;
    }

    attackerNum = ( attacker && attacker->client )
                  ? attacker->s.number : ENTITYNUM_WORLD;

    G_Scr_SetSelfToPlayer( clientNum );

    /* eInflictor */
    if ( inflictor && g_scrPlayerRefs[ inflictor->s.number ] != GSC_NOREF ) {
        gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ inflictor->s.number ] );
    } else {
        gsc_add_int( g_scrCtx, inflictor ? inflictor->s.number : ENTITYNUM_WORLD );
    }
    /* eAttacker */
    if ( attacker && attacker->client
         && g_scrPlayerRefs[ attackerNum ] != GSC_NOREF ) {
        gsc_push_ref( g_scrCtx, g_scrPlayerRefs[ attackerNum ] );
    } else {
        gsc_add_int( g_scrCtx, attackerNum );
    }
    /* iDamage */
    gsc_add_int( g_scrCtx, damage );
    /* iDFlags */
    gsc_add_int( g_scrCtx, dflags );
    /* sMeansOfDeath */
    if ( mod >= 0 && mod < MOD_NUM ) {
        gsc_add_string( g_scrCtx, modNames[mod] );
    } else {
        gsc_add_string( g_scrCtx, "MOD_UNKNOWN" );
    }
    /* sWeapon */
    gsc_add_string( g_scrCtx, weapon ? weapon : "" );
    /* vPoint */
    if ( point ) { VectorCopy( point, vpoint ); }
    gsc_add_vec3( g_scrCtx, vpoint );
    /* vDir */
    if ( dir ) { VectorCopy( dir, vdir ); }
    gsc_add_vec3( g_scrCtx, vdir );
    /* sHitLoc */
    gsc_add_string( g_scrCtx, G_GetHitLocationString( hitLoc ) );

    {
        int status = gsc_call( g_scrCtx, g_scrCallbackFile,
                               "CodeCallback_PlayerDamage", 9 );
        if ( status != GSC_OK && status != GSC_NOT_FOUND ) {
            G_Printf( "GSC: error in CodeCallback_PlayerDamage (status %d)\n",
                      status );
        }
    }
}

#endif /* STANDALONE */
