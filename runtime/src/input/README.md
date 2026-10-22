# `runtime/src/input/` — XInput / DirectInput8 → SDL2 (Fase 4, etapa 4.4)

Planejado: `XInputGetState`/`XInputSetState` (`XINPUT1_3.dll`, também por ordinal)
sobre `SDL_GameController`; `DirectInput8Create` devolvendo dispositivos COM-sombra
de teclado e mouse alimentados por eventos do SDL2. No Android, os controles touch
(Fase 6) entram aqui como um gamepad virtual.
