/*
 * See Licensing and Copyright notice in naev.h
 */

/**
 * @file ai.c
 *
 * @brief Controls the Pilot AI.
 *
 * AI Overview
 *
 * Concept: Goal (Task) Based AI with additional Optimization
 *
 *  AI uses the goal (task) based AI approach with tasks scripted in lua,
 * additionally there is a task that is hardcoded and obligatory in any AI
 * script, the 'control' task, whose sole purpose is to assign tasks if there
 * is no current tasks and optimizes or changes tasks if there are.
 *
 *  For example: Pilot A is attacking Pilot B.  Say that Pilot C then comes in
 * the same system and is of the same faction as Pilot B, and therefore attacks
 * Pilot A.  Pilot A would keep on fighting Pilot B until the control task
 * kicks in.  Then he/she could run if it deems that Pilot C and Pilot B
 * together are too strong for him/her, or attack Pilot C because it's an
 * easier target to finish off then Pilot B.  Therefore there are endless
 * possibilities and it's up to the AI coder to set up.
 *
 *
 * Specification
 *
 *   -  AI will follow basic tasks defined from Lua AI script.  
 *     - if Task is NULL, AI will run "control" task
 *     - Task is continued every frame
 *     -  "control" task is a special task that MUST exist in any given  Pilot AI
 *        (missiles and such will use "seek")
 *     - "control" task is not permanent, but transitory
 *     - "control" task sets another task
 *   - "control" task is also run at a set rate (depending on Lua global "control_rate")
 *     to choose optimal behaviour (task)
 *
 * 
 * Memory
 *
 *  The AI currently has per-pilot memory which is accessible as "mem".  This
 * memory is actually stored in the table pilotmem[cur_pilot->id].  This allows
 * the pilot to keep some memory always accesible between runs without having
 * to rely on the storage space a task has.
 *
 * @note Nothing in this file can be considered reentrant.  Plan accordingly.
 *
 * @todo Clean up most of the code, it was written as one of the first
 *         subsystems and is pretty lacking in quite a few aspects. Notably
 *         removing the entire lightuserdata thing and actually go with full
 *         userdata.
 */


#include "ai.h"
#include "ai_extra.h"

#include "naev.h"

#include <stdlib.h>
#include <stdio.h> /* malloc realloc */
#include <string.h> /* strncpy strlen strncat strcmp strdup */
#include <math.h>

/* yay more lua */
#include "lauxlib.h"
#include "lualib.h"

#include "log.h"
#include "pilot.h"
#include "player.h"
#include "physics.h"
#include "ndata.h"
#include "rng.h"
#include "space.h"
#include "faction.h"
#include "escort.h"
#include "nlua.h"
#include "nluadef.h"
#include "nlua_space.h"
#include "nlua_vec2.h"
#include "nlua_rnd.h"
#include "nlua_pilot.h"
#include "nlua_faction.h"
#include "board.h"

/* this is a push test to make sure I have GIT working correctly */

/**
 * @def lua_regnumber(l,s,n)
 *
 * @brief Registers a number constant n to name s (syntax like lua_regfunc).
 */
#define lua_regnumber(l,s,n)  \
(lua_pushnumber(l,n), lua_setglobal(l,s))


/*
 * ai flags
 *
 * They can be used for stuff like movement or for pieces of code which might
 *  run AI stuff when the AI module is not reentrant.
 */
#define ai_setFlag(f)   (pilot_flags |= f ) /**< Sets pilot flag f */
#define ai_isFlag(f)    (pilot_flags & f ) /**< Checks pilot flag f */
/* flags */
#define AI_PRIMARY      (1<<0)   /**< Firing primary weapon */
#define AI_SECONDARY    (1<<1)   /**< Firing secondary weapon */
#define AI_DISTRESS     (1<<2)   /**< Sent distress signal. */


/*
 * file info
 */
#define AI_PREFIX       "ai/" /**< AI file prefix. */
#define AI_SUFFIX       ".lua" /**< AI file suffix. */
#define AI_INCLUDE      "include/" /**< Where to search for includes. */


/*
 * all the AI profiles
 */
static AI_Profile* profiles = NULL; /**< Array of AI_Profiles loaded. */
static int nprofiles = 0; /**< Number of AI_Profiles loaded. */
static lua_State *equip_L = NULL; /**< Equipment state. */


/*
 * extern pilot hacks
 */
extern Pilot** pilot_stack;
extern int pilot_nstack;


/*
 * prototypes
 */
/* Internal C routines */
static void ai_run( lua_State *L, const char *funcname );
static int ai_loadProfile( const char* filename );
static void ai_setMemory (void);
static void ai_create( Pilot* pilot, char *param );
static int ai_loadEquip (void);


/*
 * AI routines for Lua
 */
/* tasks */
static int aiL_pushtask( lua_State *L ); /* pushtask( string, number/pointer, number ) */
static int aiL_poptask( lua_State *L ); /* poptask() */
static int aiL_taskname( lua_State *L ); /* number taskname() */

/* consult values */
static int aiL_getplayer( lua_State *L ); /* number getPlayer() */
static int aiL_gettarget( lua_State *L ); /* pointer gettarget() */
static int aiL_getrndpilot( lua_State *L ); /* number getrndpilot() */
static int aiL_armour( lua_State *L ); /* armour() */
static int aiL_shield( lua_State *L ); /* shield() */
static int aiL_parmour( lua_State *L ); /* parmour() */
static int aiL_pshield( lua_State *L ); /* pshield() */
static int aiL_getdistance( lua_State *L ); /* number getdist(Vector2d) */
static int aiL_getpos( lua_State *L ); /* getpos(number) */
static int aiL_minbrakedist( lua_State *L ); /* number minbrakedist( [number] ) */
static int aiL_cargofree( lua_State *L ); /* number cargofree() */
static int aiL_shipclass( lua_State *L ); /* string shipclass( [number] ) */
static int aiL_shipmass( lua_State *L ); /* string shipmass( [number] ) */
static int aiL_isbribed( lua_State *L ); /* bool isbribed( number ) */
static int aiL_getstanding( lua_State *L ); /* number getstanding( number ) */

/* boolean expressions */
static int aiL_exists( lua_State *L ); /* boolean exists() */
static int aiL_ismaxvel( lua_State *L ); /* boolean ismaxvel() */
static int aiL_isstopped( lua_State *L ); /* boolean isstopped() */
static int aiL_isenemy( lua_State *L ); /* boolean isenemy( number ) */
static int aiL_isally( lua_State *L ); /* boolean isally( number ) */
static int aiL_incombat( lua_State *L ); /* boolean incombat( [number] ) */
static int aiL_isdisabled( lua_State *L ); /* boolean isdisabled( number ) */
static int aiL_haslockon( lua_State *L ); /* boolean haslockon() */

/* movement */
static int aiL_accel( lua_State *L ); /* accel(number); number <= 1. */
static int aiL_turn( lua_State *L ); /* turn(number); abs(number) <= 1. */
static int aiL_face( lua_State *L ); /* face(number/pointer) */
static int aiL_aim( lua_State *L ); /* aim(number) */
static int aiL_brake( lua_State *L ); /* brake() */
static int aiL_getnearestplanet( lua_State *L ); /* Vec2 getnearestplanet() */
static int aiL_getrndplanet( lua_State *L ); /* Vec2 getrndplanet() */
static int aiL_getlandplanet( lua_State *L ); /* Vec2 getlandplanet() */
static int aiL_hyperspace( lua_State *L ); /* [number] hyperspace() */
static int aiL_stop( lua_State *L ); /* stop() */
static int aiL_relvel( lua_State *L ); /* relvel( number ) */

/* escorts */
static int aiL_e_attack( lua_State *L ); /* bool e_attack() */
static int aiL_e_hold( lua_State *L ); /* bool e_hold() */
static int aiL_e_clear( lua_State *L ); /* bool e_clear() */
static int aiL_e_return( lua_State *L ); /* bool e_return() */
static int aiL_dock( lua_State *L ); /* dock( number ) */

/* combat */
static int aiL_combat( lua_State *L ); /* combat( number ) */
static int aiL_settarget( lua_State *L ); /* settarget( number ) */
static int aiL_secondary( lua_State *L ); /* string secondary() */
static int aiL_hasturrets( lua_State *L ); /* bool hasturrets() */
static int aiL_shoot( lua_State *L ); /* shoot( number ); number = 1,2,3 */
static int aiL_getenemy( lua_State *L ); /* number getenemy() */
static int aiL_hostile( lua_State *L ); /* hostile( number ) */
static int aiL_getweaprange( lua_State *L ); /* number getweaprange() */
static int aiL_canboard( lua_State *L ); /* boolean canboard( number ) */

