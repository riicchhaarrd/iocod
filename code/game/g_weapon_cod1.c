/*
===========================================================================
g_weapon_cod1.c  --  Server-side CoD1 weapon entities and melee

Handles:
  - Spawning weapon_* entities from CoD1 maps as pickupable world models
  - Sending pickup notification to the client (reliable "weapon" command)
  - Server-side melee damage trace (G_MeleeDamage)

CoD1 map entities look like:
  "classname" "weapon_mp44_mp"     or "weapon_mp44"
  "classname" "mpweapon_kar98k_mp" or "mpweapon_colt"
  "origin"    "512 -128 64"
  "angles"    "0 90 0"

The weapon name is derived from the classname by stripping the leading
"weapon_" or "mpweapon_" prefix.
===========================================================================
*/
#ifdef STANDALONE

#include "g_local.h"
#include "g_hitloc.h"
#include "../qcommon/bg_weapon_cod1.h"

/* Respawn time in seconds; 0 = do not respawn */
#define WEAPON_RESPAWN_TIME   20

/* Weapon name is re-derived from classname each time (classname pointer persists) */
static const char *WeaponNameFromClass( const char *classname )
{
    if ( !Q_stricmpn( classname, "mpweapon_", 9 ) ) return classname + 9;
    if ( !Q_stricmpn( classname, "weapon_",   7 ) ) return classname + 7;
    return classname;
}

/*
 * G_WeaponRespawn -- re-enable a weapon entity after respawn delay
 */
static void G_WeaponRespawn( gentity_t *ent )
{
    ent->r.svFlags &= ~SVF_NOCLIENT;
    ent->r.contents = CONTENTS_TRIGGER;
    ent->nextthink  = 0;
    ent->think      = NULL;
    trap_LinkEntity( ent );
}

/*
 * Touch_WeaponCod1 -- player walks into a weapon pickup trigger
 */
void Touch_WeaponCod1( gentity_t *ent, gentity_t *other, trace_t *trace )
{
    gclient_t  *client;
    const char *weapName;
    int         clipAmmo, reserveAmmo;

    if ( !other || !other->client ) return;
    client = other->client;

    /* Only alive players can pick up weapons */
    if ( client->ps.pm_type != PM_NORMAL ) return;

    weapName    = WeaponNameFromClass( ent->classname );
    clipAmmo    = ent->count;    /* clip size stored during spawn   */
    reserveAmmo = ent->damage;   /* reserve ammo stored during spawn */

    /* Store in player's weapon slots */
    {
        weaponDef_t wd;
        int slotIdx = 0, i;

        if ( BG_ParseWeaponDef( weapName, &wd ) ) {
            slotIdx = !Q_stricmp(wd.weaponSlot,"pistol") ? 2 :
                      !Q_stricmp(wd.weaponSlot,"grenade") ? 3 :
                      !Q_stricmp(wd.weaponSlot,"smokegrenade") ? 4 : 0;
        }
        /* if slot occupied, try primaryb or any empty */
        if ( client->weaponSlots[slotIdx].name[0] ) {
            if ( slotIdx == 0 && !client->weaponSlots[1].name[0] ) {
                slotIdx = 1;
            } else {
                for ( i = 5; i < COD1_WEAPON_SLOT_NUM; i++ ) {
                    if ( !client->weaponSlots[i].name[0] ) { slotIdx = i; break; }
                }
            }
        }
        Q_strncpyz( client->weaponSlots[slotIdx].name, weapName,
                    sizeof(client->weaponSlots[slotIdx].name) );
        client->weaponSlots[slotIdx].clipAmmo = clipAmmo;
        client->weaponSlots[slotIdx].reserveAmmo = reserveAmmo;
        client->currentWeaponSlot = slotIdx;
    }

    /* Send weapon pickup notification to the client */
    trap_SendServerCommand( other->s.number,
        va( "weapon %s %d %d", weapName, clipAmmo, reserveAmmo ) );

    /* Temporarily hide the entity and disable its trigger */
    ent->r.contents = 0;
    ent->r.svFlags |= SVF_NOCLIENT;
    trap_UnlinkEntity( ent );

    if ( WEAPON_RESPAWN_TIME > 0 ) {
        ent->think     = G_WeaponRespawn;
        ent->nextthink = level.time + WEAPON_RESPAWN_TIME * 1000;
    }
}

