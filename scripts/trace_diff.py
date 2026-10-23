#!/usr/bin/env python3
"""trace_diff.py - Deterministic Replay & Trace Diff (Lockstep Diff) para RE5.

Compara arquivos de rastreio binário gerados pelo proxy do jogo original
(trace_orig.bin) e pelo runtime recompilado (trace_recomp.bin) gravados na entrada
e na saída de 0x00B72FA0.

Uso:
    python scripts/trace_diff.py [trace_orig.bin] [trace_recomp.bin] [opções]

Opções:
    --char ID       Filtra por personagem: 0 = Chris (padrão), 1 = Sheva, all = ambos
    --tol EPSILON   Tolerância para floats (padrão: 1e-4)
    --verbose, -v   Exibe todos os pacotes comparados
    --limit N       Compara no máximo N quadros
"""

import argparse
import math
import os
import struct
import sys

RE5_TRACE_MAGIC = 0x52453554  # "RE5T"
PACKET_SIZE = 128

# Formato do pacote (88 bytes úteis + 40 bytes padding = 128 bytes):
# uint32_t magic, frameIndex, hookSite, characterId;
# float deltaTime, inputStickX, inputStickY;
# float pos[3], oldPos[3], moveVec[3];
# uint32_t collisionFlags, filterMask;
# float stepHeight, slopeCos, wallCos;
# uint32_t playerStructCRC32;
# uint8_t reserved[40];
PACKET_FMT = "<IIIIfff3f3f3fIIfffI40s"


class Packet:
    def __init__(self, raw):
        if len(raw) != PACKET_SIZE:
            raise ValueError(f"Tamanho inválido de pacote: {len(raw)} (esperado {PACKET_SIZE})")

        unpacked = struct.unpack(PACKET_FMT, raw)
        self.magic = unpacked[0]
        self.frameIndex = unpacked[1]
        self.hookSite = unpacked[2]  # 1 = Pre, 2 = Post
        self.characterId = unpacked[3]
        self.deltaTime = unpacked[4]
        self.inputStickX = unpacked[5]
        self.inputStickY = unpacked[6]
        self.pos = unpacked[7:10]
        self.oldPos = unpacked[10:13]
        self.moveVec = unpacked[13:16]
        self.collisionFlags = unpacked[16]
        self.filterMask = unpacked[17]
        self.stepHeight = unpacked[18]
        self.slopeCos = unpacked[19]
        self.wallCos = unpacked[20]
        self.playerStructCRC32 = unpacked[21]

        if self.magic != RE5_TRACE_MAGIC:
            raise ValueError(f"Magic inválido: 0x{self.magic:08X} (esperado 'RE5T' = 0x52453554)")

    def hook_str(self):
        return "Pre-0x00B72FA0" if self.hookSite == 1 else "Post-0x00B72FA0"

    def char_str(self):
        return "Chris" if self.characterId == 0 else "Sheva"


def load_packets(path, target_char=None):
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Arquivo não encontrado: {path}")

    packets = []
    with open(path, "rb") as f:
        idx = 0
        while True:
            chunk = f.read(PACKET_SIZE)
            if not chunk:
                break
            if len(chunk) < PACKET_SIZE:
                print(f"[Aviso] Fim de arquivo truncado em {path} no byte {idx * PACKET_SIZE + len(chunk)}")
                break
            pkt = Packet(chunk)
            if target_char is None or target_char == "all" or pkt.characterId == int(target_char):
                packets.append(pkt)
            idx += 1
    return packets


def diff_floats(a, b, tol=1e-4):
    return abs(a - b) > tol and not (math.isnan(a) and math.isnan(b))


