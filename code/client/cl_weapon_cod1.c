/*
===========================================================================
cl_weapon_cod1.c  --  Client-side CoD1 weapon system

Handles:
  - Weapon equipping from weapon files (weapons/sp or weapons/mp)
  - Clip/reserve ammo tracking and reload cycle
  - Animation state machine (idle, fire, reload, raise, drop, melee, ADS)
  - Aim Down Sights (ADS): FOV interpolation, overlay shader
  - Melee attack (plays anim; server applies damage via BUTTON_MELEE)
  - Viewmodel rendering (second RE_RenderScene pass, RDF_NOWORLDMODEL)
  - Ammo HUD display

Console commands:
  give <name>          e.g. "give mp44_mp"
  dropweapon           holster current weapon
  weaponinfo <name>    print weapon stats

Cvars:
  cl_startWeapon       weapon to auto-equip on spawn     (default "mp44_mp")
  cl_gunFov            viewmodel field of view (degrees) (default 65)
  cl_drawGun           0 = hide viewmodel                (default 1)
===========================================================================
*/
#include "client.h"
#include "../qcommon/bg_weapon_cod1.h"


/* ===========================================================================
   Cvars
   =========================================================================== */

static cvar_t *cl_startWeapon;
static cvar_t *cl_gunFov;
static cvar_t *cl_drawGun;
static cvar_t *cl_animDebug;
/* Manual position fine-tune (view-space, same convention as CoD cg_gun_x/y/z) */
static cvar_t *cl_gunX;        /* right (+) / left (-)  */
static cvar_t *cl_gunY;        /* forward (+) / back (-) */
static cvar_t *cl_gunZ;        /* up (+) / down (-)     */

/* ===========================================================================
   Weapon animation states
   =========================================================================== */

#define WA_IDLE         0
#define WA_FIRE         1
#define WA_RELOAD       2
#define WA_RELOAD_EMPTY 3
#define WA_RAISE        4
#define WA_DROP         5
#define WA_MELEE        6
#define WA_ADS_FIRE     7
#define WA_ADS_UP       8
#define WA_ADS_DOWN     9
#define WA_LAST_SHOT    10
#define WA_EMPTY_IDLE   11
#define WA_COUNT        12

/* ===========================================================================
   Weapon state struct
   =========================================================================== */

typedef struct {
    qboolean    active;
    char        name[64];
    weaponDef_t def;

    qhandle_t   gunModel;
    qhandle_t   handModel;
    qhandle_t   worldModel;

    qhandle_t   anims[WA_COUNT];
    int         currentAnim;
    int         animStartTime;

    /* Ammo */
    int         clipAmmo;       /* rounds remaining in current magazine */
    int         reserveAmmo;    /* total reserve ammo                   */

    /* Reload state */
    qboolean    reloadAmmoAdded; /* qtrue after ammo is added mid-reload  */

    /* Fire timing */
    int         nextFireTime;   /* cls.realtime when we can fire next    */
    int         nextMeleeTime;  /* cls.realtime when we can melee next   */

    /* ADS */
    qboolean    adsActive;      /* currently aiming down sights          */
    float       adsFrac;        /* 0.0=hip, 1.0=full ADS                 */
    qhandle_t   adsOverlay;     /* registered overlay shader handle      */
} clWeapon_t;

static clWeapon_t cl_weapon;

/* Pending weapon switch: stores next weapon while drop anim plays */
static char  s_pendingWeapon[64];
static int   s_pendingClipAmmo   = -1;
static int   s_pendingReserveAmmo = -1;

/* Track player state to detect spawns */
static int  s_prevPmType  = -1;
static int  s_spawnSerial =  0;
static int  s_lastThinkTime = 0;   /* for dt computation */

/* ===========================================================================
   Helpers
   =========================================================================== */

static qhandle_t LoadAnim( const char *animName )
{
    if ( !animName || !animName[0] ) return 0;
    if ( !re.RegisterXAnim ) return 0;
    return re.RegisterXAnim( animName );
}

static qhandle_t LoadXModel( const char *shortName )
{
    char path[MAX_QPATH];
    if ( !shortName || !shortName[0] ) return 0;
    if ( Q_strncmp( shortName, "xmodel/", 7 ) == 0 )
        Q_strncpyz( path, shortName, sizeof(path) );
    else
        Com_sprintf( path, sizeof(path), "xmodel/%s", shortName );
    return re.RegisterModel( path );
}

static float AnimDuration( int animSlot )
{
    qhandle_t h   = cl_weapon.anims[animSlot];
    int       fps = re.XAnimFramerate ? re.XAnimFramerate(h) : 30;
    int       nf  = re.XAnimNumFrames  ? re.XAnimNumFrames(h)  : 1;
    if ( fps <= 0 ) fps = 30;
    if ( nf  <= 0 ) nf  = 1;
    return (float)nf / (float)fps;
}

static void SetAnim( int anim )
{
    cl_weapon.currentAnim   = anim;
    cl_weapon.animStartTime = cls.realtime;
}

/* ===========================================================================
   CL_GiveWeapon / CL_WeaponPickup
   =========================================================================== */

