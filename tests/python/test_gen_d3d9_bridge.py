"""scripts/gen_d3d9_bridge.py: ordem da vtable lida do d3d9.h, bytes de pilha
stdcall, o que vira repassador automático e o que fica para a mão (D8)."""
import os
import sys
import unittest

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

import gen_d3d9_bridge as g  # noqa: E402

HEADER = os.path.join(REPO, "third_party", "dxvk-native", "include", "native", "directx", "d3d9.h")


class GenD3D9Bridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(HEADER, encoding="utf-8") as f:
            cls.text = f.read()
        cls.ifaces = g.parse_interfaces(cls.text)
        cls.src, cls.total, cls.auto = g.generate(cls.text, "d3d9.h")

    def slot(self, iface, name):
        return [m["name"] for m in self.ifaces[iface]].index(name)

    def test_vtable_order_is_the_abi(self):
        self.assertEqual(len(self.ifaces["IDirect3DDevice9"]), 119)
        self.assertEqual(len(self.ifaces["IDirect3D9"]), 17)
        self.assertEqual(self.slot("IDirect3DDevice9", "Present"), 17)
        self.assertEqual(self.slot("IDirect3DDevice9", "Clear"), 43)
        self.assertEqual(self.slot("IDirect3DDevice9", "SetRenderState"), 57)
        self.assertEqual(self.slot("IDirect3DDevice9", "CreateQuery"), 118)
        self.assertEqual(self.slot("IDirect3D9", "CreateDevice"), 16)
        self.assertEqual(self.slot("IDirect3DTexture9", "LockRect"), 19)
        self.assertEqual(self.slot("IDirect3DSurface9", "LockRect"), 13)
        self.assertEqual(self.slot("IDirect3DVertexBuffer9", "Lock"), 11)
        # Interfaces derivadas repetem os métodos herdados no próprio bloco.
        self.assertEqual(self.slot("IDirect3DTexture9", "GetType"), 10)

    def test_stdcall_bytes_count_this(self):
        self.assertIn('{"IDirect3D9::CreateDevice", 28, NULL}, /* 16 */', self.src)
        self.assertIn('{"IDirect3DDevice9::Clear", 28, d9a_IDirect3DDevice9_Clear}, /* 43 */', self.src)
        self.assertIn('{"IDirect3DDevice9::BeginScene", 4, d9a_IDirect3DDevice9_BeginScene}, /* 41 */', self.src)
        # Parâmetro sem nome no cabeçalho (GetDeclaration(D3DVERTEXELEMENT9*, UINT*)).
        self.assertIn('{"IDirect3DVertexDeclaration9::GetDeclaration", 12,', self.src)

    def test_hand_written_or_abort(self):
        for name in ("IDirect3D9::CreateDevice", "IDirect3DDevice9::Present", "IDirect3DDevice9::Reset",
                     "IDirect3DDevice9::GetNPatchMode", "IDirect3DDevice9::GetCreationParameters",
                     "IDirect3DTexture9::LockRect", "IDirect3DTexture9::UnlockRect",
                     "IDirect3DVertexBuffer9::Unlock", "IDirect3DSurface9::GetDC", "IDirect3D9::GetAdapterMonitor",
                     "IDirect3D9::RegisterSoftwareDevice", "IDirect3DSurface9::SetPrivateData"):
            self.assertRegex(self.src, r'\{"' + name.replace(":", r"\:") + r'", \d+, NULL\}', name)

    def test_forwarders(self):
        clear = self.src[self.src.index("static void d9a_IDirect3DDevice9_Clear"):]
        clear = clear[:clear.index("}\n")]
        self.assertIn("d9_f32(re5_arg(c, 5))", clear)               # float Z
        self.assertIn("(const D3DRECT*)d9_ptr(re5_arg(c, 2))", clear)
        tex = self.src[self.src.index("static void d9a_IDirect3DDevice9_CreateTexture"):]
        tex = tex[:tex.index("}\n")]
        self.assertIn("d9_out(re5_arg(c, 7), SUCCEEDED(hr), o7, D9K_TEXTURE)", tex)
        self.assertIn('d9_no_shared(re5_arg(c, 8), "IDirect3DDevice9::CreateTexture")', tex)
        st = self.src[self.src.index("static void d9a_IDirect3DDevice9_SetTexture"):]
        self.assertIn("d9_in(re5_arg(c, 2), D9K_BASETEXTURE", st[:st.index("}\n")])

    def test_iids(self):
        iids = g.parse_iids(self.text)
        for iface, _ in g.KINDS + g.ABSTRACT:
            self.assertIn(iface, iids)
        self.assertEqual(iids["IDirect3DDevice9"][0], 0xD0223B96)

    def test_unknown_type_breaks_the_build(self):
        with self.assertRaises(ValueError):
            g.classify("D3DSOMETHINGNEW*")
        self.assertIsNone(g.classify("HWND"))
        self.assertEqual(g.classify("const D3DMATRIX*"), ("ptr", "const D3DMATRIX*"))


if __name__ == "__main__":
    unittest.main()