def compare_packets(pkt_orig, pkt_recomp, tol=1e-4):
    diffs = []

    def check_float(name, val_o, val_r):
        if diff_floats(val_o, val_r, tol):
            diffs.append((name, f"{val_o:.6f}", f"{val_r:.6f}", f"{val_r - val_o:+.6f}"))

    def check_u32(name, val_o, val_r):
        if val_o != val_r:
            diffs.append((name, f"0x{val_o:08X}", f"0x{val_r:08X}", f"diff"))

    check_float("deltaTime", pkt_orig.deltaTime, pkt_recomp.deltaTime)
    check_float("inputStickX", pkt_orig.inputStickX, pkt_recomp.inputStickX)
    check_float("inputStickY", pkt_orig.inputStickY, pkt_recomp.inputStickY)

    for i, axis in enumerate(["X", "Y", "Z"]):
        check_float(f"pos[{axis}]", pkt_orig.pos[i], pkt_recomp.pos[i])
        check_float(f"oldPos[{axis}]", pkt_orig.oldPos[i], pkt_recomp.oldPos[i])
        check_float(f"moveVec[{axis}]", pkt_orig.moveVec[i], pkt_recomp.moveVec[i])

    check_u32("collisionFlags", pkt_orig.collisionFlags, pkt_recomp.collisionFlags)
    check_u32("filterMask", pkt_orig.filterMask, pkt_recomp.filterMask)
    check_float("stepHeight", pkt_orig.stepHeight, pkt_recomp.stepHeight)
    check_float("slopeCos", pkt_orig.slopeCos, pkt_recomp.slopeCos)
    check_float("wallCos", pkt_orig.wallCos, pkt_recomp.wallCos)
    check_u32("playerCRC32", pkt_orig.playerStructCRC32, pkt_recomp.playerStructCRC32)

    return diffs


def classify_scenario(frame, hook_site, diff_names):
    """Classifica a causa da divergência conforme especificação do projeto."""
    has_flags = any(x in diff_names for x in ["collisionFlags", "filterMask", "stepHeight", "slopeCos", "wallCos"])
    has_pos_or_move = any(x.startswith("pos") or x.startswith("moveVec") or x.startswith("oldPos") for x in diff_names)
    has_dt_or_input = any(x in diff_names for x in ["deltaTime", "inputStickX", "inputStickY"])

    if frame == 0 and (has_flags or "playerCRC32" in diff_names):
        return (
            "Cenário A — Divergência no Frame 0 (Inicialização / Propriedades)",
            "Campos de colisão ou propriedades da struct do jogador nasceram diferentes antes de andar.\n"
            "Causa provável: Ordem de construtores da MT Framework ou valores lidos dos arquivos .arc/lfs."
        )

    if hook_site == 1:  # Pre-0x00B72FA0
        return (
            "Cenário B — Divergência no Pre-0x00B72FA0 (Lógica / Root Motion / DeltaTime)",
            "O deslocamento pedido (moveVec), oldPos ou input que chega à função 0x00B72FA0 já diverge antes da colisão estática rodar.\n"
            "Causa provável: Extração de Root Motion da animação (LMT), cálculo de DeltaTime em cUnit, ou normalização do input analógico."
        )

    # Post-0x00B72FA0 com Pre idêntico
    return (
        "Cenário C — Pre-0x00B72FA0 idêntico, mas Post-0x00B72FA0 divergiu (Detector SBC1 / Havok)",
        "A função de movimento 0x00B72FA0 recebeu os mesmos valores exatos, mas retornou resultados diferentes.\n"
        "Causa provável: Alguma variável global interna do detector de colisão estática MT (sCollision, malha SBC1 ou matriz de mundo) está divergente."
    )


def print_table(diffs, pkt_o, pkt_r):
    print("=" * 78)
    print(f" Frame {pkt_o.frameIndex} | {pkt_o.hook_str()} | Personagem: {pkt_o.char_str()}")
    print("=" * 78)
    print(f" {'Campo':<18} | {'Original':<18} | {'Recompilado':<18} | {'Diferença':<14}")
    print("-" * 78)
    for name, vo, vr, delta in diffs:
        print(f" {name:<18} | {vo:<18} | {vr:<18} | {delta:<14}")
    print("-" * 78)