/*
 * SP_weapon_cod1 -- generic CoD1 weapon entity spawner.
 *
 * Called for any "weapon_*" or "mpweapon_*" classname.
 */
void SP_weapon_cod1( gentity_t *ent )
{
    const char *weapName;
    weaponDef_t wd;
    char        worldModel[MAX_QPATH];

    weapName = WeaponNameFromClass( ent->classname );

    /* Load weapon definition to get ammo and world model */
    Com_Memset( &wd, 0, sizeof(wd) );
    if ( !BG_ParseWeaponDef( weapName, &wd ) ) {
        G_Printf( "SP_weapon_cod1: no def for '%s'\n", weapName );
        ent->count  = 30;   /* default clip */
        ent->damage = 90;   /* default reserve */
    } else {
        ent->count  = wd.clipSize;
        ent->damage = wd.startAmmo;
    }

    /* Determine world model */
    worldModel[0] = '\0';
    if ( ent->model && ent->model[0] ) {
        Q_strncpyz( worldModel, ent->model, sizeof(worldModel) );
    } else if ( wd.worldModel[0] ) {
        Q_strncpyz( worldModel, wd.worldModel, sizeof(worldModel) );
    } else if ( wd.gunModel[0] ) {
        if ( Q_strncmp( wd.gunModel, "xmodel/", 7 ) == 0 )
            Q_strncpyz( worldModel, wd.gunModel, sizeof(worldModel) );
        else
            Com_sprintf( worldModel, sizeof(worldModel), "xmodel/%s", wd.gunModel );
    }

    if ( worldModel[0] )
        ent->s.modelindex = G_ModelIndex( worldModel );

    /* Pickup trigger bounds (ref: BG_PlayerTouchesItem ±36 horiz, -88/+18 vert) */
    VectorSet( ent->r.mins, -36, -36, -88 );
    VectorSet( ent->r.maxs,  36,  36,  18 );
    ent->r.contents = CONTENTS_TRIGGER;
    ent->touch      = Touch_WeaponCod1;

    G_SetOrigin( ent, ent->s.origin );
    VectorCopy( ent->s.angles, ent->s.apos.trBase );

    trap_LinkEntity( ent );

    G_Printf( "SP_weapon_cod1: '%s' -> weapon='%s' clip=%d res=%d model=%s\n",
                 ent->classname, weapName, ent->count, ent->damage,
                 worldModel[0] ? worldModel : "(none)" );
}

/* ===========================================================================
   G_DropCurrentWeapon -- drop current weapon as a pickup entity
   ===========================================================================
   Reference: GAME_MP_.c Drop_Weapon
   Creates a weapon_* entity at the player's position with random velocity,
   transferring current ammo from the player to the dropped item.
   =========================================================================== */

