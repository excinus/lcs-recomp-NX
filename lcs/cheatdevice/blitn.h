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

#ifndef __BLITN_H__
#define __BLITN_H__

#include <psptypes.h>


#define SCREEN_WIDTH 480.0f
#define SCREEN_HEIGHT 272.0f

/// my ugly text storeing thingy 
#define MAX_TEXTS 512  // number of texts that can be displayed at once
#define MSGLENGTH 128  // length messages can have

/// and boxes too
#define MAX_BOXES 128

#define FONT_NAMES 0   // used for LOCATION NAME, VEHICLE NAME, RADIO-STATION NAME, MENU TABs (italic on VCS)
#define FONT_DIALOG 1  // used for DIALOGS, in MESSAGE BOX etc
#define FONT_GTA 2     // used for "MISSION SUCCESS!", "WASTED!" etc and mainmenu categories

#define SHADOW_ON 2    // only works for LCS
#define SHADOW_OFF 0

#define ALIGN_FREE 0
#define ALIGN_CENTER 2
#define ALIGN_LEFT 3
#define ALIGN_RIGHT 4
#define ALIGN_SCREENCENTER 6

#define SIZE_LCS_SMALL 0.6f
#define SIZE_LCS_NORMAL 0.8f
#define SIZE_LCS_BIG 1.0f
#define SIZE_LCS_HUGE 1.2f

#define SIZE_VCS_SMALL 0.46f //0.5f
#define SIZE_VCS_NORMAL 0.6f
#define SIZE_VCS_BIG 0.8f 
#define SIZE_VCS_HUGE 1.0f 


// #define MAKE_JUMP: see cdr_platform.h (code patches are recorded, not applied)

// #define HIJACK_FUNCTION: see cdr_platform.h (code patches are recorded, not applied)

typedef struct menu_blit_text
{
  wchar_t text[MSGLENGTH];
  u32 color;
  short style;
  short shadow;
  int origin;
  float scale;
  ScePspFVector2 pos;
} menu_blit_text;

typedef struct menu_blit_box
{
  short id;
  u32 color;
  ScePspFVector2 pos;
  ScePspFVector2 size;
} menu_blit_box;

/// box
inline GuestFunction<char *(char *param_1,char alpha,char red,char green,char blue)> SetBoxColor;
inline GuestFunction<float *(float param_1, float param_2, float param_3, float param_4, float *param_5)> SetBoxCoords;
inline GuestFunction<void(float *param_1,unsigned int *param_2,char param_3)> DrawRect;

      
/// string
inline GuestFunction<void(float X, float Y, wchar_t* text, int* unknown)> PrintString_LCS;
inline GuestFunction<void(wchar_t* text, int x, int y)> PrintString_VCS;
inline GuestFunction<void()> SetRightJustifyOn;
inline GuestFunction<void()> SetRightJustifyOff;
inline GuestFunction<void()> SetCentreOff;
inline GuestFunction<void()> SetCentreOn;
inline GuestFunction<void()> SetJustifyOn;
inline GuestFunction<void()> SetJustifyOff;
inline GuestFunction<void(u32* color)> SetColor;
inline GuestFunction<void(u32* color)> SetBackgroundColor;
inline GuestFunction<void(short style)> SetFontStyle;
inline GuestFunction<void(short bool_box)> SetDropShadowPosition;
inline GuestFunction<void(char* rgba)> SetDropColor;
inline GuestFunction<void(float width, float height)> SetScale_LCS;
inline GuestFunction<void(float scale)> SetScale_VCS;
inline GuestFunction<void()> SetBackGroundOnlyTextOn;
inline GuestFunction<void()> SetBackGroundOnlyTextOff;
inline GuestFunction<void()> SetPropOn;
inline GuestFunction<void()> SetPropOff;
inline GuestFunction<void()> SetBackgroundOn;
inline GuestFunction<void()> SetBackgroundOff;
inline GuestFunction<void(int origin)> SetTextOriginPoint;
inline GuestFunction<void(int option)> SetTextSpaceing;
inline GuestFunction<void(float* array)> SetTextBounds;
inline GuestFunction<void()> ResetFontStyling;
inline GuestFunction<void(float x)> SetRightJustifyWrap;
inline GuestFunction<void(float x)> SetWrapx;
inline GuestFunction<void(float param_1)> SetSlant;
inline GuestFunction<void(float param_1)> SetCentreSize;
inline GuestFunction<void(float param_1, float param_2)> SetSlantRefPoint;


/// colors (0xAABBGGRR)
#define RED     0xFF0000FF
#define GREEN   0xFF00FF00
#define BLUE    0xFFFF0000
#define CYAN    0xFFffff00
#define MAGENTA 0xFFff00ff
#define YELLOW  0xFF00ffff
#define AZURE   0xFFFF7F00
#define VIOLET  0xFFFF007F
#define ORANGE  0xFF007FFF
#define GREY    0xFF808080
#define LGREY   0xFFA0A0A0
#define WHITE   0xFFFFFFFF
#define BLACK   0xFF000000
#define CLEAR   0x00000000

#define ALPHABLACK  0x88000000 // used for help-box, map legend by game
#define ALPHAWHITE  0x99FFFFFF // 

#define LIGHTBLUE  0xFFFF9900

#define ORANGERED  0xFF0055FF
#define DARKGREEN  0xFF1BA000

/// original colors used in the ogs
#define CHDVC_YELLOW  0xFF66ffff
#define CHDVC_ORANGE  0xFF05AFFF
#define CHDVC_MAGENTA 0xFF5C00A2
#define CHDVC_AZURE   0xFFAEE223
#define CHDVC_GREEN   0xFF00FF00

#define LCS_HEALTH   0xFF00008E
#define VCS_HEALTH   0xFF8056BB // not 100% original
#define LCS_ARMOR    0xFFBE782A
#define VCS_ARMOR    0xFFCFD714 // not 100% original 

void drawBox(float x, float y, float width, float height, u32 color);
void drawUiBox(float x, float y, float width, float height, float bordersize, u32 bordercolor, u32 bgcolor);

void drawString(const char *string, int origin, short style, float scale, short shadow, float x, float y, u32 color);
void drawStringLCS(const char *string, int origin, short style, float scale, short shadow, float x, float y, u32 color);
void drawStringVCS(const char *string, int origin, short style, float scale, short shadow, float x, float y, u32 color);

void Utf8ToGameEncoding(const char *in, wchar_t *out);
void Utf8ToGameEncodingGuest(const char *in, u32 out_address); // LCSNative

void cWorldStream_Render_LCS_Patched(void *self_, int mode);
void cWorldStream_Render_VCS_Patched(void *self_, int mode);

int initTextBlit(u32 text_addr, u32 text_size);

#endif