/* timers */
static int aiL_settimer( lua_State *L ); /* settimer( number, number ) */
static int aiL_timeup( lua_State *L ); /* boolean timeup( number ) */

/* messages */
static int aiL_comm( lua_State *L ); /* say( number, string ) */
static int aiL_broadcast( lua_State *L ); /* broadcast( string ) */
static int aiL_distress( lua_State *L ); /* distress( string [, bool] ) */

/* loot */
static int aiL_credits( lua_State *L ); /* credits( number ) */
static int aiL_cargo( lua_State *L ); /* cargo( name, quantity ) */
static int aiL_shipprice( lua_State *L ); /* shipprice() */

/* misc */
static int aiL_board( lua_State *L ); /* boolean board() */
static int aiL_refuel( lua_State *L ); /* boolean, boolean refuel() */
static int aiL_donerefuel( lua_State *L ); /* boolean donerefuel() */


static const luaL_reg aiL_methods[] = {
   /* tasks */
   { "pushtask", aiL_pushtask },
   { "poptask", aiL_poptask },
   { "taskname", aiL_taskname },
   /* is */
   { "exists", aiL_exists },
   { "ismaxvel", aiL_ismaxvel },
   { "isstopped", aiL_isstopped },
   { "isenemy", aiL_isenemy },
   { "isally", aiL_isally },
   { "incombat", aiL_incombat },
   { "isdisabled", aiL_isdisabled },
   { "haslockon", aiL_haslockon },
   /* get */
   { "getPlayer", aiL_getplayer },
   { "target", aiL_gettarget },
   { "rndpilot", aiL_getrndpilot },
   { "armour", aiL_armour },
   { "shield", aiL_shield },
   { "parmour", aiL_parmour },
   { "pshield", aiL_pshield },
   { "dist", aiL_getdistance },
   { "pos", aiL_getpos },
   { "minbrakedist", aiL_minbrakedist },
   { "cargofree", aiL_cargofree },
   { "shipclass", aiL_shipclass },
   { "shipmass", aiL_shipmass },
   { "isbribed", aiL_isbribed },
   { "getstanding", aiL_getstanding },
   /* movement */
   { "nearestplanet", aiL_getnearestplanet },
   { "rndplanet", aiL_getrndplanet },
   { "landplanet", aiL_getlandplanet },
   { "accel", aiL_accel },
   { "turn", aiL_turn },
   { "face", aiL_face },
   { "brake", aiL_brake },
   { "stop", aiL_stop },
   { "hyperspace", aiL_hyperspace },
   { "relvel", aiL_relvel },
   /* escorts */
   { "e_attack", aiL_e_attack },
   { "e_hold", aiL_e_hold },
   { "e_clear", aiL_e_clear },
   { "e_return", aiL_e_return },
   { "dock", aiL_dock },
   /* combat */
   { "aim", aiL_aim },
   { "combat", aiL_combat },
   { "settarget", aiL_settarget },
   { "secondary", aiL_secondary },
   { "hasturrets", aiL_hasturrets },
   { "shoot", aiL_shoot },
   { "getenemy", aiL_getenemy },
   { "hostile", aiL_hostile },
   { "getweaprange", aiL_getweaprange },
   { "canboard", aiL_canboard },
   /* timers */
   { "settimer", aiL_settimer },
   { "timeup", aiL_timeup },
   /* messages */
   { "comm", aiL_comm },
   { "broadcast", aiL_broadcast },
   { "distress", aiL_distress },
   /* loot */
   { "setcredits", aiL_credits },
   { "setcargo", aiL_cargo },
   { "shipprice", aiL_shipprice },
   /* misc */
   { "board", aiL_board },
   { "refuel", aiL_refuel },
   { "donerefuel", aiL_donerefuel },
   {0,0} /* end */
}; /**< Lua AI Function table. */



/*
 * current pilot "thinking" and assorted variables
 */
static Pilot *cur_pilot = NULL; /**< Current pilot.  All functions use this. */
static double pilot_acc = 0.; /**< Current pilot's acceleration. */
static double pilot_turn = 0.; /**< Current pilot's turning. */
static int pilot_flags = 0; /**< Handle stuff like weapon firing. */
static int pilot_firemode = 0; /**< Method pilot is using to shoot. */
static char aiL_distressmsg[PATH_MAX]; /**< Buffer to store distress message. */

/*
 * ai status, used so that create functions can't be used elsewhere
 */
#define AI_STATUS_NORMAL      1 /**< Normal ai function behaviour. */
#define AI_STATUS_CREATE      2 /**< AI is running create function. */
static int aiL_status = AI_STATUS_NORMAL; /**< Current AI run status. */


/**
 * @brief Sets the cur_pilot's ai.
 */
static void ai_setMemory (void)
{
   lua_State *L;
   L = cur_pilot->ai->L;

   /* */
   lua_getglobal(L, "pilotmem");
   /* pilotmem */
   lua_pushnumber(L, cur_pilot->id);
   /* pilotmem, id */
   lua_gettable(L, -2);
   /* pilotmem, table */
   lua_setglobal(L, "mem");
   /* pilotmem */
   lua_pop(L,1);
   /* */
}


/**
 * @brief Sets the pilot for furthur AI calls.
 *
 *    @param p Pilot to set.
 */
void ai_setPilot( Pilot *p )
{
   cur_pilot = p;
   ai_setMemory();
}


/**
 * @brief Attempts to run a function.
 *
 *    @param[in] L Lua state to run function on.
 *    @param[in] funcname Function to run.
 */
static void ai_run( lua_State *L, const char *funcname )
{
   lua_getglobal(L, funcname);

#ifdef DEBUGGING
   if (lua_isnil(L, -1)) {
      WARN("Pilot '%s' ai -> '%s': attempting to run non-existant function",
            cur_pilot->name, funcname );
      lua_pop(L,1);
      return;
   }
#endif /* DEBUGGING */

   if (lua_pcall(L, 0, 0, 0)) { /* error has occured */
      WARN("Pilot '%s' ai -> '%s': %s", cur_pilot->name, funcname, lua_tostring(L,-1));
      lua_pop(L,1);
   }
}


/**
 * @brief Initializes the pilot in the ai.
 *
 * Mainly used to create the pilot's memory table.
 *
 *    @param p Pilot to initialize in AI.
 *    @param ai AI to initialize pilot.
 */
int ai_pinit( Pilot *p, const char *ai )
{
   int i, n;
   AI_Profile *prof;
   lua_State *L;
   char buf[PATH_MAX], param[PATH_MAX];

   /* Split parameter from ai itself. */
   n = 0;
   for (i=0; ai[i] != '\0'; i++) {
      /* Overflow protection. */
      if (i > PATH_MAX)
         break;

      /* Check to see if we find the splitter. */
      if (ai[i] == '*') {
         buf[i] = '\0';
         n = i+1;
         continue;
      }

      if (n==0)
         buf[i] = ai[i];
      else
         param[i-n] = ai[i];
   }
   if (n!=0) param[i-n] = '\0'; /* Terminate string if needed. */
   else buf[i] = '\0';

   /* Set up the profile. */
   prof = ai_getProfile(buf);
   if (prof == NULL)
      WARN("AI Profile '%s' not found.", buf);
   p->ai = prof;
   L = p->ai->L;

   /* Set fuel.  Hack until we do it through AI itself. */
   p->fuel  = (RNG_2SIGMA()/4. + 0.5) * (p->fuel_max - HYPERSPACE_FUEL);
   p->fuel += HYPERSPACE_FUEL;

   /* Adds a new pilot memory in the memory table. */
   lua_getglobal(L, "pilotmem"); /* pm */
   lua_newtable(L);              /* pm, nt */
   lua_pushnumber(L, p->id);     /* pm, nt, n */
   lua_pushvalue(L,-2);          /* pm, nt, n, nt */
   lua_settable(L,-4);           /* pm, nt */

   /* Copy defaults over. */
   lua_pushstring(L, "default"); /* pm, nt, s */
   lua_gettable(L, -3);          /* pm, nt, dt */
   lua_pushnil(L);               /* pm, nt, dt, nil */
   while (lua_next(L,-2) != 0) { /* pm, nt, dt, k, v */
      lua_pushvalue(L,-2);       /* pm, nt, dt, k, v, k */
      lua_pushvalue(L,-2);       /* pm, nt, dt, k, v, k, v */
      lua_remove(L, -3);         /* pm, nt, dt, k, k, v */
      lua_settable(L,-5);        /* pm, nt, dt, k */
   }                             /* pm, nt, dt */
   lua_pop(L,3);                 /* */

   /* Create the pilot. */
   ai_create( p, (n!=0) ? param : NULL );
   pilot_setFlag(p, PILOT_CREATED_AI);

   return 0;
}


