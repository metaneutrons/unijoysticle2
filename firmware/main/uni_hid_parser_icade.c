/****************************************************************************
http://retro.moe/unijoysticle2

Copyright 2019 Ricardo Quesada

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
****************************************************************************/

#include "uni_hid_parser_icade.h"

#include "hid_usage.h"
#include "uni_debug.h"

/*
 *   ↑      A C Y L
 *  ← →
 *   ↓      B X Z R
 *
 *
 *  UP ON,OFF  = w,e
 *  RT ON,OFF  = d,c
 *  DN ON,OFF  = x,z
 *  LT ON,OFF  = a,q
 *  A  ON,OFF  = y,t        : Mapped to Button A
 *  B  ON,OFF  = h,r        : Mapped to Button B
 *  C  ON,OFF  = u,f        : Mapped to Button X
 *  X  ON,OFF  = j,n        : Mapped to Button Y
 *  Y  ON,OFF  = i,m        : unmapped
 *  Z  ON,OFF  = k,p        : unmapped
 *  L  ON,OFF  = o,g        : Mapped to "Menu" button
 *  R  ON,OFF  = l,v        : unmapped
 */

void uni_hid_parser_icade_init(uni_gamepad_t* gp) {
  UNUSED(gp);
  // Don't do anything, like reseting the gp->updated_states,
  // since each report is a "delta", and doesn't include a full state.
}

void uni_hid_parser_icade_parse_usage(uni_gamepad_t* gp,
                                      hid_globals_t* globals,
                                      uint16_t usage_page,
                                      uint16_t usage,
                                      int32_t value) {
  UNUSED(globals);
  switch (usage_page) {
    case HID_USAGE_PAGE_KEYBOARD_KEYPAD:
      switch (usage) {
        case 0x00:  // reserved. ignore
        case 0xe0:  // from 0xe0 - 0xe7: ignore
        case 0xe1:
        case 0xe2:
        case 0xe3:
        case 0xe4:
        case 0xe5:
        case 0xe6:
        case 0xe7:
          break;
        case 0x1a:  // w (up on)
          gp->dpad |= DPAD_UP;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x08:  // e (up off)
          gp->dpad &= ~DPAD_UP;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x07:  // d (right on)
          gp->dpad |= DPAD_RIGHT;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x06:  // c (right off)
          gp->dpad &= ~DPAD_RIGHT;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x1b:  // x (down on)
          gp->dpad |= DPAD_DOWN;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x1d:  // z (down off)
          gp->dpad &= ~DPAD_DOWN;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x04:  // a (left on)
          gp->dpad |= DPAD_LEFT;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x14:  // q (left off)
          gp->dpad &= ~DPAD_LEFT;
          gp->updated_states |= GAMEPAD_STATE_DPAD;
          break;
        case 0x1c:  // y (button A: on)
          gp->buttons |= BUTTON_A;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_A;
          break;
        case 0x17:  // t (button A: off)
          gp->buttons &= ~BUTTON_A;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_A;
          break;
        case 0x0b:  // h (button B: on)
          gp->buttons |= BUTTON_B;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_B;
          break;
        case 0x15:  // r (button B: off)
          gp->buttons &= ~BUTTON_B;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_B;
          break;
        case 0x18:  // u (button C: on)
          gp->buttons |= BUTTON_X;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_X;
          break;
        case 0x09:  // f (button C: off)
          gp->buttons &= ~BUTTON_X;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_X;
          break;
        case 0x0d:  // j (button X: on)
          gp->buttons |= BUTTON_Y;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_Y;
          break;
        case 0x11:  // n (button X: off)
          gp->buttons &= ~BUTTON_Y;
          gp->updated_states |= GAMEPAD_STATE_BUTTON_Y;
          break;
        case 0x12:  // o (button L: on)
          gp->misc_buttons |= MISC_BUTTON_SYSTEM;
          gp->updated_states |= GAMEPAD_STATE_MISC_BUTTON_SYSTEM;
          break;
        case 0x0a:  // g (button L: off)
          gp->misc_buttons &= ~MISC_BUTTON_SYSTEM;
          gp->updated_states |= GAMEPAD_STATE_MISC_BUTTON_SYSTEM;
          break;
        case 0x0c:  // i,m,k,p,l,v. Buttons Y,Z,R
        case 0x10:  // m
        case 0x0e:  // k
        case 0x13:  // p
        case 0x0f:  // l
        case 0x19:  // v
          // unmapped buttons
          break;
        default:
          logi("iCade: Unsupported page: 0x%04x, usage: 0x%04x, value=0x%x\n", usage_page, usage, value);
          break;
      }
  }
}