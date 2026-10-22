#!/usr/bin/env python3
"""Gera a tabela de métodos da ponte COM do Direct3D 9 (etapa 4.2, decisão D4).

    gen_d3d9_bridge.py <d3d9.h> <saída.inc>

Lê a ordem dos métodos de cada interface do d3d9.h (o do DXVK native, fixo em
third_party/: a ordem da vtable é o ABI, igual em qualquer cabeçalho) e emite C
incluído por runtime/src/win32/d3d9.c:

  - enum d9_kind (um valor por interface que vira objeto no guest);
  - os IIDs de cada interface (QueryInterface);
  - para cada método, uma entrada {nome, bytes de pilha, handler}. Os bytes são
    4 * (argumentos + this): COM é stdcall e todo argumento do D3D9 tem 32 bits
    no x86;
  - um repassador automático (d9a_<Interface>_<Método>) para cada método cujos
    tipos são todos "planos": escalares, float e ponteiros para estruturas sem
    ponteiros nem handles dentro (o layout é o mesmo no x86 e no x64, então o
    ponteiro do guest vira ponteiro do host com re5_g2h). Interfaces de entrada
    viram o objeto do host; interfaces de saída (T**) viram objetos do guest.

Métodos com HWND, HDC, void**, D3DLOCKED_*, D3DPRESENT_PARAMETERS e afins, com
retorno float (x87) ou HMONITOR, ou listados em MANUAL, ficam com handler NULL:
o d3d9.c dá um handler escrito à mão ou o thunk aborta com
"Interface::Método sem implementação" (D8). Tipo desconhecido para o gerador
derruba o build: alguém precisa decidir se ele é plano.
"""
import re
import sys

# Interfaces que viram objetos do guest, na ordem do enum d9_kind.
KINDS = [
    ("IDirect3D9", "D9K_D3D9"),
    ("IDirect3DDevice9", "D9K_DEVICE"),
    ("IDirect3DSwapChain9", "D9K_SWAPCHAIN"),
    ("IDirect3DSurface9", "D9K_SURFACE"),
    ("IDirect3DVolume9", "D9K_VOLUME"),
    ("IDirect3DTexture9", "D9K_TEXTURE"),
    ("IDirect3DCubeTexture9", "D9K_CUBETEXTURE"),
    ("IDirect3DVolumeTexture9", "D9K_VOLUMETEXTURE"),
    ("IDirect3DVertexBuffer9", "D9K_VERTEXBUFFER"),
    ("IDirect3DIndexBuffer9", "D9K_INDEXBUFFER"),
    ("IDirect3DVertexDeclaration9", "D9K_VERTEXDECL"),
    ("IDirect3DVertexShader9", "D9K_VERTEXSHADER"),
    ("IDirect3DPixelShader9", "D9K_PIXELSHADER"),
    ("IDirect3DStateBlock9", "D9K_STATEBLOCK"),
    ("IDirect3DQuery9", "D9K_QUERY"),
]
# Interfaces abstratas: nunca são o tipo de um objeto, mas aparecem como
# parâmetro (aceitam vários tipos) e em QueryInterface.
ABSTRACT = [
    ("IDirect3DBaseTexture9", "D9K_BASETEXTURE"),
    ("IDirect3DResource9", "D9K_RESOURCE"),
]
KIND_OF = dict(KINDS + ABSTRACT)

# Escalares de 32 bits (inteiros e enums): passam como estão.
SCALARS = {
    "UINT", "DWORD", "INT", "int", "WINBOOL", "BOOL", "ULONG", "LONG", "UINT32", "D3DCOLOR",
    "D3DFORMAT", "D3DDEVTYPE", "D3DPOOL", "D3DRESOURCETYPE", "D3DMULTISAMPLE_TYPE",
    "D3DRENDERSTATETYPE", "D3DTRANSFORMSTATETYPE", "D3DSAMPLERSTATETYPE",
    "D3DTEXTURESTAGESTATETYPE", "D3DPRIMITIVETYPE", "D3DBACKBUFFER_TYPE", "D3DCUBEMAP_FACES",
    "D3DTEXTUREFILTERTYPE", "D3DSTATEBLOCKTYPE", "D3DQUERYTYPE",
}
# Estruturas (e dados crus) com o mesmo layout no x86 e no x64: sem ponteiros,
# handles, size_t ou long. Ponteiro para elas = re5_g2h do ponteiro do guest.
FLAT_POINTEES = {
    "void", "DWORD", "UINT", "INT", "int", "float", "WINBOOL",
    "D3DADAPTER_IDENTIFIER9", "D3DCAPS9", "D3DCLIPSTATUS9", "D3DDISPLAYMODE", "D3DGAMMARAMP",
    "D3DINDEXBUFFER_DESC", "D3DVERTEXBUFFER_DESC", "D3DLIGHT9", "D3DMATERIAL9", "D3DMATRIX",
    "D3DRASTER_STATUS", "D3DSURFACE_DESC", "D3DVOLUME_DESC", "D3DVERTEXELEMENT9", "D3DVIEWPORT9",
    "D3DBOX", "D3DRECT", "D3DRECTPATCH_INFO", "D3DTRIPATCH_INFO", "PALETTEENTRY", "RECT", "POINT",
    "RGNDATA",
}
# Tipos que exigem tradução escrita à mão (o método fica sem repassador).
NEEDS_HAND = {
    "HWND", "HDC", "HDC*", "void**", "D3DLOCKED_RECT*", "D3DLOCKED_BOX*",
    "D3DPRESENT_PARAMETERS*", "D3DDEVICE_CREATION_PARAMETERS*", "IDirect3DResource9**",
}
RETURNS = {"HRESULT", "UINT", "DWORD", "ULONG", "WINBOOL", "D3DRESOURCETYPE", "D3DQUERYTYPE",
           "D3DTEXTUREFILTERTYPE", "void"}