/**
 * @brief Clears the pilot's tasks.
 *
 *    @param p Pilot to clear tasks of.
 */
void ai_cleartasks( Pilot* p )
{
   /* Clean up tasks. */
   if (p->task)
      ai_freetask( p->task );
   p->task = NULL;
}


/**
 * @brief Destroys the ai part of the pilot
 *
 *    @param[in] p Pilot to destroy it's AI part.
 */
void ai_destroy( Pilot* p )
{
   lua_State *L;
   L = p->ai->L;

   /* Get rid of pilot's memory. */
   lua_getglobal(L, "pilotmem");
   lua_pushnumber(L, p->id);
   lua_pushnil(L);
   lua_settable(L,-3);
   lua_pop(L,1);

   /* Clear the tasks. */
   ai_cleartasks( p );
}


/**
 * @brief Initializes the AI stuff which is basically Lua.
 *
 *    @return 0 on no errors.
 */
int ai_load (void)
{
   char** files;
   uint32_t nfiles, i;
   char path[PATH_MAX];
   int flen, suflen;

   /* get the file list */
   files = ndata_list( AI_PREFIX, &nfiles );

   /* load the profiles */
   suflen = strlen(AI_SUFFIX);
   for (i=0; i<nfiles; i++) {
      flen = strlen(files[i]);
      if ((flen > suflen) &&
            strncmp(&files[i][flen-suflen], AI_SUFFIX, suflen)==0) {

         snprintf( path, PATH_MAX, AI_PREFIX"%s", files[i] );
         if (ai_loadProfile(path)) /* Load the profile */
            WARN("Error loading AI profile '%s'", path);
      }

      /* Clean up. */
      free(files[i]);
   }

   DEBUG("Loaded %d AI Profile%c", nprofiles, (nprofiles==1)?' ':'s');

   /* More clean up. */
   free(files);

   /* Load equipment thingy. */
   return ai_loadEquip();
}


/**
 * @brief Loads the equipment selector script.
 */
static int ai_loadEquip (void)
{
   char *buf;
   uint32_t bufsize;
   const char *filename = "ai/equip/equip.lua";
   lua_State *L;

   /* Make sure doesn't already exist. */
   if (equip_L != NULL)
      lua_close(equip_L);

   /* Create new state. */
   equip_L = nlua_newState();
   L = equip_L;

   /* Prepare state. */
   nlua_loadStandard(L,0);

   /* Load the file. */
   buf = ndata_read( filename, &bufsize );
   if (luaL_dobuffer(L, buf, bufsize, filename) != 0) {
      ERR("Error loading file: %s\n"
          "%s\n"
          "Most likely Lua file has improper syntax, please check",
            filename, lua_tostring(L,-1));
      return -1;
   }
   free(buf);

   return 0;
}


/**
 * @brief Initializes an AI_Profile and adds it to the stack.
 *
 *    @param[in] filename File to create the profile from.
 *    @return 0 on no error.
 */
static int ai_loadProfile( const char* filename )
{
   char* buf = NULL;
   uint32_t bufsize = 0;
   lua_State *L;

   profiles = realloc( profiles, sizeof(AI_Profile)*(++nprofiles) );

   profiles[nprofiles-1].name =
      malloc(sizeof(char)*(strlen(filename)-strlen(AI_PREFIX)-strlen(AI_SUFFIX))+1 );
   snprintf( profiles[nprofiles-1].name,
         strlen(filename)-strlen(AI_PREFIX)-strlen(AI_SUFFIX)+1,
         "%s", filename+strlen(AI_PREFIX) );

   profiles[nprofiles-1].L = nlua_newState();

   if (profiles[nprofiles-1].L == NULL) {
      ERR("Unable to create a new Lua state");
      return -1;
   }

   L = profiles[nprofiles-1].L;

   /* open basic lua stuff */
   nlua_loadBasic(L);

   /* constants */
   lua_regnumber(L, "player", PLAYER_ID); /* player ID */

   /* Register C functions in Lua */
   luaL_register(L, "ai", aiL_methods);
   nlua_loadRnd(L);

   /* Metatables to register. */
   nlua_loadVector(L);

   /* Add the player memory table. */
   lua_newtable(L);
   lua_setglobal(L, "pilotmem");

   /* Set "mem" to be default template. */
   lua_getglobal(L, "pilotmem"); /* pm */
   lua_newtable(L);              /* pm, nt */
   lua_pushstring(L, "default"); /* pm, nt, s */
   lua_pushvalue(L,-2);          /* pm, nt, s, nt */
   lua_settable(L,-4);           /* pm, nt */
   lua_setglobal(L, "mem");      /* pm */
   lua_pop(L,1);                 /* */

   /* now load the file since all the functions have been previously loaded */
   buf = ndata_read( filename, &bufsize );
   if (luaL_dobuffer(L, buf, bufsize, filename) != 0) {
      ERR("Error loading AI file: %s\n"
          "%s\n"
          "Most likely Lua file has improper syntax, please check",
            filename, lua_tostring(L,-1));
      return -1;
   }
   free(buf);

   return 0;
}


/**
 * @brief Gets the AI_Profile by name.
 *
 *    @param[in] name Name of the profile to get.
 *    @return The profile or NULL on error.
 */
AI_Profile* ai_getProfile( char* name )
{
   if (profiles == NULL) return NULL;

   int i;

   for (i=0; i<nprofiles; i++)
      if (strcmp(name,profiles[i].name)==0)
         return &profiles[i];

   WARN("AI Profile '%s' not found in AI stack", name);
   return NULL;
}


/**
 * @brief Cleans up global AI.
 */
void ai_exit (void)
{
   int i;

   /* Free AI profiles. */
   for (i=0; i<nprofiles; i++) {
      free(profiles[i].name);
      lua_close(profiles[i].L);
   }
   free(profiles);

   /* Free equipment Lua. */
   if (equip_L != NULL)
      lua_close(equip_L);
   equip_L = NULL;
}


/**
 * @brief Heart of the AI, brains of the pilot.
 *
 *    @param pilot Pilot that needs to think.
 */
void ai_think( Pilot* pilot, const double dt )
{
   (void) dt;

   lua_State *L;

   ai_setPilot(pilot);
   L = cur_pilot->ai->L; /* set the AI profile to the current pilot's */

   /* clean up some variables */
   pilot_acc         = 0;
   pilot_turn        = 0.;
   pilot_flags       = 0;
   pilot_firemode    = 0;
   cur_pilot->target = cur_pilot->id;

   /* control function if pilot is idle or tick is up */
   if (!pilot_isFlag(cur_pilot, PILOT_MANUAL_CONTROL) &&
         ((cur_pilot->tcontrol < 0.) || (cur_pilot->task == NULL))) {
      ai_run(L, "control"); /* run control */
      lua_getglobal(L,"control_rate");
      cur_pilot->tcontrol = lua_tonumber(L,-1);
      lua_pop(L,1);
   }

   /* pilot has a currently running task */
   if (cur_pilot->task) {
      ai_run(L, cur_pilot->task->name);
      if ((cur_pilot->task==NULL) && pilot_isFlag(cur_pilot, PILOT_MANUAL_CONTROL))
         pilot_runHook( cur_pilot, PILOT_HOOK_IDLE );
   }

   /* make sure pilot_acc and pilot_turn are legal */
   pilot_acc   = CLAMP( 0., 1., pilot_acc );
   pilot_turn  = CLAMP( -1., 1., pilot_turn );

   /* Set turn and thrust. */
   pilot_setTurn( cur_pilot, pilot_turn );
   pilot_setThrust( cur_pilot, pilot_acc );

   /* fire weapons if needed */
   if (ai_isFlag(AI_PRIMARY))
      pilot_shoot(cur_pilot, pilot_firemode); /* primary */
   if (ai_isFlag(AI_SECONDARY))
      pilot_shootSecondary(cur_pilot); /* secondary */

   /* other behaviours. */
   if (ai_isFlag(AI_DISTRESS))
      pilot_distress(cur_pilot, aiL_distressmsg, 0);
}