void G_DropCurrentWeapon( gentity_t *ent )
{
    gclient_t   *cl;
    int          slot;
    const char  *weapName;
    gentity_t   *drop;
    vec3_t       forward, vel;
    weaponDef_t  wd;
    char         classname[72];

    if ( !ent || !ent->client ) return;
    cl = ent->client;
    slot = cl->currentWeaponSlot;
    if ( slot < 0 || slot >= COD1_WEAPON_SLOT_NUM ) return;
    if ( !cl->weaponSlots[slot].name[0] ) return;

    weapName = cl->weaponSlots[slot].name;

    /* Spawn the dropped weapon entity */
    drop = G_Spawn();
    if ( !drop ) return;

    Com_sprintf( classname, sizeof(classname), "weapon_%s", weapName );
    drop->classname = G_NewString( classname );

    /* Transfer ammo from player to dropped item */
    drop->count  = cl->weaponSlots[slot].clipAmmo;
    drop->damage = cl->weaponSlots[slot].reserveAmmo;

    /* World model */
    Com_Memset( &wd, 0, sizeof(wd) );
    if ( BG_ParseWeaponDef( weapName, &wd ) && wd.worldModel[0] ) {
        drop->s.modelindex = G_ModelIndex( wd.worldModel );
    }

    /* Position at player center, toss forward with random arc */
    AngleVectors( ent->client->ps.viewangles, forward, NULL, NULL );
    VectorCopy( ent->r.currentOrigin, drop->s.origin );
    drop->s.origin[2] += ( ent->r.maxs[2] + ent->r.mins[2] ) * 0.5f;

    VectorScale( forward, 150.0f, vel );
    vel[2] += 200.0f + (float)( rand() % 100 - 50 );
    drop->s.pos.trType  = TR_GRAVITY;
    drop->s.pos.trTime  = level.time;
    VectorCopy( drop->s.origin, drop->s.pos.trBase );
    VectorCopy( vel, drop->s.pos.trDelta );

    /* Pickup trigger (ref: BG_PlayerTouchesItem ±36 horiz, -88/+18 vert) */
    VectorSet( drop->r.mins, -36, -36, -88 );
    VectorSet( drop->r.maxs,  36,  36,  18 );
    drop->r.contents = CONTENTS_TRIGGER;
    drop->touch      = Touch_WeaponCod1;
    drop->nextthink  = level.time + 30000; /* despawn after 30s */
    drop->think      = G_FreeEntity;

    G_SetOrigin( drop, drop->s.origin );
    trap_LinkEntity( drop );

    /* Clear the weapon from the player */
    cl->weaponSlots[slot].name[0]    = '\0';
    cl->weaponSlots[slot].clipAmmo   = 0;
    cl->weaponSlots[slot].reserveAmmo = 0;

    /* Switch to next available weapon */
    {
        int i;
        cl->currentWeaponSlot = -1;
        for ( i = 0; i < COD1_WEAPON_SLOT_NUM; i++ ) {
            if ( cl->weaponSlots[i].name[0] ) {
                cl->currentWeaponSlot = i;
                trap_SendServerCommand( ent->s.number,
                    va( "weapon %s %d %d",
                        cl->weaponSlots[i].name,
                        cl->weaponSlots[i].clipAmmo,
                        cl->weaponSlots[i].reserveAmmo ) );
                break;
            }
        }
        if ( cl->currentWeaponSlot < 0 ) {
            trap_SendServerCommand( ent->s.number, "weapon none 0 0" );
        }
    }
}

/* ===========================================================================
   G_DropWeaponsOnDeath -- drop all weapons when player dies
   ===========================================================================
   Reference: GAME_MP_.c Drop_Weapon (called from player_die path)
   Drops the current weapon as a pickup entity.
   =========================================================================== */

void G_DropWeaponsOnDeath( gentity_t *ent )
{
    if ( !ent || !ent->client ) return;
    if ( ent->client->currentWeaponSlot >= 0 &&
         ent->client->weaponSlots[ent->client->currentWeaponSlot].name[0] ) {
        G_DropCurrentWeapon( ent );
    }
}

/* ===========================================================================
   G_MeleeDamage -- server-side melee trace from player position
   ===========================================================================
   Called from ClientThink_real when BUTTON_MELEE is pressed.
   Traces a short box forward from the player's eye, applies damage to the
   first entity hit.
   =========================================================================== */

#define MELEE_RANGE     64.0f
#define MELEE_DAMAGE   150

void G_MeleeDamage( gentity_t *attacker )
{
    vec3_t    forward, right, up;
    vec3_t    muzzle, end;
    trace_t   tr;
    gentity_t *traceEnt;
    static const vec3_t meleeHalf = { 8, 8, 8 };

    if ( !attacker || !attacker->client ) return;

    AngleVectors( attacker->client->ps.viewangles, forward, right, up );

    VectorCopy( attacker->client->ps.origin, muzzle );
    muzzle[2] += attacker->client->ps.viewheight;

    VectorMA( muzzle, MELEE_RANGE, forward, end );

    trap_Trace( &tr, muzzle, (float*)meleeHalf, (float*)meleeHalf, end,
                attacker->s.number, MASK_SHOT );

    if ( tr.fraction >= 1.0f ) return;

    traceEnt = &g_entities[ tr.entityNum ];
    if ( !traceEnt->takedamage ) return;

    /* CoD1 melee damage: base + rand()%5 (ref: line 54942) */
    G_Damage( traceEnt, attacker, attacker, forward, tr.endpos,
              MELEE_DAMAGE + ( rand() % 5 ), 0, MOD_MELEE );
}

/* ===========================================================================
   G_FireWeapon -- server-side bullet/projectile fire
   ===========================================================================
   Called from ClientThink_real when BUTTON_ATTACK is pressed.
   Traces a ray from the player's eye using weapon spread, applies damage
   to the first entity hit.

   Reference: GAME_MP_.c FireWeapon / Bullet_Fire_Extended
   =========================================================================== */

#define BULLET_RANGE   8192.0f