RETURNS_HAND = {"float", "HMONITOR"}   # float volta em st(0) no x86; HMONITOR é handle do host
# Métodos planos que mesmo assim precisam de mão:
#   RegisterSoftwareDevice - o void* é um ponteiro de função do guest;
#   SetPrivateData - com D3DSPD_IUNKNOWN o void* é um IUnknown;
#   Unlock* - copiam o buffer-sombra do guest para a memória do host antes.
MANUAL = {"RegisterSoftwareDevice", "SetPrivateData", "Unlock", "UnlockRect", "UnlockBox"}


def parse_interfaces(text):
    out = {}
    for m in re.finditer(r"DECLARE_INTERFACE_\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\{(.*?)\};", text, re.S):
        name, body = m.group(1), m.group(3)
        methods = []
        for r, n1, n2, params in re.findall(
                r"STDMETHOD(?:_\(\s*([^,]+?)\s*,\s*(\w+)\s*\)|\((\w+)\))\s*\((.*?)\)\s*PURE\s*;", body, re.S):
            params = re.sub(r"\s+", " ", params).strip()
            if params == "THIS":
                params = ""
            elif params.startswith("THIS_"):
                params = params[5:].strip()
            else:
                raise ValueError(f"{name}: método sem THIS: {params}")
            args = []
            for i, p in enumerate([x.strip() for x in params.split(",")] if params else []):
                pm = re.match(r"^(.*?[\s*])(\w+)$", p)
                # Sem nome ("D3DVERTEXELEMENT9*", "UINT"): o texto inteiro é o tipo.
                t, pname = (pm.group(1), pm.group(2)) if pm else (p, f"a{i + 1}")
                t = re.sub(r"\s*\*\s*", "*", t.replace("struct ", "")).strip()
                args.append((t, pname))
            methods.append({"ret": (r or "HRESULT").strip(), "name": n1 or n2, "args": args})
        out[name] = methods
    return out


def parse_iids(text):
    iids = {}
    for m in re.finditer(r"DEFINE_GUID\(\s*IID_(\w+)\s*,([^)]*)\)", text):
        vals = [int(v.strip(), 16) for v in m.group(2).split(",")]
        if len(vals) != 11:
            raise ValueError(f"IID_{m.group(1)}: {len(vals)} campos")
        iids[m.group(1)] = vals
    return iids


def classify(t):
    """-> (tipo de parâmetro, detalhe) ou None se precisa de mão."""
    if t in NEEDS_HAND:
        return None
    if t in SCALARS:
        return ("scalar", t)
    if t == "float":
        return ("float", t)
    if t in ("REFGUID", "REFIID"):
        return ("ptr", t)
    if t == "HANDLE*":
        return ("shared", t)
    if t.endswith("**") and t[:-2] in KIND_OF:
        return ("out", t[:-2])
    if t.endswith("*") and t[:-1] in KIND_OF:
        return ("in", t[:-1])
    if t.endswith("*"):
        base = t[:-1]
        if base.startswith("const "):
            base = base[6:]
        if base in FLAT_POINTEES:
            return ("ptr", t)
    raise ValueError(f"tipo sem classificação no gerador: {t!r} (plano? decida em scripts/gen_d3d9_bridge.py)")