/**
 * @brief Triggers the attacked() function in the pilot's AI.
 *
 *    @param attacked Pilot that is attacked.
 *    @param[in] attacker ID of the attacker.
 */
void ai_attacked( Pilot* attacked, const unsigned int attacker )
{
   lua_State *L;

   /* Behaves differently if manually overriden. */
   if (pilot_isFlag( attacked, PILOT_MANUAL_CONTROL )) {
      pilot_runHook( attacked, PILOT_HOOK_ATTACKED );
      return;
   }

   /* Must have an AI profile. */
   if (attacked->ai == NULL)
      return;

   ai_setPilot(attacked);
   L = cur_pilot->ai->L;
   lua_getglobal(L, "attacked");
   lua_pushnumber(L, attacker);
   if (lua_pcall(L, 1, 0, 0)) {
      WARN("Pilot '%s' ai -> 'attacked': %s", cur_pilot->name, lua_tostring(L,-1));
      lua_pop(L,1);
   }
}


/**
 * @brief Has a pilot attempt to refuel the other.
 *
 *    @param refueler Pilot doing the refueling.
 *    @param target Pilot to refuel.
 */
void ai_refuel( Pilot* refueler, unsigned int target )
{
   Task *t;

   /* Create the task. */
   t = malloc(sizeof(Task));
   t->next     = NULL;
   t->name     = strdup("refuel");
   t->dtype    = TASKDATA_INT;
   t->dat.num  = target;

   /* Prepend the task. */
   t->next = refueler->task;
   refueler->task = t;

   return;
}


/**
 * @brief Sends a distress signal to a pilot.
 *
 *    @param p Pilot recieving the distress signal.
 *    @param distressed Pilot sending the distress signal.
 */
void ai_getDistress( Pilot* p, const Pilot* distressed )
{
   lua_State *L;

   /* Ignore distress signals when under manual control. */
   if (pilot_isFlag( p, PILOT_MANUAL_CONTROL ))
      return;

   /* Set up the environment. */
   ai_setPilot(p);
   L = cur_pilot->ai->L;

   /* See if function exists. */
   lua_getglobal(L, "distress");
   if (lua_isnil(L,-1)) {
      lua_pop(L,1);
      return;
   }

   /* Run the function. */
   lua_pushnumber(L, distressed->id);
   lua_pushnumber(L, distressed->target);
   if (lua_pcall(L, 2, 0, 0)) {
      WARN("Pilot '%s' ai -> 'distress': %s", cur_pilot->name, lua_tostring(L,-1));
      lua_pop(L,1);
   }
}


/**
 * @brief Runs the create() function in the pilot.
 *
 * Should create all the gear and sucth the pilot has.
 *
 *    @param pilot Pilot to "create".
 *    @param param Parameter to pass to "create" function.
 */
static void ai_create( Pilot* pilot, char *param )
{
   LuaPilot lp;
   LuaFaction lf;
   lua_State *L;

   /* Set creation mode. */
   if (!pilot_isFlag(pilot, PILOT_CREATED_AI))
      aiL_status = AI_STATUS_CREATE;

   /* Prepare AI. */
   ai_setPilot( pilot );

   /* Create equipment first - only if creating for the first time. */
   if ((aiL_status==AI_STATUS_CREATE) || !pilot_isFlag(pilot, PILOT_EMPTY)) {
      L = equip_L;
      lua_getglobal(L, "equip");
      lp.pilot = cur_pilot->id;
      lua_pushpilot(L,lp); 
      lf.f = cur_pilot->faction;
      lua_pushfaction(L,lf);
      if (lua_pcall(L, 2, 0, 0)) { /* Error has occurred. */
         WARN("Pilot '%s' equip -> '%s': %s", cur_pilot->name, "equip", lua_tostring(L,-1));
         lua_pop(L,1);
      }
   }

   /* Prepare stack. */
   L = cur_pilot->ai->L;
   lua_getglobal(L, "create");

   /* Parse parameter. */
   if (param != NULL) {
      /* Number */
      if (isdigit(param[0]))
         lua_pushnumber(L, atoi(param));
      /* Special case player. */
      else if (strcmp(param,"player")==0)
         lua_pushnumber(L, PLAYER_ID);
      /* Default. */
      else
         lua_pushstring(L, param);
   }

   /* Run function. */
   if (lua_pcall(L, (param!=NULL) ? 1 : 0, 0, 0)) { /* error has occured */
      WARN("Pilot '%s' ai -> '%s': %s", cur_pilot->name, "create", lua_tostring(L,-1));
      lua_pop(L,1);
   }

   /* Recover normal mode. */
   if (!pilot_isFlag(pilot, PILOT_CREATED_AI))
      aiL_status = AI_STATUS_NORMAL;
}


/**
 * @brief Creates a new AI task.
 */
Task *ai_newtask( Pilot *p, const char *func, int pos )
{
   Task *t, *pointer;
   
   /* Create the new task. */
   t        = malloc(sizeof(Task));
   t->next  = NULL;
   t->name  = strdup(func);
   t->dtype = TASKDATA_NULL;

   /* Attach the task. */
   if ((pos == 1) && (p->task != NULL)) { /* put at the end */
      for (pointer = p->task; pointer->next != NULL; pointer = pointer->next);
      pointer->next = t;
   }
   else { /* default put at the beginning */
      t->next = p->task;
      p->task = t;
   }

   return t;
}


/**
 * @brief Frees an AI task.
 *
 *    @param t Task to free.
 */
void ai_freetask( Task* t )
{
   if (t->next != NULL) {
      ai_freetask(t->next); /* yay recursive freeing */
      t->next = NULL;
   }

   if (t->name)
      free(t->name);
   free(t);
}


/**
 * @defgroup AI Lua AI Bindings
 *
 * @brief Handles how the AI interacts with the universe.
 *
 * Usage is:
 * @code
 * ai.function( params )
 * @endcode
 *
 * @{
 */
/**
 * @brief Pushes a task onto the pilot's task list.
 *    @luaparam pos Position to push into stack, 0 is front, 1 is back.
 *    @luaparam func Function to call for task.
 *    @luaparam data Data to pass to the function.  Only lightuserdata or number
 *           is currently supported.
 * @luafunc pushtask( pos, func, data )
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_pushtask( lua_State *L )
{
   NLUA_MIN_ARGS(2);
   int pos;
   const char *func;
   Task *t;
   LuaVector *lv;

   /* Parse basic parameters. */
   pos   = luaL_checkint(L,1);
   func  = luaL_checkstring(L,2);

   /* Creates a new AI task. */
   t = ai_newtask( cur_pilot, func, pos );

   /* Set the data. */
   if (lua_gettop(L) > 2) {
      if (lua_isnumber(L,3)) {
         t->dtype = TASKDATA_INT;
         t->dat.num = (unsigned int)lua_tonumber(L,3);
      }
      else if (lua_isvector(L,3)) {
         t->dtype = TASKDATA_VEC2;
         lv       = lua_tovector(L,3);
         vectcpy( &t->dat.vec, &lv->vec );
      }
      else NLUA_INVALID_PARAMETER();
   }

   return 0;
}
/**
 * @brief Pops the current running task.
 * @luafunc poptask()
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_poptask( lua_State *L )
{
   (void)L; /* hack to avoid -W -Wall warnings */
   Task* t = cur_pilot->task;

   /* Tasks must exist. */
   if (t == NULL) {
      NLUA_DEBUG("Trying to pop task when there are no tasks on the stack.");
      return 0;
   }

   cur_pilot->task = t->next;
   t->next = NULL;
   ai_freetask(t);
   return 0;
}