void CL_GiveWeapon( const char *name )
{
    weaponDef_t *d;
    char         tryName[64];

    if ( !name || !name[0] ) return;
    if ( !re.RegisterModel ) {
        Com_Printf( "give: renderer not ready\n" );
        return;
    }

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    Q_strncpyz( cl_weapon.name, name, sizeof(cl_weapon.name) );

    if ( !BG_ParseWeaponDef( name, &cl_weapon.def ) ) {
        if ( !strstr( name, "_mp" ) && !strstr( name, "_sp" ) ) {
            Com_sprintf( tryName, sizeof(tryName), "%s_mp", name );
            if ( !BG_ParseWeaponDef( tryName, &cl_weapon.def ) ) {
                Com_Printf( "give: weapon '%s' not found (tried %s and %s)\n",
                            name, name, tryName );
                return;
            }
            Q_strncpyz( cl_weapon.name, tryName, sizeof(cl_weapon.name) );
        } else {
            Com_Printf( "give: weapon '%s' not found\n", name );
            return;
        }
    }
    d = &cl_weapon.def;

    /* Models */
    cl_weapon.gunModel   = LoadXModel( d->gunModel  );
    cl_weapon.handModel  = LoadXModel( d->handModel );
    if ( d->worldModel[0] )
        cl_weapon.worldModel = re.RegisterModel( d->worldModel );

    /* Animations */
    cl_weapon.anims[WA_IDLE]         = LoadAnim( d->anims.idleAnim        );
    cl_weapon.anims[WA_FIRE]         = LoadAnim( d->anims.fireAnim        );
    cl_weapon.anims[WA_RELOAD]       = LoadAnim( d->anims.reloadAnim      );
    cl_weapon.anims[WA_RELOAD_EMPTY] = LoadAnim( d->anims.reloadEmptyAnim );
    cl_weapon.anims[WA_RAISE]        = LoadAnim( d->anims.raiseAnim       );
    cl_weapon.anims[WA_DROP]         = LoadAnim( d->anims.dropAnim        );
    cl_weapon.anims[WA_MELEE]        = LoadAnim( d->anims.meleeAnim       );
    cl_weapon.anims[WA_ADS_FIRE]     = LoadAnim( d->anims.adsFireAnim     );
    cl_weapon.anims[WA_ADS_UP]       = LoadAnim( d->anims.adsUpAnim       );
    cl_weapon.anims[WA_ADS_DOWN]     = LoadAnim( d->anims.adsDownAnim     );
    cl_weapon.anims[WA_LAST_SHOT]    = LoadAnim( d->anims.lastShotAnim    );
    cl_weapon.anims[WA_EMPTY_IDLE]   = LoadAnim( d->anims.emptyIdleAnim   );

    /* ADS overlay */
    if ( d->adsOverlayShader[0] )
        cl_weapon.adsOverlay = re.RegisterShader( d->adsOverlayShader );

    /* Initial ammo from weapon def */
    cl_weapon.clipAmmo    = d->clipSize;
    cl_weapon.reserveAmmo = d->startAmmo;

    /* Start with raise animation if available */
    SetAnim( cl_weapon.anims[WA_RAISE] ? WA_RAISE : WA_IDLE );
    cl_weapon.active = qtrue;

    Com_Printf( "Weapon: %s  gun=%s hands=%s  clip=%d reserve=%d\n",
                cl_weapon.name,
                d->gunModel[0]  ? d->gunModel  : "(none)",
                d->handModel[0] ? d->handModel : "(none)",
                cl_weapon.clipAmmo, cl_weapon.reserveAmmo );
}

/*
 * CL_WeaponPickup -- called when the server notifies us of a weapon pickup.
 * clipAmmo/reserveAmmo of -1 means use weapon def defaults.
 */
void CL_WeaponPickup( const char *name, int clipAmmo, int reserveAmmo )
{
    if ( !name || !name[0] ) return;

    /* "none" = holster current weapon */
    if ( !Q_stricmp( name, "none" ) ) {
        if ( cl_weapon.active && cl_weapon.anims[WA_DROP] ) {
            SetAnim( WA_DROP );
        } else {
            Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
        }
        s_pendingWeapon[0] = '\0';
        return;
    }

    /* If already holding a different weapon, play drop first then switch */
    if ( cl_weapon.active && Q_stricmp( cl_weapon.name, name ) != 0 ) {
        /* Different weapon — queue the switch */
        if ( cl_weapon.anims[WA_DROP] ) {
            Q_strncpyz( s_pendingWeapon, name, sizeof(s_pendingWeapon) );
            s_pendingClipAmmo    = clipAmmo;
            s_pendingReserveAmmo = reserveAmmo;
            SetAnim( WA_DROP );
            return;
        }
        /* No drop anim, switch immediately */
    }

    CL_GiveWeapon( name );

    /* Override ammo if server supplied explicit values */
    if ( cl_weapon.active ) {
        if ( clipAmmo    >= 0 ) cl_weapon.clipAmmo    = clipAmmo;
        if ( reserveAmmo >= 0 ) cl_weapon.reserveAmmo = reserveAmmo;
    }
}

/*
 * CL_WeaponAmmoSync -- server pushes authoritative ammo counts.
 * Keeps client-side ammo in sync with server-side tracking.
 */
void CL_WeaponAmmoSync( int clipAmmo, int reserveAmmo )
{
    if ( !cl_weapon.active ) return;
    if ( clipAmmo    >= 0 ) cl_weapon.clipAmmo    = clipAmmo;
    if ( reserveAmmo >= 0 ) cl_weapon.reserveAmmo = reserveAmmo;
}

/* ===========================================================================
   Animation state machine helpers
   =========================================================================== */

static qboolean IsIdleAnim( int anim )
{
    return ( anim == WA_IDLE || anim == WA_EMPTY_IDLE );
}

static float ElapsedSec( void )
{
    return (float)( cls.realtime - cl_weapon.animStartTime ) / 1000.0f;
}