def method_plan(iface, slot, m):
    """Repassador em C, ou None se o método fica para o d3d9.c (ou aborta)."""
    if slot < 3 or m["name"] in MANUAL or m["ret"] in RETURNS_HAND:
        return None
    if m["ret"] not in RETURNS:
        raise ValueError(f"{iface}::{m['name']}: retorno sem classificação: {m['ret']}")
    kinds = [classify(t) for t, _ in m["args"]]
    if any(k is None for k in kinds):
        return None
    full = f"{iface}::{m['name']}"
    pre, call, post = [], [], []
    for i, ((t, pname), (kind, detail)) in enumerate(zip(m["args"], kinds), start=1):
        a = f"re5_arg(c, {i})"
        if kind == "scalar":
            call.append(f"({detail}){a}")
        elif kind == "float":
            call.append(f"d9_f32({a})")
        elif kind == "ptr":
            call.append(f"({detail})d9_ptr({a})")
        elif kind == "shared":
            call.append(f'd9_no_shared({a}, "{full}")')
        elif kind == "in":
            call.append(f'({detail} *)d9_in({a}, {KIND_OF[detail]}, "{full}")')
        elif kind == "out":
            pre.append(f"    {detail} *o{i} = NULL;")
            call.append(f"{a} ? &o{i} : NULL")
            post.append(f"    d9_out({a}, SUCCEEDED(hr), o{i}, {KIND_OF[detail]});")
    self_kind = KIND_OF[iface]
    lines = [f"static void d9a_{iface}_{m['name']}(CPU *c)", "{",
             f"    {iface} *self = ({iface} *)d9_self(c, {self_kind});"]
    lines += pre
    inv = f"self->lpVtbl->{m['name']}(self{''.join(', ' + x for x in call)})"
    if post:
        lines.append(f"    HRESULT hr = {inv};")
        lines += post
        lines.append("    c->eax = (uint32_t)hr;")
    elif m["ret"] == "void":
        lines.append(f"    {inv};")
    else:
        lines.append(f"    c->eax = (uint32_t){inv};")
    lines.append("}")
    return "\n".join(lines)


def generate(text, header_name):
    ifaces = parse_interfaces(text)
    iids = parse_iids(text)
    out = [f"/* Gerado por scripts/gen_d3d9_bridge.py a partir de {header_name}. Não edite. */", ""]
    out.append("typedef enum {")
    for _, k in KINDS:
        out.append(f"    {k},")
    out.append("    D9K_COUNT,")
    for _, k in ABSTRACT:
        out.append(f"    {k},")
    out.append("} d9_kind;")
    out.append("")
    out.append("typedef struct { const char *name; uint32_t arg_bytes; re5_host_fn fn; } d9_method;")
    out.append("typedef struct { const char *iface; d9_kind kind; const d9_method *methods; uint32_t count;")
    out.append("                 uint32_t iid[11]; } d9_iface;")
    out.append("")
    auto = 0
    total = 0
    tables = []
    for iface, kind in KINDS + ABSTRACT:
        if iface not in ifaces:
            raise ValueError(f"interface {iface} ausente de {header_name}")
        methods = ifaces[iface]
        if [m["name"] for m in methods[:3]] != ["QueryInterface", "AddRef", "Release"]:
            raise ValueError(f"{iface}: não começa por IUnknown")
        if (iface, kind) in ABSTRACT:
            continue
        entries = []
        for slot, m in enumerate(methods):
            fn = method_plan(iface, slot, m)
            nbytes = 4 * (1 + len(m["args"]))
            if fn:
                out.append(fn)
                out.append("")
                auto += 1
            entries.append(f'    {{"{iface}::{m["name"]}", {nbytes}, '
                           f'{"d9a_" + iface + "_" + m["name"] if fn else "NULL"}}}, /* {slot} */')
        total += len(methods)
        tables.append((iface, kind, entries))
    for iface, kind, entries in tables:
        out.append(f"static const d9_method k_d9m_{iface}[] = {{")
        out += entries
        out.append("};")
        out.append("")
    out.append("static const d9_iface k_d9_ifaces[] = {")
    for iface, kind in KINDS + ABSTRACT:
        iid = ", ".join(f"0x{v:X}u" for v in iids[iface])
        if (iface, kind) in ABSTRACT:
            out.append(f'    {{"{iface}", {kind}, NULL, 0, {{{iid}}}}},')
        else:
            out.append(f'    {{"{iface}", {kind}, k_d9m_{iface}, {len(ifaces[iface])}u, {{{iid}}}}},')
    out.append("};")
    out.append(f"#define D9_IFACE_COUNT {len(KINDS) + len(ABSTRACT)}u")
    out.append(f"/* {total} métodos em {len(KINDS)} interfaces; {auto} com repassador automático. */")
    out.append("")
    return "\n".join(out), total, auto


def main():
    if len(sys.argv) != 3:
        print(__doc__.split("\n")[2].strip(), file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    src, total, auto = generate(text, "d3d9.h")
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    print(f"gen_d3d9_bridge: {total} métodos, {auto} repassados automaticamente")
    return 0


if __name__ == "__main__":
    sys.exit(main())