/**
 * @brief Gets the current task's name.
 *    @return The current task name or "none" if there are no tasks.
 * @luafunc taskname()
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_taskname( lua_State *L )
{
   if (cur_pilot->task) lua_pushstring(L, cur_pilot->task->name);
   else lua_pushstring(L, "none");
   return 1;
}

/**
 * @brief Gets the player.
 *    @return The player's ship identifier.
 * @luafunc getPlayer()
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_getplayer( lua_State *L )
{
   lua_pushnumber(L, PLAYER_ID);
   return 1;
}

/**
 * @brief Gets the pilot's target.
 *    @return The pilot's target ship identifier or nil if no target.
 * @luafunc target()
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_gettarget( lua_State *L )
{
   LuaVector lv;

   /* Must have a task. */
   if (cur_pilot->task == NULL)
      return 0;

   /* Pask task type. */
   switch (cur_pilot->task->dtype) {
      case TASKDATA_INT:
         lua_pushnumber(L, cur_pilot->task->dat.num);
         return 1;

      case TASKDATA_VEC2:
         lv.vec = cur_pilot->task->dat.vec;
         lua_pushvector(L, lv);
         return 1;

      default:
         return 0;
   }
}

/**
 * @brief Gets a random target's ID
 *    @return Gets a random pilot in the system.
 * @luafunc rndpilot()
 *    @param L Lua state.
 *    @return Number of Lua parameters.
 */
static int aiL_getrndpilot( lua_State *L )
{
   int p;
   p = RNG(0, pilot_nstack-1);
   /* Make sure it can't be the same pilot. */
   if (pilot_stack[p]->id == cur_pilot->id) {
      p++;
      if (p >= pilot_nstack)
         p = 0;
   }
   /* Last check. */
   if (pilot_stack[p]->id == cur_pilot->id)
      return 0;
   /* Actually found a pilot. */
   lua_pushnumber(L, pilot_stack[p]->id );
   return 1;
}

/*
 * gets the pilot's armour
 */