/* Start a reload.  Picks empty or normal reload anim automatically. */
static void StartReload( void )
{
    qboolean empty = ( cl_weapon.clipAmmo == 0 );
    int      anim  = empty ? WA_RELOAD_EMPTY : WA_RELOAD;
    /* fall back to the other if only one anim exists */
    if ( !cl_weapon.anims[anim] && cl_weapon.anims[anim ^ 1] )
        anim = anim ^ 1;
    if ( !cl_weapon.anims[anim] ) return;
    cl_weapon.reloadAmmoAdded = qfalse;
    SetAnim( anim );
}

/* Start melee */
static void StartMelee( void )
{
    if ( !cl_weapon.anims[WA_MELEE] ) return;
    cl_weapon.nextMeleeTime = cls.realtime +
        (int)( cl_weapon.def.meleeTime * 1000.0f );
    SetAnim( WA_MELEE );
}

/* Fire one round */
static void FireRound( qboolean inAds )
{
    int fireAnim;

    if ( cl_weapon.clipAmmo <= 0 ) return;

    cl_weapon.clipAmmo--;
    cl_weapon.nextFireTime = cls.realtime +
        (int)( cl_weapon.def.fireTime * 1000.0f );

    if ( cl_weapon.clipAmmo == 0 ) {
        /* Last shot in the clip */
        fireAnim = ( inAds && cl_weapon.anims[WA_ADS_FIRE] )
                   ? WA_ADS_FIRE : WA_LAST_SHOT;
        if ( !cl_weapon.anims[fireAnim] )
            fireAnim = WA_FIRE;
    } else {
        fireAnim = ( inAds && cl_weapon.anims[WA_ADS_FIRE] )
                   ? WA_ADS_FIRE : WA_FIRE;
    }
    if ( !cl_weapon.anims[fireAnim] )
        fireAnim = WA_IDLE;

    SetAnim( fireAnim );
}

/* ===========================================================================
   CL_WeaponThink  -- called every frame from CL_DrawViewModel
   =========================================================================== */