/*
 * CalcMuzzlePointCod1 -- compute the muzzle origin for a player's shot.
 * Eye = origin + viewheight, plus a small forward offset so the trace
 * doesn't start inside the player's own bounding box.
 */
static void CalcMuzzlePointCod1( gentity_t *ent, vec3_t forward, vec3_t muzzle )
{
    VectorCopy( ent->client->ps.origin, muzzle );
    muzzle[2] += ent->client->ps.viewheight;
    /* nudge forward to clear the player bbox */
    VectorMA( muzzle, 14.0f, forward, muzzle );
    /* snap to integer coords to match Q3/CoD trace semantics */
    SnapVector( muzzle );
}

/*
 * G_FireBullet -- trace a single hitscan bullet with spread.
 *
 * Reference: GAME_MP_.c Bullet_Fire_Extended
 *   spread = tan(spreadDeg * PI/180) * BULLET_RANGE
 *   end = muzzle + forward * RANGE + right * rnd * spread + up * rnd * spread
 */
static void G_FireBullet( gentity_t *attacker, vec3_t muzzle,
                           vec3_t forward, vec3_t right, vec3_t up,
                           float spreadDeg, int damage, int mod )
{
    vec3_t    end;
    trace_t   tr;
    gentity_t *traceEnt;
    float     spreadRad, rndR, rndU;
    int       hitLoc;

    if ( spreadDeg < 0.0f ) spreadDeg = 0.0f;
    spreadRad = (float)tan( (double)spreadDeg * M_PI / 180.0 ) * BULLET_RANGE;

    /* CoD1 gunrandom: uniform polar distribution (ref: lines 21277-21296)
     * angle = rand(0..360), magnitude = rand(0..1)
     * x = magnitude * cos(angle), y = magnitude * sin(angle) */
    {
        float angle = (float)( rand() & 0xffff ) / 65536.0f * 360.0f;
        float mag   = (float)( rand() & 0xffff ) / 65536.0f;
        float rad   = (float)( angle * M_PI / 180.0 );
        rndR = mag * (float)cos( rad ) * spreadRad;
        rndU = mag * (float)sin( rad ) * spreadRad;
    }

    VectorMA( muzzle, BULLET_RANGE, forward, end );
    VectorMA( end, rndR, right, end );
    VectorMA( end, rndU, up, end );

    trap_Trace( &tr, muzzle, NULL, NULL, end,
                attacker->s.number, MASK_SHOT );

    if ( tr.fraction >= 1.0f ) return;

    traceEnt = &g_entities[ tr.entityNum ];
    if ( !traceEnt->takedamage ) return;

    /* Hit location damage multiplier (CoD1 body-region system) */
    hitLoc = G_CalcHitLocFromPoint( tr.endpos, traceEnt );
    if ( hitLoc >= 0 && hitLoc < HITLOC_NUM ) {
        damage = (int)( (float)damage * g_fHitLocDamageMult[hitLoc] );
        /* Head shot: override MOD for kill feed */
        if ( hitLoc == HITLOC_HEAD || hitLoc == HITLOC_HELMET )
            mod = MOD_HEAD_SHOT;
    }
    if ( damage < 1 ) damage = 1;

    G_Damage( traceEnt, attacker, attacker, forward, tr.endpos,
              damage, DAMAGE_NO_HITLOC, mod );
}

