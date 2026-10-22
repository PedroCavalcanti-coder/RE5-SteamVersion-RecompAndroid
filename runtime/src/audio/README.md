# `runtime/src/audio/` — XAudio2 / X3DAudio → FAudio (Fase 4, etapa 4.3)

Planejado: `CoCreateInstance(CLSID_XAudio2)` (via HLE do `ole32`) devolve um objeto
COM-sombra (decisão D4) sobre o FAudio (`third_party/FAudio`); voices, buffers e
callbacks do guest (`IXAudio2VoiceCallback`) chamados por `re5_guest_call` com o
lock do guest. X3DAudio → F3DAudio. DirectSound (`DSOUND.dll` ordinal 11 =
`DirectSoundCreate8`) só se o build limpo de fato usar.