static void CL_WeaponThink( void )
{
    int       curPmType;
    const char *startWeap;
    usercmd_t *cmd;
    qboolean   wantFire, wantReload, wantMelee, wantAds;
    float      elapsed;
    float      addTime;
    int        needed, transferred;

    /* ---- Spawn detection ---- */
    if ( !cl.snap.valid ) {
        s_prevPmType = -1;
        return;
    }

    curPmType = cl.snap.ps.pm_type;
    if ( curPmType == 0 && s_prevPmType != 0 ) {
        s_spawnSerial++;
        startWeap = ( cl_startWeapon && cl_startWeapon->string[0] )
                    ? cl_startWeapon->string : "";
        if ( startWeap[0] ) {
            Com_Printf( "Auto-equipping '%s' on spawn...\n", startWeap );
            CL_GiveWeapon( startWeap );
        }
    }
    s_prevPmType = curPmType;

    if ( !cl_weapon.active ) return;

    /* ---- Read current input state ---- */
    cmd = &cl.cmds[ (cl.cmdNumber - 1) & (CMD_BACKUP - 1) ];
    wantFire   = ( cmd->buttons & BUTTON_ATTACK  ) ? qtrue : qfalse;
    wantReload = ( cmd->buttons & BUTTON_RELOAD  ) ? qtrue : qfalse;
    wantMelee  = ( cmd->buttons & BUTTON_MELEE   ) ? qtrue : qfalse;
    /* CoD1 ADS: either +ads button OR "toggle cl_run" (BUTTON_WALKING) */
    wantAds    = ( cmd->buttons & ( BUTTON_ADS | BUTTON_WALKING ) ) ? qtrue : qfalse;

    /* ---- ADS fraction interpolation ---- */
    {
        float dt;
        float inRate, outRate;
        if ( s_lastThinkTime > 0 )
            dt = (float)( cls.realtime - s_lastThinkTime ) / 1000.0f;
        else
            dt = 0.016f;
        s_lastThinkTime = cls.realtime;

        inRate  = ( cl_weapon.def.adsTransInTime  > 0.001f )
                  ? ( 1.0f / cl_weapon.def.adsTransInTime  ) : 8.0f;
        outRate = ( cl_weapon.def.adsTransOutTime > 0.001f )
                  ? ( 1.0f / cl_weapon.def.adsTransOutTime ) : 8.0f;
        if ( wantAds )
            cl_weapon.adsFrac += dt * inRate;
        else
            cl_weapon.adsFrac -= dt * outRate;
        if ( cl_weapon.adsFrac < 0.0f ) cl_weapon.adsFrac = 0.0f;
        if ( cl_weapon.adsFrac > 1.0f ) cl_weapon.adsFrac = 1.0f;
    }

    /* ---- ADS active flag ---
     * adsActive just gates firing/reload/melee while in ADS.
     * The animation is driven purely by adsFrac in CL_DrawViewModel —
     * no WA_ADS_UP / WA_ADS_DOWN states needed in the state machine.
     */
    cl_weapon.adsActive = ( cl_weapon.adsFrac > 0.0f ) ? qtrue : qfalse;

    /* Force adsFrac to zero while firing/reloading/meleeing */
    if ( cl_weapon.currentAnim == WA_FIRE || cl_weapon.currentAnim == WA_RELOAD ||
         cl_weapon.currentAnim == WA_RELOAD_EMPTY || cl_weapon.currentAnim == WA_MELEE ) {
        cl_weapon.adsFrac   = 0.0f;
        cl_weapon.adsActive = qfalse;
    }

    /* ---- Per-state logic ---- */
    switch ( cl_weapon.currentAnim )
    {
    case WA_RAISE:
        if ( ElapsedSec() >= AnimDuration( WA_RAISE ) )
            SetAnim( cl_weapon.clipAmmo > 0 ? WA_IDLE : WA_EMPTY_IDLE );
        break;

    case WA_DROP:
        if ( ElapsedSec() >= AnimDuration( WA_DROP ) ) {
            cl_weapon.active = qfalse;
            /* If a weapon switch was pending, load the new weapon now */
            if ( s_pendingWeapon[0] ) {
                CL_GiveWeapon( s_pendingWeapon );
                if ( cl_weapon.active ) {
                    if ( s_pendingClipAmmo    >= 0 ) cl_weapon.clipAmmo    = s_pendingClipAmmo;
                    if ( s_pendingReserveAmmo >= 0 ) cl_weapon.reserveAmmo = s_pendingReserveAmmo;
                }
                s_pendingWeapon[0]    = '\0';
                s_pendingClipAmmo     = -1;
                s_pendingReserveAmmo  = -1;
            }
        }
        break;

    case WA_IDLE:
    case WA_EMPTY_IDLE:
        /* --- Priority: melee > fire > reload --- */
        if ( wantMelee && cls.realtime >= cl_weapon.nextMeleeTime ) {
            StartMelee();
        } else if ( wantFire && cls.realtime >= cl_weapon.nextFireTime
                    && cl_weapon.clipAmmo > 0 ) {
            FireRound( cl_weapon.adsActive );
        } else if ( wantReload
                    && cl_weapon.clipAmmo < cl_weapon.def.clipSize
                    && cl_weapon.reserveAmmo > 0 ) {
            StartReload();
        }
        break;

    case WA_FIRE:
    case WA_ADS_FIRE:
        elapsed = ElapsedSec();
        if ( elapsed >= AnimDuration( cl_weapon.currentAnim ) ) {
            /* Auto-fire: keep firing if button held and ammo available */
            if ( wantFire && cl_weapon.clipAmmo > 0
                 && cls.realtime >= cl_weapon.nextFireTime ) {
                FireRound( cl_weapon.adsActive );
            } else {
                SetAnim( cl_weapon.clipAmmo > 0 ? WA_IDLE : WA_EMPTY_IDLE );
            }
        }
        break;

    case WA_LAST_SHOT:
        if ( ElapsedSec() >= AnimDuration( WA_LAST_SHOT ) ) {
            SetAnim( WA_EMPTY_IDLE );
            /* Auto-reload if reserve available */
            if ( cl_weapon.reserveAmmo > 0 )
                StartReload();
        }
        break;

    case WA_RELOAD:
    case WA_RELOAD_EMPTY:
        elapsed  = ElapsedSec();
        /* Determine when ammo is actually added during the reload cycle */
        addTime  = ( cl_weapon.currentAnim == WA_RELOAD_EMPTY )
                   ? cl_weapon.def.reloadEmptyTime
                   : cl_weapon.def.reloadAddTime;
        /* Fall back to total reload time if no addTime defined */
        if ( addTime <= 0.001f ) {
            addTime = ( cl_weapon.currentAnim == WA_RELOAD_EMPTY )
                      ? cl_weapon.def.reloadEmptyTime
                      : cl_weapon.def.reloadTime;
        }

        if ( !cl_weapon.reloadAmmoAdded && elapsed >= addTime ) {
            needed      = cl_weapon.def.clipSize - cl_weapon.clipAmmo;
            transferred = ( cl_weapon.reserveAmmo < needed )
                          ? cl_weapon.reserveAmmo : needed;
            cl_weapon.clipAmmo    += transferred;
            cl_weapon.reserveAmmo -= transferred;
            cl_weapon.reloadAmmoAdded = qtrue;

            /* Notify server of reload ammo transfer so server-side
             * weapon slots stay in sync (server is authoritative for firing) */
            CL_AddReliableCommand(
                va( "rld %d %d", cl_weapon.clipAmmo, cl_weapon.reserveAmmo ),
                qfalse );
        }

        {
            float totalTime = ( cl_weapon.currentAnim == WA_RELOAD_EMPTY )
                              ? cl_weapon.def.reloadEmptyTime
                              : cl_weapon.def.reloadTime;
            if ( totalTime <= 0.001f ) totalTime = 2.0f;
            if ( elapsed >= totalTime ) {
                SetAnim( cl_weapon.clipAmmo > 0 ? WA_IDLE : WA_EMPTY_IDLE );
            }
        }
        break;

    case WA_MELEE:
        if ( ElapsedSec() >= AnimDuration( WA_MELEE ) ) {
            SetAnim( cl_weapon.clipAmmo > 0 ? WA_IDLE : WA_EMPTY_IDLE );
        }
        break;

    default:
        SetAnim( WA_IDLE );
        break;
    }
}

/* ===========================================================================
   CL_WeaponCurrentAnimFrame
   =========================================================================== */

/*
 * CL_WeaponCurrentAnimFrame
 *
 * Returns the animation handle and frame to render for the current state.
 * ADS (adsUp / adsDown) are driven purely by adsFrac — exactly like CoD2's
 * XAnimSetGoalWeight / XAnimSetTime approach:
 *
 *   Entering ADS (wantAds=true):   play adsUp,   frame = adsFrac * (nf-1)
 *   Exiting  ADS (wantAds=false):  play adsDown, frame = (1-adsFrac) * (nf-1)
 *   Fully in ADS (adsFrac=1.0):    adsUp last frame held
 *   Fully hip    (adsFrac=0.0):    normal currentAnim
 */
