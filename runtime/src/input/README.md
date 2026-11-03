# `runtime/src/input/` — XInput / DirectInput8 → SDL2 (Fase 4, etapa 4.4)

O que foi planejado aqui está em `runtime/src/win32/`: `xinput1_3.c`
(`XInputGetState`/`SetState` sobre `SDL_GameController`), `dinput8.c` (teclado e mouse
do SDL2 em `IDirectInput8A`) e `touch.c` (controles de toque do Android: dispositivo
próprio do runtime, somado ao usuário 0 do XInput; ver `re5/touch.h` e a seção 4 do
`docs/PLANO_APP_ANDROID.md`).