def find_files():
    orig_candidates = [
        "trace_orig.bin",
        "work/trace_orig.bin",
        "tools/re5_logger/trace_orig.bin",
    ]
    recomp_candidates = [
        "work/trace_recomp.bin",
        "trace_recomp.bin",
    ]
    orig = next((p for p in orig_candidates if os.path.isfile(p)), None)
    recomp = next((p for p in recomp_candidates if os.path.isfile(p)), None)
    return orig, recomp


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("orig", nargs="?", help="Caminho para trace_orig.bin (original Steam)")
    ap.add_argument("recomp", nargs="?", help="Caminho para trace_recomp.bin (recompilado)")
    ap.add_argument("--char", default="0", help="Personagem: 0 = Chris, 1 = Sheva, all = ambos (padrão: 0)")
    ap.add_argument("--tol", type=float, default=1e-4, help="Tolerância para floats (padrão: 1e-4)")
    ap.add_argument("--verbose", "-v", action="store_true", help="Exibe todos os pacotes analisados")
    ap.add_argument("--limit", type=int, default=0, help="Limite máximo de quadros a analisar")
    args = ap.parse_args()

    orig_path = args.orig
    recomp_path = args.recomp

    if not orig_path or not recomp_path:
        fo, fr = find_files()
        orig_path = orig_path or fo
        recomp_path = recomp_path or fr

    if not orig_path or not recomp_path:
        print("[ERRO] Arquivos de trace não encontrados!")
        print("Forneça os caminhos explicitamente:")
        print("    python scripts/trace_diff.py trace_orig.bin work/trace_recomp.bin")
        sys.exit(1)

    print("======================================================================")
    print(" Resident Evil 5 - Deterministic Replay & Trace Diff (TAS Lockstep)")
    print("======================================================================")
    print(f" Original:    {orig_path}")
    print(f" Recompilado: {recomp_path}")
    print(f" Filtro:      Personagem {args.char} ({'Chris' if args.char == '0' else ('Sheva' if args.char == '1' else 'Todos')})")
    print(f" Tolerância:  {args.tol}")
    print("======================================================================\n")

    pkts_orig = load_packets(orig_path, args.char)
    pkts_recomp = load_packets(recomp_path, args.char)

    print(f"Pacotes carregados: Original = {len(pkts_orig)} | Recompilado = {len(pkts_recomp)}")

    n_compare = min(len(pkts_orig), len(pkts_recomp))
    if args.limit > 0:
        n_compare = min(n_compare, args.limit * 2)

    if n_compare == 0:
        print("[AVISO] Nenhum pacote compatível encontrado para comparação.")
        sys.exit(1)

    divergence_found = False

    for i in range(n_compare):
        po = pkts_orig[i]
        pr = pkts_recomp[i]

        diffs = compare_packets(po, pr, args.tol)

        if args.verbose:
            print(f"Frame {po.frameIndex:03d} [{po.hook_str()}]: pos=({po.pos[0]:.2f}, {po.pos[1]:.2f}, {po.pos[2]:.2f}) move=({po.moveVec[0]:.3f}, {po.moveVec[1]:.3f}, {po.moveVec[2]:.3f}) diffs={len(diffs)}")

        if diffs:
            divergence_found = True
            print("\n" + "!" * 78)
            print(f" DIVERGÊNCIA ENCONTRADA NO FRAME {po.frameIndex} ({po.hook_str()})!")
            print("!" * 78 + "\n")

            print_table(diffs, po, pr)

            diff_names = [d[0] for d in diffs]
            scenario_title, scenario_desc = classify_scenario(po.frameIndex, po.hookSite, diff_names)

            print(f"\n[DIAGNÓSTICO AUTOMÁTICO]: {scenario_title}")
            print(f"{scenario_desc}\n")
            break

    if not divergence_found:
        if len(pkts_orig) != len(pkts_recomp):
            print(f"\n[AVISO] Os primeiros {n_compare} pacotes são 100% IDÊNTICOS, mas o tamanho total difere:")
            print(f"        Original: {len(pkts_orig)} pacotes | Recompilado: {len(pkts_recomp)} pacotes")
        else:
            print("\n" + "=" * 78)
            print(f" [SUCESSO TOTAL] Todos os {n_compare} pacotes (quadros 0..{pkts_orig[-1].frameIndex}) são 100% IDÊNTICOS!")
            print(" Determinismo e física em lockstep perfeito com o jogo original!")
            print("=" * 78)


if __name__ == "__main__":
    main()