float CL_WeaponCurrentAnimFrame( void )
{
    qhandle_t h;
    int       nf;
    float     elapsed, frame;

    /* ADS: override anim selection based on adsFrac */
    if ( cl_weapon.adsFrac > 0.0f ) {
        usercmd_t *cmd = &cl.cmds[ (cl.cmdNumber - 1) & (CMD_BACKUP - 1) ];
        qboolean wantAds = ( cmd->buttons & ( BUTTON_ADS | BUTTON_WALKING ) ) ? qtrue : qfalse;

        if ( wantAds && cl_weapon.anims[WA_ADS_UP] ) {
            h  = cl_weapon.anims[WA_ADS_UP];
            nf = re.XAnimNumFrames ? re.XAnimNumFrames(h) : 1;
            if ( nf < 1 ) nf = 1;
            /* Drive frame from adsFrac: 0→first, 1→last (held) */
            cl_weapon.currentAnim = WA_ADS_UP;
            return cl_weapon.adsFrac * (float)( nf - 1 );
        } else if ( !wantAds && cl_weapon.anims[WA_ADS_DOWN] ) {
            h  = cl_weapon.anims[WA_ADS_DOWN];
            nf = re.XAnimNumFrames ? re.XAnimNumFrames(h) : 1;
            if ( nf < 1 ) nf = 1;
            /* adsFrac going 1→0: play adsDown 0→last as adsFrac falls */
            cl_weapon.currentAnim = WA_ADS_DOWN;
            return ( 1.0f - cl_weapon.adsFrac ) * (float)( nf - 1 );
        }
        /* If no ADS anims, fall through to normal anim */
    }

    /* Normal (hip) animation */
    h  = cl_weapon.anims[cl_weapon.currentAnim];
    nf = re.XAnimNumFrames ? re.XAnimNumFrames(h) : 1;
    if ( nf < 1 ) nf = 1;

    elapsed = (float)( cls.realtime - cl_weapon.animStartTime ) / 1000.0f;
    frame   = elapsed * (float)( re.XAnimFramerate ? re.XAnimFramerate(h) : 30 );
    if ( frame < 0 ) frame = 0;

    if ( IsIdleAnim( cl_weapon.currentAnim ) ) {
        frame = ( nf > 1 ) ? (float)fmod( (double)frame, (double)nf ) : 0.0f;
    } else {
        if ( frame >= (float)nf ) frame = (float)( nf - 1 );
    }
    return frame;
}

/* ===========================================================================
   CL_DrawWeaponHud  -- ammo counter
   =========================================================================== */