static int aiL_armour( lua_State *L )
{
   Pilot *p;
   double d;

   if (lua_isnumber(L,1)) {
      p = pilot_get((unsigned int)lua_tonumber(L,1));
      if (p==NULL) {
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
      d = p->armour;
   }
   else d = cur_pilot->armour;

   lua_pushnumber(L, d);
   return 1;
}

/*
 * gets the pilot's shield
 */
static int aiL_shield( lua_State *L )
{
   Pilot *p;
   double d;

   if (lua_isnumber(L,1)) {
      p = pilot_get((unsigned int)lua_tonumber(L,1));
      if (p==NULL) {
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
      d = p->shield;
   }
   else d = cur_pilot->shield;

   lua_pushnumber(L, d);
   return 1;
}

/*
 * gets the pilot's armour in percent
 */
static int aiL_parmour( lua_State *L )
{
   double d;
   Pilot* p;

   if (lua_isnumber(L,1)) {
      p = pilot_get((unsigned int)lua_tonumber(L,1));
      if (p==NULL) {
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
      d = p->armour / p->armour_max * 100.;
   }
   else d = cur_pilot->armour / cur_pilot->armour_max * 100.;

   lua_pushnumber(L, d);
   return 1;
}

/* 
 * gets the pilot's shield in percent
 */              
static int aiL_pshield( lua_State *L )
{
   double d;
   Pilot* p;

   if (lua_isnumber(L,1)) {
      p = pilot_get((unsigned int)lua_tonumber(L,1));
      if (p==NULL) {
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
      d = p->shield / p->shield_max * 100.;
   }
   else d = cur_pilot->shield / cur_pilot->shield_max * 100.;

   lua_pushnumber(L, d);
   return 1;
} 

/*
 * gets the distance from the pointer
 */
static int aiL_getdistance( lua_State *L )
{
   Vector2d *v;
   LuaVector *lv;
   Pilot *pilot;
   unsigned int n;

   NLUA_MIN_ARGS(1);

   /* vector as a parameter */
   if (lua_isvector(L,1)) {
      lv = lua_tovector(L,1);
      v = &lv->vec;
   }

   else if (lua_islightuserdata(L,1))
      v = lua_touserdata(L,1);
   
   /* pilot id as parameter */
   else if (lua_isnumber(L,1)) {
      n = (unsigned int) lua_tonumber(L,1);
      pilot = pilot_get( (unsigned int) lua_tonumber(L,1) );
      if (pilot==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
      v = &pilot->solid->pos;
   }
   
   /* wrong parameter */
   else
      NLUA_INVALID_PARAMETER();

   lua_pushnumber(L, vect_dist(v, &cur_pilot->solid->pos));
   return 1;
}

/*
 * gets the pilot's position
 */
static int aiL_getpos( lua_State *L )
{
   Pilot *p;

   if (lua_isnumber(L,1)) {
      p = pilot_get((unsigned int)lua_tonumber(L,1)); /* Pilot ID */
      if (p==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
   }
   else p = cur_pilot; /* default to self */

   lua_pushlightuserdata(L, &p->solid->pos );

   return 1;
}

/*
 * gets the minimum braking distance
 *
 * braking vel ==> 0 = v - a*dt
 * add turn around time (to inital vel) ==> 180.*360./cur_pilot->turn
 * add it to general euler equation  x = v * t + 0.5 * a * t^2
 * and voila!
 *
 * I hate this function and it'll probably need to get changed in the future
 */
static int aiL_minbrakedist( lua_State *L )
{
   double time, dist, vel;
   Vector2d vv;
   unsigned int id;
   Pilot *p;

   /* More complicated calculation based on relative velocity. */
   if (lua_gettop(L) > 0) {
      /* Get target. */
      id = luaL_checklong(L,1);
      p = pilot_get(id);
      if (p==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }

      /* Set up the vectors. */
      vect_cset( &vv, p->solid->vel.x - cur_pilot->solid->vel.x,
            p->solid->vel.y - cur_pilot->solid->vel.y );
   
      /* Run the same calculations. */
      time = VMOD(vv) /
            (cur_pilot->thrust / cur_pilot->solid->mass);

      /* Get relative velocity. */
      vel = MIN(cur_pilot->speed - VMOD(p->solid->vel), VMOD(vv));
      if (vel < 0.)
         vel = 0.;

      /* Get distance to brake. */
      dist = vel*(time+1.1*180./cur_pilot->turn) -
            0.5*(cur_pilot->thrust/cur_pilot->solid->mass)*time*time;
   }

   /* Simple calculation based on distance. */
   else {
      /* Get current time to reach target. */
      time = VMOD(cur_pilot->solid->vel) /
            (cur_pilot->thrust / cur_pilot->solid->mass);

      /* Get velocity. */
      vel = MIN(cur_pilot->speed,VMOD(cur_pilot->solid->vel));

      /* Get distance. */
      dist = vel*(time+1.1*180./cur_pilot->turn) -
            0.5*(cur_pilot->thrust/cur_pilot->solid->mass)*time*time;
   }

   lua_pushnumber(L, dist); /* return */
   return 1; /* returns one thing */
}

/*
 * gets the pilot's free cargo space
 */
static int aiL_cargofree( lua_State *L )
{
   lua_pushnumber(L, pilot_cargoFree(cur_pilot));
   return 1;
}


/*
 * gets the pilot's ship class.
 */
static int aiL_shipclass( lua_State *L )
{
   unsigned int l;
   Pilot *p;

   if (lua_gettop(L) > 0) {
      l = luaL_checklong(L,1);
      p = pilot_get(l);
      if (p==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
   }
   else
      p = cur_pilot;

   lua_pushstring(L, ship_class(p->ship));
   return 1;
}


/*
 * Gets the ship's mass.
 */
static int aiL_shipmass( lua_State *L )
{
   unsigned int l;
   Pilot *p;

   if (lua_gettop(L) > 0) {
      l = luaL_checklong(L,1);
      p = pilot_get(l);
      if (p==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
   }
   else
      p = cur_pilot;

   lua_pushnumber(L, p->solid->mass);
   return 1;
}


/*
 * Checks to see if target has bribed pilot.
 */
static int aiL_isbribed( lua_State *L )
{
   unsigned int target;
   target = luaL_checklong(L,1);
   lua_pushboolean(L, (target == PLAYER_ID) && pilot_isFlag(cur_pilot,PILOT_BRIBED));
   return 1;
}


/**
 * @brief Gets the standing of the target pilot with the current pilot.
 *
 *    @luaparam target Target pilot to get faction standing of.
 *    @luareturn The faction standing of the target [-100,100] or nil if invalid.
 * @luafunc getstanding( target )
 */
static int aiL_getstanding( lua_State *L )
{
   unsigned int id;
   Pilot *p;
   /* Get parameters. */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) { 
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Get faction standing. */
   if (p->faction == FACTION_PLAYER)
      lua_pushnumber(L, faction_getPlayer(cur_pilot->faction));
   else {
      if (areAllies( cur_pilot->faction, p->faction ))
         lua_pushnumber(L, 100);
      else if (areEnemies( cur_pilot->faction, p->faction ))
         lua_pushnumber(L,-100);
      else
         lua_pushnumber(L, 0);
   }

   return 1;
}


/*
 * pilot exists?
 */
static int aiL_exists( lua_State *L )
{
   Pilot *p;
   int i;

   if (lua_isnumber(L,1)) {
      i = 1;
      p = pilot_get((unsigned int)lua_tonumber(L,1));
      if (p==NULL) i = 0;
      else if (pilot_isFlag(p,PILOT_DEAD)) i = 0;
      lua_pushboolean(L, i );
      return 1;
   }

   /* Default to false for everything that isn't a pilot */
   lua_pushboolean(L, 0);
   return 0;
}


/*
 * is at maximum velocity?
 */
static int aiL_ismaxvel( lua_State *L )
{
   lua_pushboolean(L,(VMOD(cur_pilot->solid->vel) > cur_pilot->speed-MIN_VEL_ERR));
   return 1;
}


/*
 * is stopped?
 */
static int aiL_isstopped( lua_State *L )
{
   lua_pushboolean(L,(VMOD(cur_pilot->solid->vel) < MIN_VEL_ERR));
   return 1;
}


/*
 * checks if pilot is an enemy
 */
static int aiL_isenemy( lua_State *L )
{
   unsigned int id;
   Pilot *p;

   /* Get the pilot. */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) { 
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Player needs special handling in case of hostility. */
   if (p->faction == FACTION_PLAYER) {
      lua_pushboolean(L, pilot_isHostile(cur_pilot));
      lua_pushboolean(L,1);
      return 1;
   }

   /* Check if is ally. */
   lua_pushboolean(L,areEnemies(cur_pilot->faction, p->faction));
   return 1;
}

/*
 * checks if pillot is an ally
 */
static int aiL_isally( lua_State *L )
{
   unsigned int id;
   Pilot *p;

   /* Get the pilot. */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) { 
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Player needs special handling in case of friendliness. */
   if (p->faction == FACTION_PLAYER) {
      lua_pushboolean(L, pilot_isFriendly(cur_pilot));
      return 1;
   }

   /* Check if is ally. */
   lua_pushboolean(L,areAllies(cur_pilot->faction, p->faction));
   return 1;
}

/*
 * checks to see if the pilot is in combat, defaults to self
 */
static int aiL_incombat( lua_State *L )
{
   unsigned int id;
   Pilot* p;

   /* Get the pilot. */
   if (lua_gettop(L) > 0) {
      id = luaL_checklong(L,1);
      p = pilot_get(id);
      if (p==NULL) { 
         NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
         return 0;
      }
   }
   else
      p = cur_pilot;

   lua_pushboolean(L, pilot_isFlag(p, PILOT_COMBAT));
   return 1;
}


/**
 * @brief Checks to see if the target is disabled.
 *
 *    @luaparam p Pilot to see if is disabled.
 *    @luareturn true if pilot is disabled.
 * @luafunc isdisabled( p )
 */
static int aiL_isdisabled( lua_State *L )
{
   Pilot *p;
   unsigned int id;

   id = luaL_checklong(L, 1);
   p = pilot_get(id);
   if (p==NULL) { 
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   lua_pushboolean(L, pilot_isDisabled(p));
   return 1;
}


/*
 * pilot has is locked by some missile
 */
static int aiL_haslockon( lua_State *L )
{
   lua_pushboolean(L, cur_pilot->lockons > 0);
   return 1;
}


/*
 * starts accelerating the pilot based on a parameter
 */
static int aiL_accel( lua_State *L )
{
   double n;

   if (lua_gettop(L) > 1 && lua_isnumber(L,1)) {
      n = (double)lua_tonumber(L,1);

      if (n > 1.) n = 1.;
      else if (n < 0.) n = 0.;

      if (VMOD(cur_pilot->solid->vel) > (n * cur_pilot->speed))
         pilot_acc = 0.;
   }
   else
      pilot_acc = 1.;

   return 0;
}


/*
 * starts turning the pilot based on a parameter
 */
static int aiL_turn( lua_State *L )
{
   pilot_turn = luaL_checknumber(L,1);
   return 0;
}


/*
 * faces the target
 */
static int aiL_face( lua_State *L )
{
   NLUA_MIN_ARGS(1);
   LuaVector *lv;
   Vector2d sv, tv; /* get the position to face */
   Pilot* p;
   double d, mod, diff;
   unsigned int id;
   int n;

   /* Get first parameter, aka what to face. */
   n  = -2;
   lv = NULL;
   if (lua_isnumber(L,1)) {
      d = (double)lua_tonumber(L,1);
      if (d < 0.)
         n = -1;
      else {
         id = (unsigned int)d;
         p = pilot_get(id);
         if (p==NULL) { 
            NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
            return 0;
         }
         vect_cset( &tv, VX(p->solid->pos), VY(p->solid->pos) );
      }
   }
   else if (lua_isvector(L,1))
      lv = lua_tovector(L,1);
   else NLUA_INVALID_PARAMETER();

   mod = 10;

   /* Check if must invert. */
   if (lua_gettop(L) > 1) {
      if (lua_isboolean(L,2) && lua_toboolean(L,2))
         mod *= -1;
   }

   vect_cset( &sv, VX(cur_pilot->solid->pos), VY(cur_pilot->solid->pos) );

   if (lv==NULL) /* target is dynamic */
      diff = angle_diff(cur_pilot->solid->dir,
            (n==-1) ? VANGLE(sv) :
            vect_angle(&sv, &tv));
   else /* target is static */
      diff = angle_diff( cur_pilot->solid->dir,   
            (n==-1) ? VANGLE(cur_pilot->solid->pos) :
            vect_angle(&cur_pilot->solid->pos, &lv->vec));

   /* Make pilot turn. */
   pilot_turn = mod*diff;

   /* Return angle in degrees away from target. */
   lua_pushnumber(L, ABS(diff*180./M_PI));
   return 1;
}

/*
 * brakes the pilot
 */
static int aiL_brake( lua_State *L )
{
   (void)L; /* hack to avoid -W -Wall warnings */
   double diff, d;

   d = cur_pilot->solid->dir+M_PI;
   if (d >= 2*M_PI) d = fmod(d, 2*M_PI);
   
   diff = angle_diff(d,VANGLE(cur_pilot->solid->vel));
   pilot_turn = 10*diff;

   if (ABS(diff) < MAX_DIR_ERR && VMOD(cur_pilot->solid->vel) > MIN_VEL_ERR)
      pilot_acc = 1.;

   return 0;
}

/*
 * returns the nearest friendly planet's position to the pilot
 */
static int aiL_getnearestplanet( lua_State *L )
{
   double dist, d;
   int i, j;
   LuaVector lv;

   if (cur_system->nplanets == 0) return 0; /* no planets */

   /* cycle through planets */
   for (dist=0., j=-1, i=0; i<cur_system->nplanets; i++) {
      d = vect_dist( &cur_system->planets[i]->pos, &cur_pilot->solid->pos );
      if ((!areEnemies(cur_pilot->faction,cur_system->planets[i]->faction)) &&
            (d < dist)) { /* closer friendly planet */
         j = i;
         dist = d;
      }
   }

   /* no friendly planet found */
   if (j == -1) return 0;

   vectcpy( &lv.vec, &cur_system->planets[j]->pos );
   lua_pushvector(L, lv);

   return 1;
}


/*
 * returns a random planet's position to the pilot
 */
static int aiL_getrndplanet( lua_State *L )
{
   LuaVector lv;
   int p;

   if (cur_system->nplanets == 0) return 0; /* no planets */

   /* get a random planet */
   p = RNG(0, cur_system->nplanets-1);

   /* Copy the data into a vector */
   vectcpy( &lv.vec, &cur_system->planets[p]->pos );
   lua_pushvector(L, lv);

   return 1;
}

/*
 * returns a random friendly planet's position to the pilot
 */
static int aiL_getlandplanet( lua_State *L )
{
   Planet** planets;
   int nplanets, i;
   LuaVector lv;

   if (cur_system->nplanets == 0) return 0; /* no planets */

   /* Allocate memory. */
   planets = malloc( sizeof(Planet*) * cur_system->nplanets );

   /* Copy friendly planet.s */
   for (nplanets=0, i=0; i<cur_system->nplanets; i++)
      if (planet_hasService(cur_system->planets[i],PLANET_SERVICE_INHABITED) &&
            !areEnemies(cur_pilot->faction,cur_system->planets[i]->faction))
         planets[nplanets++] = cur_system->planets[i];

   /* no planet to land on found */
   if (nplanets==0) {
      free(planets);
      return 0;
   }

   /* we can actually get a random planet now */
   i = RNG(0,nplanets-1);
   vectcpy( &lv.vec, &planets[i]->pos );
   vect_cadd( &lv.vec, RNG(0, planets[i]->gfx_space->sw)-planets[i]->gfx_space->sw/2.,
         RNG(0, planets[i]->gfx_space->sh)-planets[i]->gfx_space->sh/2. );
   lua_pushvector( L, lv );
   free(planets);
   return 1;
}

/*
 * tries to enter the pilot in hyperspace, returns the distance if too far away
 */
static int aiL_hyperspace( lua_State *L )
{
   int dist;
   
   dist = space_hyperspace(cur_pilot);
   if (dist == 0.) {
      pilot_shootStop( cur_pilot, 0 );
      pilot_shootStop( cur_pilot, 1 );
      return 0;
   }

   lua_pushnumber(L,dist);
   return 1;
}

/*
 * Gets the relative velocity of a pilot.
 */
static int aiL_relvel( lua_State *L )
{
   unsigned int id;
   double dot, mod;
   Pilot *p;
   Vector2d vv, pv;

   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) {
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Get the projection of target on current velocity. */
   vect_cset( &vv, p->solid->vel.x - cur_pilot->solid->vel.x,
         p->solid->vel.y - cur_pilot->solid->vel.y );
   vect_cset( &pv, p->solid->pos.x - cur_pilot->solid->pos.x,
         p->solid->pos.y - cur_pilot->solid->pos.y );
   dot = vect_dot( &pv, &vv );
   mod = MAX(VMOD(pv), 1.); /* Avoid /0. */
   
   lua_pushnumber(L, dot / mod );
   return 1;
}

/*
 * completely stops the pilot if it is below minimum vel error (no instastops)
 */
static int aiL_stop( lua_State *L )
{
   (void) L; /* avoid gcc warning */

   if (VMOD(cur_pilot->solid->vel) < MIN_VEL_ERR)
      vect_pset( &cur_pilot->solid->vel, 0., 0. );

   return 0;
}

/*
 * Tells the pilot's escort's to attack it's target.
 */
static int aiL_e_attack( lua_State *L )
{
   int ret;
   ret = escorts_attack(cur_pilot);
   lua_pushboolean(L,!ret);
   return 1;
}

/* 
 * Tells the pilot's escorts to hold position.
 */
static int aiL_e_hold( lua_State *L )
{
   int ret;
   ret = escorts_hold(cur_pilot);
   lua_pushboolean(L,!ret);
   return 1;
}

/*
 * Tells the pilot's escorts to clear orders.
 */
static int aiL_e_clear( lua_State *L )
{
   int ret;
   ret = escorts_clear(cur_pilot);
   lua_pushboolean(L,!ret);
   return 1;
}

/*
 * Tells the pilot's escorts to return to dock.
 */
static int aiL_e_return( lua_State *L )
{
   int ret;
   ret = escorts_return(cur_pilot);
   lua_pushboolean(L,!ret);
   return 1;
}

/*
 * Docks the ship.
 */
static int aiL_dock( lua_State *L )
{
   unsigned int id;
   Pilot *p;

   /* Target is another ship. */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) {
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }
   pilot_dock(cur_pilot, p, 1);

   return 0;
}


/*
 * Aims at the pilot, trying to hit it.
 */
static int aiL_aim( lua_State *L )
{
   unsigned int id;
   double x,y;
   double t;
   Pilot *p;
   Vector2d tv;
   double dist, diff;
   double mod;
   double speed;
   NLUA_MIN_ARGS(1);

   /* Only acceptable parameter is pilot id */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) {
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Get the distance */
   dist = vect_dist( &cur_pilot->solid->pos, &p->solid->pos );

   /* Check if should recalculate weapon speed with secondary weapon. */
   if ((cur_pilot->secondary != NULL) &&
         outfit_isBolt(cur_pilot->secondary->outfit) &&
         (cur_pilot->secondary->outfit->type == OUTFIT_TYPE_LAUNCHER)) {
      speed  = cur_pilot->weap_speed + outfit_speed(cur_pilot->secondary->outfit);
      speed /= 2.;
   }
   else speed = cur_pilot->weap_speed;

   /* Time for shots to reach that distance */
   t = dist / speed;

   /* Position is calculated on where it should be */
   x = p->solid->pos.x + p->solid->vel.x*t
         - (cur_pilot->solid->pos.x + cur_pilot->solid->vel.x*t);
   y = p->solid->pos.y + p->solid->vel.y*t
      - (cur_pilot->solid->pos.y + cur_pilot->solid->vel.y*t);
   vect_cset( &tv, x, y );

   /* Calculate what we need to turn */
   mod = 10.;
   diff = angle_diff(cur_pilot->solid->dir, VANGLE(tv));
   pilot_turn = mod * diff;

   /* Return distance to target (in grad) */
   lua_pushnumber(L, ABS(diff*180./M_PI));
   return 1;
}


/*
 * toggles the combat flag, default is on
 */
static int aiL_combat( lua_State *L )
{
   int i;

   if (lua_gettop(L) > 0) {
      i = lua_toboolean(L,1);
      if (i==1) pilot_setFlag(cur_pilot, PILOT_COMBAT);
      else if (i==0) pilot_rmFlag(cur_pilot, PILOT_COMBAT);
   }
   else pilot_setFlag(cur_pilot, PILOT_COMBAT);

   return 0;
}


/*
 * sets the pilot's target
 */
static int aiL_settarget( lua_State *L )
{
   cur_pilot->target = luaL_checklong(L,1);
   return 0;
}


/**
 * @brief Checks to see if an outfit is a melee weapon.
 *    @param p Pilot to check for.
 *    @param o Outfit to check.
 */
static int outfit_isMelee( Pilot *p, PilotOutfitSlot *o )
{
   (void) p;
   if (outfit_isBolt(o->outfit) || outfit_isBeam(o->outfit) ||
         (((o->outfit->type == OUTFIT_TYPE_LAUNCHER) ||
           (o->outfit->type == OUTFIT_TYPE_TURRET_LAUNCHER)) &&
          (o->outfit->u.lau.ammo->u.amm.ai == 0)))

      return 1;
   return 0;
}
/**
 * @brief Checks to see if an outfit is a ranged weapon.
 *    @param p Pilot to check for.
 *    @param o Outfit to check.
 */
static int outfit_isRanged( Pilot *p, PilotOutfitSlot *o )
{
   (void) p;
   if (outfit_isFighterBay(o->outfit) ||
         (((o->outfit->type == OUTFIT_TYPE_LAUNCHER) ||
           (o->outfit->type == OUTFIT_TYPE_TURRET_LAUNCHER)) &&
          (o->outfit->u.lau.ammo->u.amm.ai > 0)))
      return 1;
   return 0;
}
/**
 * @brief Sets the secondary weapon, biased towards launchers
 */
static int aiL_secondary( lua_State *L )
{
   PilotOutfitSlot *co, *po;
   int i, melee;
   const char *str;
   const char *otype;
   int r;

   /* Parse the parameters. */
   str = luaL_checkstring(L,1);
   if (strcmp(str, "melee")==0)
      melee = 1;
   else if (strcmp(str, "ranged")==0)
      melee = 0;
   else NLUA_INVALID_PARAMETER();

   /* Pilot has secondary selected - use that */
   po = NULL;
   if (cur_pilot->secondary != NULL) {
      co = cur_pilot->secondary;
      if (melee && outfit_isMelee(cur_pilot,co))
         po = co;
      else if (!melee && outfit_isRanged(cur_pilot,co))
         po = co;
   }

   /* Need to get new secondary */
   if (po==NULL)  {
      /* Iterate over the list */
      for (i=0; i<cur_pilot->noutfits; i++) {
         co = cur_pilot->outfits[i];

         /* Must have an outfit. */
         if (co->outfit == NULL)
            continue;

         /* Not a secondary weapon. */
         if (!outfit_isProp(co->outfit, OUTFIT_PROP_WEAP_SECONDARY))
            continue;

         /* Get the first match. */
         if (melee && outfit_isMelee(cur_pilot,co)) {
            po = co;
            break;
         }
         else if (!melee && outfit_isRanged(cur_pilot,co)) {
            po = co;
            break;
         }
      }
   }

   /* Return 0 by default. */
   r = 0;

   /* Check to see if we have a good secondary weapon. */
   if (po != NULL) {
      cur_pilot->secondary = po;
      otype = outfit_getTypeBroad(po->outfit);
      lua_pushstring( L, otype );

      r = 1;

      /* Turret gets priority over launcher. */
      if (outfit_isTurret(po->outfit)) {
         lua_pushstring( L, "Turret" );
         r += 1;
      }

      /* Now we check for launcher. */
      if (outfit_isLauncher(po->outfit)) {

         /* Only if r == 1 in case of dumb turrets. */
         if (r == 1) {
            if (po->outfit->u.lau.ammo->u.amm.ai > 0)
               lua_pushstring( L, "Smart" );
            else
               lua_pushstring( L, "Dumb" );
            r += 1;
         }

         /* Get ammo. */
         if (cur_pilot->secondary->u.ammo.outfit == NULL)
            lua_pushnumber( L, 0. );
         else
            lua_pushnumber( L, cur_pilot->secondary->u.ammo.quantity );
         r += 1;
      }

      return r;
   }

   /* Return what was found. */
   return r;
}


/*
 * returns true if the pilot has turrets
 */
static int aiL_hasturrets( lua_State *L )
{
   lua_pushboolean( L, pilot_isFlag(cur_pilot, PILOT_HASTURRET) );
   return 1;
}


/*
 * makes the pilot shoot
 */
static int aiL_shoot( lua_State *L )
{
   int s;

   s = 0;

   if (lua_isboolean(L,1))
      s = lua_toboolean(L,1);
   if (!s && lua_isnumber(L,2))
      pilot_firemode = (int)lua_tonumber(L,2);

   if (s)
      ai_setFlag(AI_SECONDARY);
   else
      ai_setFlag(AI_PRIMARY);

   return 0;
}


/*
 * gets the nearest enemy
 */
static int aiL_getenemy( lua_State *L )
{
   unsigned int p;

   p = pilot_getNearestEnemy(cur_pilot);

   if (p==0) /* No enemy found */
      return 0;

   lua_pushnumber(L,p);
   return 1;
}


/*
 * sets the enemy hostile (basically notifies of an impending attack)
 */
static int aiL_hostile( lua_State *L )
{
   unsigned int id;
   Pilot *p;

   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) {
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   if (p->faction == FACTION_PLAYER)
      pilot_setHostile(cur_pilot);

   return 0;
}


/*
 * returns the maximum range of weapons, if parameter is 1, then does secondary
 */
static int aiL_getweaprange( lua_State *L )
{
   double range;

   /* if 1 is passed as a parameter, secondary weapon is checked */
   if (lua_toboolean(L,1)) {
      if (cur_pilot->secondary != NULL) {
         /* get range, launchers use ammo's range */
         if (outfit_isLauncher(cur_pilot->secondary->outfit) &&
               (cur_pilot->secondary->u.ammo.outfit != NULL))
            range = outfit_range(cur_pilot->secondary->u.ammo.outfit);
         else
            range = outfit_range(cur_pilot->secondary->outfit);

         if (range < 0.) {
            lua_pushnumber(L, 0.); /* secondary doesn't have range */
            return 1;
         }

         /* secondary does have range */
         lua_pushnumber(L, range);
         return 1;
      }
   }

   lua_pushnumber(L,cur_pilot->weap_range);
   return 1;
}


/**
 * @brief Checks to see if pilot can board the target.
 *
 *    @luaparam p Target to see if pilot can board.
 *    @luareturn true if pilot can board, false if it can't.
 * @luafunc canboard( p )
 */
static int aiL_canboard( lua_State *L )
{
   unsigned int id;
   Pilot *p;

   /* Get parameters. */
   id = luaL_checklong(L,1);
   p = pilot_get(id);
   if (p==NULL) {
      NLUA_ERROR(L, "Pilot ID does not belong to a pilot.");
      return 0;
   }

   /* Must be disabled. */
   if (!pilot_isDisabled(p)) {
      lua_pushboolean(L, 0);
      return 1;
   }

   /* Check if can be boarded. */
   lua_pushboolean(L, !pilot_isFlag(p, PILOT_BOARDED));
   return 1;
}


/**
 * @brief Attempts to board the pilot's target.
 *
 *    @luareturn true if was able to board the target.
 * @luafunc board( p )
 */
static int aiL_board( lua_State *L )
{
   lua_pushboolean(L, pilot_board( cur_pilot ));
   return 1;
}


/**
 * @brief Sees if pilot has finished refueling the target.
 *
 *    @luareturn true if target has finished refueling or false if it hasn't.
 */
static int aiL_donerefuel( lua_State *L )
{
   lua_pushboolean(L, !pilot_isFlag(cur_pilot, PILOT_REFUELING));
   return 1;
}


/**
 * @brief Attempts to refuel the pilot's target.
 *
 *    @luareturn true if pilot has begun refueling, false if it hasn't.
 */
static int aiL_refuel( lua_State *L )
{
   lua_pushboolean(L,pilot_refuelStart(cur_pilot));
   return 1;
}


/*
 * sets the timer
 */
static int aiL_settimer( lua_State *L )
{
   int n;

   /* Get parameters. */
   n = luaL_checkint(L,1);

   /* Set timer. */
   cur_pilot->timer[n] = (lua_isnumber(L,2)) ? lua_tonumber(L,2)/1000. : 0;

   return 0;
}

/*
 * checks the timer
 */
static int aiL_timeup( lua_State *L )
{
   int n;

   /* Get parameters. */
   n = luaL_checkint(L,1);

   lua_pushboolean(L, cur_pilot->timer[n] < 0.);
   return 1;
}


/*
 * makes the pilot say something to the player
 */
static int aiL_comm( lua_State *L )
{
   unsigned int p;
   const char *s;
 
   /* Get parameters. */
   p = luaL_checklong(L,1);
   s = luaL_checkstring(L,2);

   /* Send the message. */
   pilot_message( cur_pilot, p, s, 0 );

   return 0;
}

/*
 * broadcasts to the entire area
 */
static int aiL_broadcast( lua_State *L )
{
   const char *str;

   str = luaL_checkstring(L,1);
   pilot_broadcast( cur_pilot, str, 0 );

   return 0;
}


/*
 * Sends a distress signal.
 */
static int aiL_distress( lua_State *L )
{
   NLUA_MIN_ARGS(1);

   if (lua_isstring(L,1))
      snprintf( aiL_distressmsg, PATH_MAX, "%s", lua_tostring(L,1) );
   else if (lua_isnil(L,1))
      aiL_distressmsg[0] = '\0';
   else
      NLUA_INVALID_PARAMETER();

   /* Set flag because code isn't reentrant. */
   ai_setFlag(AI_DISTRESS);

   return 0;
}


/*
 * sets the pilot_nstack credits
 */
static int aiL_credits( lua_State *L )
{
   if (aiL_status != AI_STATUS_CREATE) {
      /*NLUA_ERROR(L, "This function must be called in \"create\" only.");*/
      return 0;
   }

   cur_pilot->credits = luaL_checklong(L,1);
   
   return 0;
}


/*
 * sets the pilot_nstack cargo
 */
static int aiL_cargo( lua_State *L )
{
   NLUA_MIN_ARGS(2);
   int q;
   const char *s;

   if (aiL_status != AI_STATUS_CREATE) {
      /*NLUA_ERROR(L, "This function must be called in \"create\" only.");*/
      return 0;
   }

   /* Get parameters. */
   s = luaL_checkstring(L,1);
   q = luaL_checkint(L,2);

   /* Quantity must be valid. */
   if (q<=0)
      return 0;

   pilot_addCargo( cur_pilot, commodity_get(s), q);

   return 0;
}


/*
 * gets the pilot's ship value
 */
static int aiL_shipprice( lua_State *L )
{
   lua_pushnumber(L, cur_pilot->ship->price);
   return 1;
}

/**
 * @}
 */
