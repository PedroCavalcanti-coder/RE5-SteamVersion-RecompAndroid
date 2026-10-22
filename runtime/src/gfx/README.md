# `runtime/src/gfx/` — Direct3D 9 → DXVK Native (Fase 4, etapa 4.2)

Planejado (decisão D4 em `docs/ARQUITETURA.md`):

- `d3d9_bridge.c` — `Direct3DCreate9` devolve um objeto-sombra na memória do guest
  cuja vtable é feita de thunks; cada método traduz argumentos (VAs do guest →
  ponteiros do host) e chama o objeto da DXVK Native (`third_party/dxvk-native`).
- `gen_d3d9_bridge.py` — gera as vtables e os stubs de tradução a partir da lista
  de métodos de cada interface (`IDirect3D9`, `IDirect3DDevice9`, texturas,
  buffers, shaders, queries), como `tools/nfsu2-reference/ports/dxvk_android/generate_device_bridge.py`.
- `Lock/Unlock` — buffers-sombra na memória do guest, copiados no `Unlock`.
- D3DX9 — só o subconjunto que o censo da etapa 1.2 mostrar.