void CL_DrawWeaponHud( void )
{
    char        buf[32];
    int         cx, cy;
    int         sw, sh;
    float       white[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    float       yellow[4] = { 1.0f, 0.8f, 0.2f, 1.0f };
    float       red[4]    = { 1.0f, 0.2f, 0.2f, 1.0f };
    qhandle_t   whiteShader;

    if ( !cl.snap.valid )           return;
    if ( cl.snap.ps.pm_type != 0 )  return;

    sw = cls.glconfig.vidWidth;
    sh = cls.glconfig.vidHeight;

    /* ---- Crosshair (simple + shape, hidden during ADS with overlay) ---- */
    if ( !( cl_weapon.adsActive && cl_weapon.adsOverlay ) ) {
        int hlen = 10, gap = 3, thick = 2;
        whiteShader = re.RegisterShader( "white" );
        re.SetColor( white );
        cx = sw / 2;
        cy = sh / 2;
        /* horizontal arms */
        re.DrawStretchPic( (float)(cx - hlen - gap), (float)(cy - thick/2),
                           (float)hlen, (float)thick,
                           0,0,1,1, whiteShader );
        re.DrawStretchPic( (float)(cx + gap),        (float)(cy - thick/2),
                           (float)hlen, (float)thick,
                           0,0,1,1, whiteShader );
        /* vertical arms */
        re.DrawStretchPic( (float)(cx - thick/2), (float)(cy - hlen - gap),
                           (float)thick, (float)hlen,
                           0,0,1,1, whiteShader );
        re.DrawStretchPic( (float)(cx - thick/2), (float)(cy + gap),
                           (float)thick, (float)hlen,
                           0,0,1,1, whiteShader );
        re.SetColor( NULL );
    }

    if ( !cl_weapon.active ) return;

    /* ---- Ammo counter (bottom-right, CoD1 style) ---- */
    {
        float *ammoColor = white;
        if ( cl_weapon.clipAmmo == 0 )
            ammoColor = red;
        else if ( cl_weapon.clipAmmo <= cl_weapon.def.clipSize / 4 )
            ammoColor = yellow;

        /* Clip count — large */
        Com_sprintf( buf, sizeof(buf), "%d", cl_weapon.clipAmmo );
        SCR_DrawBigString( sw - 140, sh - 52, buf, ammoColor[3], qtrue );

        /* Separator + reserve — small */
        Com_sprintf( buf, sizeof(buf), "/ %d", cl_weapon.reserveAmmo );
        SCR_DrawSmallStringExt( sw - 80, sh - 36, buf, white, qfalse, qtrue );
    }
}

/* ===========================================================================
   CL_DrawViewModel
   =========================================================================== */

static void AddViewmodelEnt( qhandle_t hModel,
                              const vec3_t origin, vec3_t axis[3] )
{
    refEntity_t ent;
    Com_Memset( &ent, 0, sizeof(ent) );
    ent.reType   = RT_MODEL;
    ent.hModel   = hModel;
    ent.renderfx = RF_DEPTHHACK;
    VectorCopy( origin, ent.origin    );
    VectorCopy( origin, ent.oldorigin );
    AxisCopy( axis, ent.axis );
    re.AddRefEntityToScene( &ent );
}

/*
 * CL_WeaponBobSway -- add movement bob and idle sway to the viewmodel.
 *
 * Reference: GAME_MP_.c BG_CalculateWeaponPosition_Sway / BG_CalculateWeaponAngles
 *
 * Bob: sinusoidal position offset driven by player speed (footstep cycle).
 * Sway: slow sine-based idle drift scaled by weapon def sway parameters.
 * Both scale down when ADS is active (adsBobFactor).
 */
static void CL_WeaponBobSway( vec3_t origin, vec3_t viewAngles, vec3_t axis[3] )
{
    float   speed, bobCycle, bobScale;
    float   swayScale, swayT;
    float   adsMult;
    vec3_t  vel;

    if ( !cl.snap.valid ) return;

    /* Player velocity magnitude (horizontal only) */
    VectorCopy( cl.snap.ps.velocity, vel );
    vel[2] = 0;
    speed = VectorLength( vel );

    /* ADS reduces bob/sway */
    adsMult = 1.0f - cl_weapon.adsFrac * 0.8f;
    if ( cl_weapon.def.adsBobFactor > 0.001f )
        adsMult *= cl_weapon.def.adsBobFactor + ( 1.0f - cl_weapon.def.adsBobFactor ) * ( 1.0f - cl_weapon.adsFrac );

    /* --- Movement bob --- */
    if ( speed > 10.0f && cl.snap.ps.groundEntityNum != ENTITYNUM_NONE ) {
        bobCycle = (float)cls.realtime * 0.006f; /* footstep frequency */
        bobScale = speed * 0.0004f * adsMult;
        if ( bobScale > 0.012f ) bobScale = 0.012f;

        /* lateral bob */
        VectorMA( origin, (float)sin( bobCycle ) * bobScale, axis[1], origin );
        /* vertical bob (2x frequency) */
        VectorMA( origin, (float)sin( bobCycle * 2.0f ) * bobScale * 0.6f, axis[2], origin );
    }

    /* --- Idle sway --- */
    {
        float swayMax = cl_weapon.def.swayMaxAngle;
        float swaySpd = cl_weapon.def.swayLerpSpeed;
        if ( swayMax <= 0.0f ) swayMax = 1.0f;
        if ( swaySpd <= 0.0f ) swaySpd = 1.0f;

        swayScale = swayMax * adsMult;
        swayT = (float)cls.realtime * 0.001f * swaySpd;

        /* pitch sway */
        viewAngles[PITCH] += (float)sin( swayT * 0.7f ) * swayScale *
            ( cl_weapon.def.swayPitchScale > 0.0f ? cl_weapon.def.swayPitchScale : 0.04f );
        /* yaw sway */
        viewAngles[YAW] += (float)sin( swayT * 1.1f ) * swayScale *
            ( cl_weapon.def.swayYawScale > 0.0f ? cl_weapon.def.swayYawScale : 0.03f );
    }
}

void CL_DrawViewModel( stereoFrame_t stereo )
{
    refdef_t    refdef;
    vec3_t      viewAngles;
    vec3_t      axis[3];
    float       gunFov, adsFov;
    float       animFrame;
    qhandle_t   curAnim;
    vec3_t      cameraOrigin, modelOrigin;
    vec3_t      entityAxis[3];
    qboolean    prone, ducked;
    float       ofsF, ofsR, ofsU;

    /* Per-frame weapon logic */
    CL_WeaponThink();

    if ( !cl_drawGun || !cl_drawGun->integer )   return;
    if ( !cl_weapon.active )                      return;
    if ( !cl_weapon.gunModel && !cl_weapon.handModel ) return;
    if ( !cl.snap.valid )                         return;
    if ( Cvar_VariableIntegerValue( "cg_thirdPerson" ) ) return;
    if ( cl.snap.ps.pm_type != 0 )                return;

    /* --- ADS overlay --- */
    if ( cl_weapon.adsActive && cl_weapon.adsFrac > 0.5f && cl_weapon.adsOverlay ) {
        re.SetColor( NULL );
        re.DrawStretchPic( 0, 0,
                           (float)cls.glconfig.vidWidth,
                           (float)cls.glconfig.vidHeight,
                           0, 0, 1, 1, cl_weapon.adsOverlay );
        /* When overlay is showing, skip the viewmodel */
        return;
    }

    /* --- Compute animation frame --- */
    animFrame = CL_WeaponCurrentAnimFrame();
    curAnim   = cl_weapon.anims[ cl_weapon.currentAnim ];

    /* --- Camera / view setup --- */
    ofsF = 0.0f; ofsR = 0.0f; ofsU = 0.0f;

    VectorCopy( cl.snap.ps.viewangles, viewAngles );
    AnglesToAxis( viewAngles, axis );

    VectorCopy( cl.snap.ps.origin, cameraOrigin );
    cameraOrigin[2] += cl.snap.ps.viewheight;

    prone  = ( cl.snap.ps.pm_flags & PMF_PRONE  ) ? qtrue : qfalse;
    ducked = ( cl.snap.ps.pm_flags & PMF_DUCKED ) ? qtrue : qfalse;

    if ( prone ) {
        ofsF = cl_weapon.def.proneOfsF;
        ofsR = cl_weapon.def.proneOfsR;
        ofsU = cl_weapon.def.proneOfsU;
    } else if ( ducked ) {
        ofsF = cl_weapon.def.duckedOfsF;
        ofsR = cl_weapon.def.duckedOfsR;
        ofsU = cl_weapon.def.duckedOfsU;
    } else {
        ofsF = cl_weapon.def.standMoveF;
        ofsR = cl_weapon.def.standMoveR;
        ofsU = cl_weapon.def.standMoveU;
    }

    ofsR += cl_gunX ? cl_gunX->value : 0.0f;
    ofsF += cl_gunY ? cl_gunY->value : 0.0f;
    ofsU += cl_gunZ ? cl_gunZ->value : 0.0f;

    VectorCopy( cameraOrigin, modelOrigin );
    VectorMA( modelOrigin,  ofsF, axis[0], modelOrigin );
    VectorMA( modelOrigin, -ofsR, axis[1], modelOrigin );
    VectorMA( modelOrigin,  ofsU, axis[2], modelOrigin );

    /* --- Weapon bob & sway --- */
    CL_WeaponBobSway( modelOrigin, viewAngles, axis );

    AnglesToAxis( viewAngles, entityAxis );

    /* --- DObj pose evaluation (must happen before tag_ads query) --- */
    if ( re.UpdateDObjPose && ( cl_weapon.handModel || cl_weapon.gunModel ) ) {
        re.UpdateDObjPose( cl_weapon.handModel, cl_weapon.gunModel,
                           curAnim, animFrame );
    }

    /* --- tag_ads camera offset ---
     *
     * CoD1 ADS works by repositioning the viewmodel so that the tag_ads bone
     * (the iron-sight / optic alignment point) sits exactly at the camera
     * origin.  We query the bone's local-space position from the current
     * animated pose, then shift modelOrigin by -(tag_ads_pos) * adsFrac
     * so the transition is smooth.
     *
     * The pose bones are stored in model-local space (root at origin), so
     * the returned tag->origin is the bone's offset from model origin.
     * We need to rotate that offset into world space using entityAxis before
     * subtracting it from modelOrigin.
     */
    if ( cl_weapon.adsFrac > 0.0f && re.LerpTag ) {
        orientation_t tagAds;
        qhandle_t     tagModel = cl_weapon.gunModel ? cl_weapon.gunModel : cl_weapon.handModel;

        Com_Memset( &tagAds, 0, sizeof(tagAds) );
        if ( re.LerpTag( &tagAds, tagModel, 0, 0, 0.0f, "tag_ads" ) ) {
            /* tagAds.origin is in model-local space; rotate to world */
            vec3_t tagWorld;
            tagWorld[0] =  DotProduct( tagAds.origin, entityAxis[0] );
            tagWorld[1] = -DotProduct( tagAds.origin, entityAxis[1] );
            tagWorld[2] =  DotProduct( tagAds.origin, entityAxis[2] );
            /* Shift model so tag_ads lands at camera origin */
            VectorMA( modelOrigin, -cl_weapon.adsFrac, tagWorld, modelOrigin );
        }
    }

    if ( cl_animDebug && cl_animDebug->integer )
        Com_Printf( "anim=%d frame=%.1f clip=%d res=%d ads=%.2f\n",
                    cl_weapon.currentAnim, animFrame,
                    cl_weapon.clipAmmo, cl_weapon.reserveAmmo,
                    cl_weapon.adsFrac );

    /* --- ADS FOV interpolation --- */
    gunFov = ( cl_gunFov && cl_gunFov->value > 1.0f ) ? cl_gunFov->value : 65.0f;
    adsFov = ( cl_weapon.def.adsZoomFov > 1.0f ) ? cl_weapon.def.adsZoomFov : gunFov * 0.6f;
    gunFov = gunFov + ( adsFov - gunFov ) * cl_weapon.adsFrac;

    /* --- Build refdef --- */
    Com_Memset( &refdef, 0, sizeof(refdef) );
    refdef.x       = 0;
    refdef.y       = 0;
    refdef.width   = cls.glconfig.vidWidth;
    refdef.height  = cls.glconfig.vidHeight;
    refdef.time    = cl.serverTime;
    refdef.rdflags = RDF_NOWORLDMODEL;

    refdef.fov_x = gunFov;
    refdef.fov_y = 2.0f * (float)atan(
                        tan( gunFov * 0.5f * (float)(M_PI / 180.0) )
                        * (float)refdef.height / (float)refdef.width )
                   * (float)(180.0 / M_PI);

    VectorCopy( cameraOrigin, refdef.vieworg );
    AnglesToAxis( viewAngles, refdef.viewaxis );

    re.ClearScene();
    if ( cl_weapon.handModel )
        AddViewmodelEnt( cl_weapon.handModel, modelOrigin, entityAxis );
    if ( cl_weapon.gunModel )
        AddViewmodelEnt( cl_weapon.gunModel,  modelOrigin, entityAxis );

    re.RenderScene( &refdef );
}

/* ===========================================================================
   Console commands
   =========================================================================== */

static void CL_Give_f( void )
{
    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: give <weaponname>\n"
                    "  e.g.  give mp44_mp\n"
                    "  e.g.  give colt_mp\n"
                    "  e.g.  give mp40_mp\n" );
        return;
    }
    CL_GiveWeapon( Cmd_Argv(1) );
}

