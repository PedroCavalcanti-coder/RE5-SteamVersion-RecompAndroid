/*
 * xinput.h - XInput 1.3 sobre SDL_GameController (runtime/src/win32/xinput1_3.c).
 */
#ifndef RE5_XINPUT_H
#define RE5_XINPUT_H

#include <stdint.h>

/* Monta o XINPUT_GAMEPAD (12 bytes: wButtons, bLeftTrigger, bRightTrigger,
 * sThumbLX/LY/RX/RY) a partir do estado do SDL: axis[] na ordem de
 * SDL_GameControllerAxis e buttons com o bit (1 << SDL_CONTROLLER_BUTTON_*).
 * Os eixos Y do SDL crescem para baixo; os do XInput, para cima. */
void re5_xinput_gamepad(const int16_t axis[6], uint32_t buttons, uint8_t out[12]);

#endif /* RE5_XINPUT_H */
