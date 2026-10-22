# Direct3D 9 usage (measured)

Source: `src/runtime/d3d9_trace.c` (`D3D_TRACE=summary`, the default), one
run of the recompiled game through boot and the intro movies, 1,320 frames.
Every run rewrites `logs/d3d9_usage_<stamp>.txt` with the same layout; this is
the specification the Vulkan renderer has to meet, and it grows as more of the
game is reached (menus and races will add states and formats).

## Device

`IDirect3D9::CreateDevice`: adapter 0, `D3DDEVTYPE_HAL`, behaviour `0x50`
(`HARDWARE_VERTEXPROCESSING | FPU_PRESERVE`), back buffer 640x480
`D3DFMT_X8R8G8B8` (21) x1, no MSAA, `D3DSWAPEFFECT_DISCARD`, **fullscreen**,
auto depth-stencil `D3DFMT_D24S8` (75), `D3DPRESENT_INTERVAL_IMMEDIATE`.
`Reset` is used (device-lost handling when the window loses focus).

## Per frame (intro)

~496 D3D calls, 14 draws, 14 `SetTexture`, ~166 `SetRenderState` per frame.

## Methods called

| Method | calls | | Method | calls |
|---|---|---|---|---|
| SetRenderState | 230,771 | | SetVertexShader / SetPixelShader | 19,822 / 19,822 |
| SetTextureStageState | 185,536 | | SetVertexShaderConstantF | 19,808 |
| SetSamplerState | 146,661 | | DrawPrimitiveUP | 19,766 |
| SetTexture | 19,866 | | SetVertexDeclaration | 6,641 |
| SetRenderTarget / SetDepthStencilSurface | 2,644 / 2,644 | | SetViewport | 2,620 |
| Clear | 1,343 | | BeginScene / EndScene | 1,311 / 1,311 |
| TestCooperativeLevel | 1,328 | | Present | 1,320 |
| CreateTexture | 814 | | BeginStateBlock / EndStateBlock | 421 / 380 |
| GetDeviceCaps | 103 | | CreateVertexDeclaration | 79 |
| CreateVertexShader / CreatePixelShader | 34 / 34 | | ValidateDevice | 33 |
| CreateDepthStencilSurface | 32 | | CreateVertexBuffer / CreateIndexBuffer | 31 / 31 |
| SetPixelShaderConstantF | 32 | | LightEnable | 10 |
| CreateCubeTexture | 8 | | GetBackBuffer | 8 |
| Reset | 6 | | GetDepthStencilSurface | 4 |
| SetStreamSource / SetIndices | 3 / 3 | | CreateRenderTarget / StretchRect | 2 / 2 |
| SetFVF | 2 | | GetAvailableTextureMem | 2 |

Not seen yet: `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawIndexedPrimitiveUP`,
`SetTransform`, fixed-function lighting beyond `LightEnable`, queries.

## Values seen

- **Render states** (`D3DRENDERSTATETYPE`): 7 ZENABLE, 14 ZWRITEENABLE, 15 ALPHATESTENABLE, 19 SRCBLEND, 20 DESTBLEND, 22 CULLMODE, 23 ZFUNC, 24 ALPHAREF, 25 ALPHAFUNC, 27 ALPHABLENDENABLE, 28 FOGENABLE, 29 SPECULARENABLE, 34 FOGCOLOR, 60 TEXTUREFACTOR, 137 LIGHTING, 141 COLORVERTEX, 145 VERTEXBLEND, 161 MULTISAMPLEANTIALIAS, 168 COLORWRITEENABLE.
- **Texture stage states**: 1 COLOROP, 4 ALPHAOP, 11 TEXCOORDINDEX, 24 TEXTURETRANSFORMFLAGS.
- **Sampler states**: 1 ADDRESSU, 2 ADDRESSV, 5 MAGFILTER, 6 MINFILTER, 7 MIPFILTER, 10 MAXANISOTROPY.
- **Texture formats**: X8R8G8B8 (21), DXT1 (`'DXT1'`), DXT5 (`'DXT5'`); cube maps X8R8G8B8; render targets X8R8G8B8; depth D16 (80) surfaces plus the D24S8 auto depth.
- **Largest texture**: 1024x1024.
- **Vertex buffers**: usage `WRITEONLY` and `WRITEONLY|DYNAMIC`; index buffers `D3DFMT_INDEX16`.
- **Vertex input**: 79 vertex declarations, and `SetFVF(0x142)` = XYZ|DIFFUSE|TEX1.
- **Primitive types**: 6 = `D3DPT_TRIANGLEFAN` (all draws so far are `DrawPrimitiveUP`).
- **Shaders**: 34 vertex + 34 pixel shaders created (from the D3DX `.fx` effects in the exe's RT_RCDATA resources, compiled at runtime by the statically linked D3DX).
