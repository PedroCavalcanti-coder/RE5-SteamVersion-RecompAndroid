# `runtime/src/video/` — WMVCore → FFmpeg (Fase 4, etapa 4.5)

Planejado: `WMCreateSyncReader` devolve um `IWMSyncReader`-sombra (decisão D4) que
demultiplexa ASF e decodifica WMV3/VC-1 e WMA com o FFmpeg (`third_party/FFmpeg`),
entregando quadros no formato que o jogo pede ao `IWMOutputMediaProps`.