void G_FireWeapon( gentity_t *attacker )
{
    gclient_t   *client;
    int          slot;
    const char  *weapName;
    weaponDef_t  wd;
    vec3_t       forward, right, up;
    vec3_t       muzzle;
    int          damage;
    float        spread;
    int          bulletMod;

    if ( !attacker || !attacker->client ) return;
    client = attacker->client;

    /* Must have a current weapon */
    slot = client->currentWeaponSlot;
    if ( slot < 0 || slot >= COD1_WEAPON_SLOT_NUM ) return;
    if ( !client->weaponSlots[slot].name[0] ) return;

    /* No ammo: click */
    if ( client->weaponSlots[slot].clipAmmo <= 0 ) return;

    weapName = client->weaponSlots[slot].name;

    /* Use cached weapon def if available, otherwise parse and cache */
    if ( Q_stricmp( client->cachedWeaponName, weapName ) == 0 ) {
        Com_Memcpy( &wd, &client->cachedWeaponDef, sizeof(wd) );
    } else {
        if ( !BG_ParseWeaponDef( weapName, &wd ) ) return;
        Q_strncpyz( client->cachedWeaponName, weapName,
                    sizeof(client->cachedWeaponName) );
        Com_Memcpy( &client->cachedWeaponDef, &wd, sizeof(wd) );
    }

    /* Semi-auto weapons (rifle, pistol): require fresh button press each shot.
     * Full-auto (smg, mg): can hold trigger.
     * Reference: GAME_MP_.c PM_Weapon checks weaponClass for fire mode. */
    if ( !Q_stricmp( wd.weaponClass, "rifle" ) ||
         !Q_stricmp( wd.weaponClass, "pistol" ) ||
         !Q_stricmp( wd.weaponClass, "sniper" ) ) {
        /* Semi-auto: require button press edge (latched) */
        if ( !( client->latched_buttons & BUTTON_ATTACK ) )
            return;
        client->latched_buttons &= ~BUTTON_ATTACK;
    }

    /* Rate limit */
    if ( level.time < client->nextFireTime ) return;
    client->nextFireTime = level.time + (int)( wd.fireTime * 1000.0f );
    if ( client->nextFireTime <= level.time )
        client->nextFireTime = level.time + 100; /* safety minimum */

    /* Rechamber: bolt-action rifles have extra delay between shots */
    if ( wd.rechamberTime > 0.001f ) {
        int rechamberMs = (int)( wd.rechamberTime * 1000.0f );
        if ( rechamberMs > client->nextFireTime - level.time )
            client->nextFireTime = level.time + rechamberMs;
    }

    /* Consume ammo server-side and sync to client.
     * Throttle sync: only send on empty, low ammo, or every 5th round
     * to avoid flooding the reliable command buffer during sustained fire. */
    client->weaponSlots[slot].clipAmmo--;
    {
        int clip = client->weaponSlots[slot].clipAmmo;
        if ( clip == 0 || clip <= 5 || ( clip % 5 ) == 0 ) {
            trap_SendServerCommand( attacker->s.number,
                va( "wa %d %d", clip, client->weaponSlots[slot].reserveAmmo ) );
        }
    }

    damage = wd.damage > 0 ? wd.damage : 50;

    /* Determine MOD based on weapon class */
    if ( !Q_stricmp( wd.weaponClass, "pistol" ) )
        bulletMod = MOD_PISTOL_BULLET;
    else
        bulletMod = MOD_RIFLE_BULLET;

    /* Compute aim vectors */
    AngleVectors( client->ps.viewangles, forward, right, up );
    CalcMuzzlePointCod1( attacker, forward, muzzle );

    /* Determine spread — ADS or hip fire, stance-dependent.
     * Reference: GAME_MP_.c FireWeapon uses BG_GetMinSpreadForWeapon
     * with pm->ps->aimSpreadScale to interpolate between min and max.
     *
     * Dynamic spread: interpolate between min and max based on player
     * movement speed (CoD1 PM_AdjustAimSpreadScale). Moving = max spread,
     * still = min spread. */
    {
        float minSpread, maxSpread, speedFrac;
        vec3_t hVel;

        if ( client->buttons & ( BUTTON_ADS | BUTTON_WALKING ) ) {
            spread = wd.adsSpread;
        } else {
            /* Base min spread per stance */
            if ( client->ps.pm_flags & PMF_PRONE ) {
                minSpread = wd.hipSpreadProneMin;
            } else if ( client->ps.pm_flags & PMF_DUCKED ) {
                minSpread = wd.hipSpreadDuckedMin;
            } else {
                minSpread = wd.hipSpreadStandMin;
            }
            if ( minSpread <= 0.0f ) minSpread = wd.hipSpreadStandMin;

            maxSpread = wd.hipSpreadMax;
            if ( maxSpread <= minSpread ) maxSpread = minSpread + 2.0f;

            /* Movement speed factor: 0.0 = still, 1.0 = full run */
            VectorCopy( client->ps.velocity, hVel );
            hVel[2] = 0;
            speedFrac = VectorLength( hVel ) / 190.0f; /* ~190 ups = full run */
            if ( speedFrac > 1.0f ) speedFrac = 1.0f;

            spread = minSpread + ( maxSpread - minSpread ) * speedFrac;
        }
        if ( spread < 0.0f ) spread = 0.0f;
    }

    /* Fire based on weapon type (CoD1: bullet / grenade / rocket) */
    if ( !Q_stricmp( wd.weaponType, "grenade" ) ) {
        /* Grenade: launch a bouncing projectile with timed fuse.
         * Reference: GAME_MP_.c fire_grenade + Weapon_RocketLauncher_Fire */
        gentity_t *grenade;
        vec3_t    vel;

        /* Grenade velocity: forward * speed + upward + player velocity.
         * Reference: GAME_MP_.c line 55231-55243
         * speed = weaponInfo[788], upvel = weaponInfo[792]
         * Then player velocity dotted with direction is added. */
        VectorScale( forward, 800.0f, vel );
        vel[2] += 200.0f; /* upward arc component */

        /* Add player momentum to throw (ref: lines 55238-55243) */
        {
            vec3_t normDir;
            float  dot;
            VectorCopy( vel, normDir );
            VectorNormalize( normDir );
            dot = DotProduct( client->ps.velocity, normDir );
            if ( dot > 0 ) {
                VectorMA( vel, dot, normDir, vel );
            }
        }

        grenade = G_Spawn();
        grenade->classname     = "grenade_projectile";
        grenade->nextthink     = level.time + 2500;  /* 2.5 second fuse (ref: line 38923) */
        grenade->think         = G_ExplodeMissile;    /* explode on timer */
        grenade->s.eType       = ET_MISSILE;
        grenade->s.eFlags      = EF_BOUNCE_HALF;      /* bounce off surfaces */
        grenade->r.svFlags     = SVF_USE_CURRENT_ORIGIN;
        grenade->s.weapon      = 0;
        grenade->r.ownerNum    = attacker->s.number;
        grenade->parent        = attacker;
        grenade->damage        = damage;
        grenade->splashDamage  = damage;
        grenade->splashRadius  = 256;
        grenade->methodOfDeath = MOD_GRENADE;
        grenade->splashMethodOfDeath = MOD_GRENADE_SPLASH;
        grenade->clipmask      = MASK_SHOT;
        grenade->s.pos.trType  = TR_GRAVITY;
        grenade->s.pos.trTime  = level.time;
        VectorCopy( muzzle, grenade->s.pos.trBase );
        VectorCopy( vel, grenade->s.pos.trDelta );
        SnapVector( grenade->s.pos.trDelta );

        VectorCopy( muzzle, grenade->r.currentOrigin );
        trap_LinkEntity( grenade );

    } else if ( !Q_stricmp( wd.weaponType, "projectile" ) ||
                !Q_stricmp( wd.weaponType, "rocket" ) ) {
        /* Rocket / projectile: straight-line missile, explodes on impact.
         * Reference: GAME_MP_.c Weapon_RocketLauncher_Fire */
        gentity_t *rocket;

        rocket = G_Spawn();
        rocket->classname     = "rocket_projectile";
        rocket->nextthink     = level.time + 30000; /* 30s max flight (ref: line 38986) */
        rocket->think         = G_ExplodeMissile;
        rocket->s.eType       = ET_MISSILE;
        rocket->r.svFlags     = SVF_USE_CURRENT_ORIGIN;
        rocket->s.weapon      = 0;
        rocket->r.ownerNum    = attacker->s.number;
        rocket->parent        = attacker;
        rocket->damage        = damage;
        rocket->splashDamage  = damage;
        rocket->splashRadius  = 320;
        rocket->methodOfDeath = MOD_PROJECTILE;
        rocket->splashMethodOfDeath = MOD_PROJECTILE_SPLASH;
        rocket->clipmask      = MASK_SHOT;
        rocket->s.pos.trType  = TR_LINEAR;
        rocket->s.pos.trTime  = level.time;
        VectorCopy( muzzle, rocket->s.pos.trBase );
        /* Rocket speed from weapon def (ref: line 39006, offset 788).
         * Default 2000 ups if weapon def doesn't specify. */
        {
            float rocketSpeed = 2000.0f; /* default */
            /* TODO: read from weapon def when field is added */
            VectorScale( forward, rocketSpeed, rocket->s.pos.trDelta );
        }
        SnapVector( rocket->s.pos.trDelta );

        VectorCopy( muzzle, rocket->r.currentOrigin );
        trap_LinkEntity( rocket );

    } else {
        /* Bullet (default): hitscan with hit location */
        G_FireBullet( attacker, muzzle, forward, right, up, spread, damage, bulletMod );
    }
}

#endif /* STANDALONE */
