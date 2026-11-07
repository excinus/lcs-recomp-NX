/*
 *  CheatDevice Remastered
 *  Copyright (C) 2017-2025, Freakler
 *  
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with self_ program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CHEATS_H__
#define __CHEATS_H__

#include <psptypes.h>

#include "main.h"

int PatchLCS(u32 addr, u32 text_addr);
int PatchVCS(u32 addr, u32 text_addr);

// #define MAKE_CALL: see cdr_platform.h (code patches are recorded, not applied)

// #define MAKE_JUMP: see cdr_platform.h (code patches are recorded, not applied)

// #define HIJACK_FUNCTION: see cdr_platform.h (code patches are recorded, not applied)

// #define REDIRECT_FUNCTION: see cdr_platform.h (code patches are recorded, not applied)

// #define MAKE_DUMMY_FUNCTION: see cdr_platform.h (code patches are recorded, not applied)


typedef struct {
  char* title;
  short id;
  char unlocked;
} achievement_pack;

/// Calls 
SceInt64 sceKernelGetSystemTimeWidePatched(void);

inline GuestFunction<int()> GetPPLAYER;
inline GuestFunction<int()> GetPOBJ;
inline GuestFunction<int()> GetPCAR;

inline GuestFunction<void(short weathercode)> SetNextWeather;
inline GuestFunction<void(short weathercode)> SetWeatherNow;
inline GuestFunction<void(void)> ReleaseWeather;

inline GuestFunction<void(unsigned int ped, int val, unsigned int object)> TaskCharWith;
inline GuestFunction<void(int ped, int vehicle)> WarpPedIntoVehicle; // FUN_001b90ec_warpPedIntoVehicle(int ped, int veh) (has more attributes in VCS!!!!)
inline GuestFunction<void(int ped, int vehicle, int seat)> WarpPedIntoVehicleAsPassenger; // FUN_001b9470_warpPedIntoVehicleAsPassenger(int ped, int veh, int seat)
inline GuestFunction<void(unsigned int model_id, unsigned int param_2)> RequestModel; // , unsigned int param_3, unsigned int param_4, unsigned int param_5);
inline GuestFunction<int(int ped_obj, int param_2, unsigned int ammo)> GiveWeaponAndAmmo; // , unsigned int param_4, unsigned int param_5);
inline GuestFunction<void(int ped_obj, const char *name)> SetActorSkinTo; // name must be lower case!!
inline GuestFunction<void(char x)> LoadAllModelsNow;
inline GuestFunction<void(int ped_obj)> RefreshActorSkin;
inline GuestFunction<void(int ped_obj)> TaskDuckLCS;
inline GuestFunction<void(int ped_obj, int duration, char x)> TaskDuckVCS;
inline GuestFunction<void(int ped_obj, char x)> TaskUnDuck;

inline GuestFunction<int(int pplayer, int stars)> SetWantedLevel;
inline GuestFunction<int(int stars)> SetMaxWantedLevel;

inline GuestFunction<int(int ip)> StartNewScript;

inline GuestFunction<void(int handle)> TankControl;
inline GuestFunction<void(int handle)> BlowupVehiclesInPath;

inline GuestFunction<void(int handle)> DoHoverSuspensionRatios;

inline GuestFunction<int(void)> IsPlayerOnAMission;
inline GuestFunction<void(int state)> SetBridgeState;

inline GuestFunction<int(int gxt_adr,char *string,int param_3,int param_4,int param_5, int param_6,int param_7,int param_8)> LoadStringFromGXT;
int LoadStringFromGXT_patched(int gxt_adr,char *string, int param_3, int param_4, int param_5, int param_6,int param_7,int param_8);

inline GuestFunction<int()> GetTEST; // for testing

short cameraX(short *pad);
short cameraY(short *pad);
short aimX(short *pad);
short aimY(short *pad);


/// Hijacks
inline GuestFunction<int(void *a1)> buttonsToAction;
int buttonsToActionPatched(void *a1);

inline GuestFunction<void(void *self_, int mode)> cWorldStream_Render;
void cWorldStream_Render_Patched(void *self_, int mode);

inline GuestFunction<void(int param_1)> FUN_002c22a0;
void FUN_002c22a0_patched(int param_1);

inline GuestFunction<void(int param_1)> FUN_002db0c0;
void FUN_002db0c0_patched(int param_1);

inline GuestFunction<int(int param_1)> _checkCustomTracksReady;
int _checkCustomTracksReady_patched(int param_1);

inline GuestFunction<unsigned int(int* x,int y,int z, int a, int b)> loadSplashScreen;
unsigned int loadSplashScreen_patched(int* x,int y,int z, int a, int b);

inline GuestFunction<void(char * string1, char * string2, char *txdname, unsigned int param_4)> Loadscreen;
void Loadscreen_patched(char * string1, char * string2, char *txdname, unsigned int param_4);

inline GuestFunction<void(float param_1)> DrawLoadingBar;
void DrawLoadingBar_patched(float param_1);

inline GuestFunction<void(int handle)> UpdatePosition;
void UpdatePosition_patched(int handle);

//#ifdef SWIM
inline GuestFunction<int(float param_1,float param_2,float *param_3)> FUN_00109dac_CWaterLevel_GetWaterLevel;
inline GuestFunction<int(int param_1)> FUN_000e7d70_CCam_IsTargetInWater;
int FUN_000e7d70_CCam_IsTargetInWater_patched(int param_1);
inline GuestFunction<void(int param_1)> FUN_001a8d9c_CPed_ProcessBuoyancy;
void *FUN_001a8d9c_CPed_ProcessBuoyancy_patched(int param_1);
//#endif

/// Cheat functions 
void *debug_monitor(int calltype, int keypress, int defaultstatus, int defaultval);
void *debug_vars(int calltype, int keypress, int defaultstatus, int defaultval);

void *customtracks(int calltype, int keypress, int defaultstatus);

void *fps_toggle(int calltype, int keypress, int defaultstatus);
void *mem_toggle(int calltype, int keypress, int defaultstatus);
void *coords_toggle(int calltype, int keypress, int defaultstatus);
void *speedometer_toggle(int calltype, int keypress, int defaultstatus, int defaultval);

void *fps_cap(int calltype, int keypress, int defaultstatus, int defaultval);
void *neverfalloffbike(int calltype, int keypress, int defaultstatus);
void *playermodel(int calltype, int keypress, int defaultstatus);
void *skip_intros(int calltype, int keypress, int defaultstatus);
void *teleporter(int calltype, int keypress, int defaultstatus, int defaultval);
void *godmode(int calltype, int keypress, int defaultstatus);
void *warp_out_water(int calltype, int keypress, int defaultstatus);
void *warp_out_water_veh(int calltype, int keypress, int defaultstatus);
void *invisible(int calltype, int keypress, int defaultstatus);
void *ignored(int calltype, int keypress, int defaultstatus);
void *unlimited_ammo(int calltype, int keypress, int defaultstatus);
void *heavy_player(int calltype, int keypress, int defaultstatus);
void *aim_of_death(int calltype, int keypress, int defaultstatus);
void *indestr_vehicle(int calltype, int keypress, int defaultstatus);
void *lockdoors(int calltype, int keypress, int defaultstatus);
void *hover_vehicle(int calltype, int keypress, int defaultstatus);
void *heavy_vehicle(int calltype, int keypress, int defaultstatus);
void *world_weather(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_gravity(int calltype, int keypress, int defaultstatus, int defaultval);
void *vehicle_base_color(int calltype, int keypress, int defaultstatus);
void *vehicle_stripe_color(int calltype, int keypress, int defaultstatus);
void stepthroughwall();
void *traffic_density(int calltype, int keypress, int defaultstatus);
void *traffic_freeze(int calltype, int keypress, int defaultstatus);
void *peds_density(int calltype, int keypress, int defaultstatus);
void *peds_killaiming(int calltype, int keypress, int defaultstatus);
void *peds_freeze(int calltype, int keypress, int defaultstatus);
void *max_health(int calltype, int keypress);
void *max_armor(int calltype, int keypress);
void *powerjump(int calltype, int keypress, int defaultstatus);
void *mission_select(int calltype, int keypress, int defaultstatus, int defaultval);
void *load_building(int calltype, int keypress, int defaultstatus, int defaultval);
void *wanted_level(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_time(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_realtimeclock(int calltype, int keypress, int defaultstatus);
void *no_cheating_warning(int calltype, int keypress, int defaultstatus);
void *traffic_radiostation(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_waterlevel(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_waveheight(int calltype, int keypress, int defaultstatus, int defaultval);
void *powerbrake(int calltype, int keypress, int defaultstatus);
void *nitro(int calltype, int keypress, int defaultstatus);
void *tank(int calltype, int keypress, int defaultstatus);
void *driveonwater(int calltype, int keypress, int defaultstatus);
void *automaticflipover(int calltype, int keypress, int defaultstatus) ;
void *driveonwalls(int calltype, int keypress, int defaultstatus);
void *unlimited_sprinting(int calltype, int keypress, int defaultstatus);
void *unlimited_swimming(int calltype, int keypress, int defaultstatus);
void *pickup_spawner(int calltype, int keypress, int defaultstatus, int defaultval);
void *player_model(int calltype, int keypress, int defaultstatus, int defaultval);
void *vehicle_spawner(int calltype, int keypress, int defaultstatus, int defaultval);
void *teleport_next(int calltype, int keypress);
void teleport_next_sub();
void teleport_marker();
void teleport_mission();
void kill_all_targets();
void freeze_all_targets();
void mission_selector();
void *down_button(int calltype, int keypress, int defaultstatus, int defaultval);
void *up_button(int calltype, int keypress, int defaultstatus, int defaultval);
void *markonmap(int calltype, int keypress, int defaultstatus, int defaultval);
void *debug_loadscreens(int calltype, int keypress, int defaultstatus);
void *random_loadscreens(int calltype, int keypress, int defaultstatus);
void *disable_textures(int calltype, int keypress, int defaultstatus);
void *touch_pedestrian(int calltype, int keypress, int defaultstatus, int defaultval);
void *touch_vehicle(int calltype, int keypress, int defaultstatus, int defaultval);
void *touch_object(int calltype, int keypress, int defaultstatus, int defaultval);
void *camera_centered(int calltype, int keypress, int defaultstatus);
void *fieldofview(int calltype, int keypress, int defaultstatus, int defaultval);
void *camera_topdown(int calltype, int keypress, int defaultstatus);
void *gamespeed(int calltype, int keypress, int defaultstatus, int defaultval);
void *dev_flag(int calltype, int keypress, int defaultstatus);
void *freeze_timers(int calltype, int keypress, int defaultstatus);
void *onmission(int calltype, int keypress, int defaultstatus, int defaultval);
void *world_liftcontrol(int calltype, int keypress, int defaultstatus, int defaultval);
void *walking_speed(int calltype, int keypress, int defaultstatus, int defaultval);
void *pedwalking_speed(int calltype, int keypress, int defaultstatus, int defaultval);
void *gather_spell(int calltype, int keypress, int defaultstatus);
void *rocketboost(int calltype, int keypress, int defaultstatus, int defaultval);
void *untouchable(int calltype, int keypress, int defaultstatus);
void *bmxjumpheight(int calltype, int keypress, int defaultstatus, int defaultval);
void *policechaseheli(int calltype, int keypress, int defaultstatus, int defaultval);
void *fake_swimming(int calltype, int keypress, int defaultstatus, int defaultval);


void bttncht_givehealth();
void bttncht_givearmor();
void bttncht_givemoney();
void bttncht_weaponset1();
void bttncht_weaponset2();
void bttncht_weaponset3();
void bttncht_carsonwater(); // LCS only
void bttncht_playcredits(); // LCS only
void *bttncht_randomplayer(int calltype, int keypress, int defaultstatus, int defaultval); // LCS only
void bttncht_unlockmult100();
void bttncht_perftraction();
void bttncht_blowupcars();
void bttncht_spawnrhino();
void bttncht_bubblehead(); // LCS only
void bttncht_pedsweapons();
void bttncht_pedsattack();
void bttncht_pedsriot();
void bttncht_agrodrivers();
void bttncht_traffichrome();


void *cdr_showpopular(int calltype, int keypress, int defaultstatus);
void *cdr_collapsecats(int calltype, int keypress, int defaultstatus);
void *cdr_alternativefont(int calltype, int keypress, int defaultstatus);
void *cdr_autostartmenu(int calltype, int keypress, int defaultstatus);
void *cdr_freezegameinmenu(int calltype, int keypress, int defaultstatus);
void *cdr_uselegend(int calltype, int keypress, int defaultstatus);
void *cdr_allowuiblocking(int calltype, int keypress, int defaultstatus);
void *cdr_liveconfig(int calltype, int keypress, int defaultstatus);
void *cdr_advancedconfig(int calltype, int keypress, int defaultstatus);
void *cdr_swapacceleration(int calltype, int keypress, int defaultstatus);

#ifdef LANG
void *cdr_changelang(int calltype, int keypress, int defaultstatus, int defaultval);
#endif

void load_defaults(const Menu_pack *menu_list, int menu_max);
void exit_game();

void user_cheats(int calltype, int keypress, int defaultstatus);
void user_scripts(int calltype, int keypress, int defaultstatus);
void savegame_editor();
void freecam(int calltype, int keypress, int defaultstatus);
void hexeditor();
void hexeditpplayer();
void hexeditpcar();

void *mp_test(int calltype, int keypress, int defaultstatus);

// ACHIEVMENTS
void achievements();
void achievements_reset();

// PREVIEW
void test_func();
void *test_switch(int calltype, int keypress, int defaultstatus);

// GAMELOG
inline GuestPrintf debugprint; 
void debugprint_patched(const char *text, ...);
void drawGameLog();

// MEMORY
void drawMemoryUsage();

#endif