static void CL_DropWeapon_f( void )
{
    if ( cl_weapon.active )
        Com_Printf( "Holstered '%s'\n", cl_weapon.name );
    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
}

static void CL_WeaponInfo_f( void )
{
    weaponDef_t wd;
    const char *name;
    if ( Cmd_Argc() < 2 ) {
        Com_Printf( "Usage: weaponinfo <weaponname>\n" );
        return;
    }
    name = Cmd_Argv(1);
    if ( !BG_ParseWeaponDef( name, &wd ) ) {
        Com_Printf( "weaponinfo: '%s' not found\n", name );
        return;
    }
    Com_Printf( "=== %s ===\n", name );
    Com_Printf( "  type/class    : %s / %s\n", wd.weaponType, wd.weaponClass );
    Com_Printf( "  animType      : %s (%d)\n", wd.playerAnimTypeName, wd.playerAnimType );
    Com_Printf( "  gunModel      : %s\n", wd.gunModel   );
    Com_Printf( "  handModel     : %s\n", wd.handModel  );
    Com_Printf( "  worldModel    : %s\n", wd.worldModel );
    Com_Printf( "  damage        : %d  (melee %d)\n",   wd.damage, wd.meleeDamage );
    Com_Printf( "  clip / max    : %d / %d\n",          wd.clipSize, wd.maxAmmo );
    Com_Printf( "  fireTime      : %.3f s\n",           wd.fireTime    );
    Com_Printf( "  reloadTime    : %.3f s\n",           wd.reloadTime  );
    Com_Printf( "  meleeTime     : %.3f s\n",           wd.meleeTime   );
    Com_Printf( "  adsZoomFov    : %.1f deg\n",         wd.adsZoomFov  );
    Com_Printf( "  adsTransIn/Out: %.3f / %.3f s\n",
                wd.adsTransInTime, wd.adsTransOutTime );
    if ( wd.anims.idleAnim[0]   ) Com_Printf( "  idle     : %s\n", wd.anims.idleAnim   );
    if ( wd.anims.fireAnim[0]   ) Com_Printf( "  fire     : %s\n", wd.anims.fireAnim   );
    if ( wd.anims.reloadAnim[0] ) Com_Printf( "  reload   : %s\n", wd.anims.reloadAnim );
    if ( wd.anims.raiseAnim[0]  ) Com_Printf( "  raise    : %s\n", wd.anims.raiseAnim  );
    if ( wd.anims.meleeAnim[0]  ) Com_Printf( "  melee    : %s\n", wd.anims.meleeAnim  );
    if ( wd.anims.adsUpAnim[0]  ) Com_Printf( "  adsUp    : %s\n", wd.anims.adsUpAnim  );
}

/* ===========================================================================
   Init / Shutdown
   =========================================================================== */

void CL_WeaponCod1_Init( void )
{
    cl_startWeapon = Cvar_Get( "cl_startWeapon", "mp44_mp", CVAR_ARCHIVE );
    cl_gunFov      = Cvar_Get( "cl_gunFov",      "65",      CVAR_ARCHIVE );
    cl_drawGun     = Cvar_Get( "cl_drawGun",     "1",       CVAR_ARCHIVE );
    /* 0 = suppress Q3 cgame 2D (weapon name, ammo counter, machinegun HUD) */
    (void)Cvar_Get( "cl_drawGunPostHud", "0", CVAR_ARCHIVE );
    cl_animDebug   = Cvar_Get( "cl_animDebug",   "0",       CVAR_TEMP   );
    cl_gunX        = Cvar_Get( "cl_gunX",        "0",       CVAR_ARCHIVE );
    cl_gunY        = Cvar_Get( "cl_gunY",        "0",       CVAR_ARCHIVE );
    cl_gunZ        = Cvar_Get( "cl_gunZ",        "0",       CVAR_ARCHIVE );

    Cmd_AddCommand( "give",       CL_Give_f       );
    Cmd_AddCommand( "dropweapon", CL_DropWeapon_f );
    Cmd_AddCommand( "weaponinfo", CL_WeaponInfo_f );

    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    s_pendingWeapon[0]    = '\0';
    s_pendingClipAmmo     = -1;
    s_pendingReserveAmmo  = -1;
    s_prevPmType    = -1;
    s_spawnSerial   =  0;
    s_lastThinkTime =  0;
}

void CL_WeaponCod1_Shutdown( void )
{
    Cmd_RemoveCommand( "give"       );
    Cmd_RemoveCommand( "dropweapon" );
    Cmd_RemoveCommand( "weaponinfo" );
    Com_Memset( &cl_weapon, 0, sizeof(cl_weapon) );
    s_pendingWeapon[0]    = '\0';
    s_pendingClipAmmo     = -1;
    s_pendingReserveAmmo  = -1;
    s_prevPmType  = -1;
    s_lastThinkTime = 0;
}

/* ===========================================================================
   Accessors (used by cl_character_cod1.c etc.)
   =========================================================================== */

const char *CL_WeaponCurrentPlayerAnimTypeName( void )
{
    if ( !cl_weapon.active ) return "";
    if ( cl_weapon.def.playerAnimTypeName[0] )
        return cl_weapon.def.playerAnimTypeName;
    if ( cl_weapon.def.playerAnimType >= 0 )
        return BG_GetPlayerAnimTypeName( cl_weapon.def.playerAnimType );
    return "";
}

int CL_WeaponCurrentPlayerAnimType( void )
{
    if ( !cl_weapon.active ) return -1;
    return cl_weapon.def.playerAnimType;
}
