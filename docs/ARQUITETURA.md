# Arquitetura do `re5-recompiled`

Como o código gerado, o runtime e o host se encaixam, e as decisões que sustentam
isso. Cada decisão tem um identificador (D1…) citado no código e no plano. Mudar uma
decisão é mudar este arquivo primeiro.

## Visão em camadas

```
 re5dx9.exe (x86-32, sem relocações, base 0x00400000)
      │  scripts/run_lift.py  (pcrecomp disasm32 + lift32_cpu)
      ▼
 generated/src/re5_NNNN.c ── void L_<VA>(CPU *c) { ... dispatch(c, alvo); ... }
      │  só depende de: cpu.h (gerado), dispatch(), dispatch_jmp(), re5_override()
      ▼
 runtime/ (libre5_runtime)
  ├─ core/guest_memory   espaço de 4 GiB do guest = reserva no host (re5_guest_base)
  ├─ core/image          PE32 mapeado em 0x00400000, IAT ligada a thunks
  ├─ core/dispatch       VA → thunk do host | função gerada | erro com trilha
  ├─ core/thunks         VAs 0xFFFC0000+ atendidos por handlers C do host
  ├─ core/imports        "dll!nome" → handler (HLE) ou thunk que aborta; censo
  ├─ core/thread         CPU/TEB/pilha por thread do guest; lock do guest; PEB
  ├─ core/handle_table   HANDLE de 32 bits ↔ objeto do host
  ├─ core/vfs            "C:\RE5\nativePC\..." → caminho do host, sem caixa
  ├─ win32/              HLE: kernel32, user32, gdi32, advapi32, ...   (Fase 3)
  ├─ gfx/                Direct3D 9 → DXVK Native (Vulkan)             (Fase 4)
  ├─ win32/dsound        DirectSound 8 → mixer próprio no áudio do SDL2 (Fase 4)
  ├─ input/              XInput/DirectInput8 → SDL2                    (Fase 4)
  └─ win32/wmvcore       IWMSyncReader → FFmpeg (asf, vc1, wmapro)     (Fase 4)
      ▼
 host: re5 (Linux x64)  |  libre5_recomp.so + APK (Android ARM64)  |  re5.exe (Windows x64)
```

## Decisões

### D1 — Lifter: `lift32_cpu` (estado da CPU explícito)

O pcrecomp tem dois lifters x86-32. O `lift32` usa registradores globais (uma
máquina implícita); o `lift32_cpu` passa `CPU *c` a toda função — é **reentrante**:
cada thread do guest tem sua CPU. A MT Framework é multithread de verdade (workers,
filas lock-free), então o modelo com estado explícito é o único que permite, mais
tarde, paralelismo real (D5).

Custo aceito: o `lift32_cpu` é o menos testado dos dois (o `difftest` do pcrecomp
cobre o `lift32`) e não traduz `pause`, `cpuid`, `rdtsc`, `cmpxchg8b` (viram
`RECOMP_TODO`). Por isso a Fase 2 começa portando o teste diferencial (2.2) e
fechando essas lacunas (2.1).

### D2 — Memória: 4 GiB do guest numa reserva do host, só hosts de 64 bits

`re5_mem_init()` reserva 4 GiB contíguos sem commit; o endereço do guest `va` vive
em `re5_guest_base + va`. O código gerado só manipula `uint32_t`. Em todo host a
tradução é uma soma — o mesmo binário de código gerado serve Linux, Android e
Windows x64. Hosts de 32 bits ficam de fora (`#error` em `guest_base.h`): no
Android, o projeto mira só `arm64-v8a`.

O `cpu.h` do pcrecomp assume host de 32 bits (`*(uint32_t *)(uintptr_t)a`). Em vez
de um fork manual, `scripts/gen_cpu_header.py` reescreve mecanicamente os 32
acessos para `RE5_G2H(a)`, troca os 6 acessos escalares por tipos de alinhamento 1
(x86 aceita desalinhado; C e ARM64 não garantem) e **falha o build** se o upstream
mudar o padrão. Correções do pcrecomp entram sem merge manual.

Mapa do guest: `0x0–0xFFFF` nunca mapeado (nulo do guest falha no host);
`0x00400000+` imagem; `0x10000000–0xFFFBFFFF` alocações dinâmicas (granularidade
de 64 KiB, como o `VirtualAlloc`); `0xFFFC0000–0xFFFEFFFF` thunks.

### D3 — Toda chamada passa pelo dispatch; o Win32 é um registro de handlers

O `lift32_cpu` traduz todo `call`/`jmp` para outra função em `dispatch(c, va)`.
O loader escreve na IAT do jogo **VAs de thunks**, e o dispatch entrega um VA de
thunk ao handler C registrado para aquele `dll!nome`. Consequências:

- a limpeza de pilha do stdcall vem de **um só lugar** (o registro), nunca do handler;
- **import sem implementação aborta com o nome** na primeira chamada — nunca um stub
  silencioso. `GetProcAddress` é a exceção: devolve `NULL`, porque o jogo usa isso
  para detectar APIs opcionais;
- o censo (`re5 --census`) mede o progresso da Fase 3 a qualquer momento.

**Atualização (2026-10-21): desempenho do dispatch.** A busca VA -> função é uma
tabela hash (endereçamento aberto, montada no `re5_runtime_init`), não mais busca
binária nas ~70 mil entradas. E `call`/`jmp` com alvo constante que já é função
gerada não passa pelo dispatch: o `run_lift.py` (`lift.direct_calls`) escreve
`RE5_CALL(c, L_<VA>)`, uma chamada C direta que mantém o ponto de preempção a cada
1024 chamadas (contador `ncalls` na CPU, sem TLS). O que continua no dispatch:
chamadas indiretas (IAT, vtables, ponteiros), thunks e alvos ainda sem função
gerada (que abortam com o endereço, D8). Custo aceito: essas chamadas diretas não
entram na trilha de diagnóstico; `direct_calls = false` a traz de volta inteira.

O `run_lift.py` também declara as funções geradas com `CPU *RE5_RESTRICT c`: a CPU
fica no heap do host e a memória do guest nunca a apelida, então o compilador
pode manter os registradores do guest em registradores entre acessos à memória
(sem isso, cada escrita no guest - ponteiro `may_alias` - obrigava a regravar e
reler o struct; código ~5% menor). Em contrapartida, toda cabeça de laço (alvo de
`goto` para trás) recebe `RE5_LOOP_BARRIER()`: uma espera ocupada do jogo numa
flag escrita por outra thread relê a memória a cada volta, como no x86.
`rep stos`/`rep movs` viram `rep_stos`/`rep_movs` do `cpu.h` (memset/memmove do
host, com a semântica de cópia para frente quando o destino cai dentro da origem).

### D4 — Objetos COM do host: vtables de thunks na memória do guest

D3D9, DirectSound, DirectInput8 e WMVCore entregam ponteiros para objetos COM. Os
objetos reais (DXVK, FFmpeg) são do host e têm ponteiros de 64 bits. O guest recebe
um **objeto-sombra na memória do guest** cujo primeiro campo aponta para uma
**vtable de thunks**; cada thunk sabe o objeto do host (pela tabela de handles) e o
método. As vtables são geradas de listas de métodos (como no NFSU2), não escritas à
mão: no D3D9, `scripts/gen_d3d9_bridge.py` lê a ordem do `d3d9.h` e gera os
repassadores dos métodos de tipos planos; o resto é escrito à mão ou aborta com o
nome. Memória que o guest escreve diretamente (`Lock` de buffers/texturas) usa
buffers-sombra na memória do guest, copiados no `Unlock`.

O Direct3D 9 do host é a **DXVK** (Vulkan). No Windows x64 há duas compiladas por
`scripts/build_dxvk.sh <versão>`, em `dxvk/<versão>/d3d9.dll` ao lado do `re5.exe`:
a 3.1.1 (`third_party/dxvk`, Vulkan 1.4 + maintenance5/6 e load_store_op_none) e
a 2.6.2 (`third_party/dxvk-2.6.2`, Vulkan 1.3 com maintenance5 opcional). Antes de
carregar, o runtime sonda o Vulkan e usa a mais nova que a GPU aceita; sem
nenhuma, o `d3d9.dll` do sistema (`RE5_DXVK`/`RE5_D3D9_DLL` escolhem à mão).
Medido: Radeon Vega (Ryzen 7 5700U, driver 31.0.21924.61, Vulkan 1.3.260) só
aceita a 2.6.2. No Android (Mali do Exynos 1380 e outros Vulkan 1.1) a escolha é a
DXVK native 1.9.2b (`third_party/dxvk-native`) com o porte do NFSU2
(`tools/nfsu2-reference/ports/dxvk_android`: recursos opcionais, DXT decodificado
na CPU sem BC), já testado em Mali.

### D5 — Threads 1:1 e lock global do guest (por enquanto)

Cada thread do guest é uma thread do host com CPU, pilha e TEB próprios no espaço
do guest (`fs:` simulado: `c->fs_base` aponta o TEB). Inicialmente **um lock global
serializa a execução de código do guest** (`runtime.guest_lock = "global"`):
elimina de saída a diferença entre o modelo de memória do x86 (TSO) e o do ARM64
(fraco), da qual dependem os spinlocks e filas lock-free da MT Framework.

Regras para isso não travar: handlers bloqueantes soltam o lock
(`RE5_BLOCKING_BEGIN/END`); `pause` e laços longos fazem yield (lacuna do lifter, 2.1).
O lock é uma **fila justa (FIFO)**, não um mutex do sistema, e há **preempção**: a cada
1024 dispatches, a thread que roda código do guest há mais de 2 ms com alguém na fila
vai para o fim dela (nunca dentro de um handler do host). Medido no RE5: com um
`CRITICAL_SECTION`, um laço de espera do jogo em `Enter/LeaveCriticalSection` segurava o
lock por minutos e a thread da janela parava ("não respondendo").
Relaxar o lock é a etapa 6.2, com estratégia de TSO no ARM64 (cargas/armazenamentos
acquire/release em memória compartilhada ou regiões detectadas) e testes de estresse.

**Atualização (2026-10-21): modo `hle`, padrão em host x86-64.** O lock passa a
proteger só os handlers do HLE: `re5_thunk_invoke` o pega, `re5_guest_call` o solta
enquanto roda código do guest (WndProc, `IStream`, início de thread) e o código do
guest roda em paralelo. Isso é seguro no x86-64 porque o hardware já é TSO e o RE5
não tem nenhum atômico em linha (contagem com capstone em todas as funções do
catálogo: zero `lock`, `cmpxchg`, `xadd`, `pause`): a sincronização dele passa toda
por imports do kernel32, que são atômicos de verdade (`Interlocked*` com atômicos do
host) ou serializados pelo lock do HLE. Handlers que só mexem no que é da própria
thread (TLS, `LastError`, relógios) têm `RE5_THUNK_NOLOCK`. O modo `global` (acima)
continua com `RE5_GUEST_LOCK=global` e é o padrão no ARM64 até a estratégia de TSO.
Medido: de 72% das amostras esperando o lock a ~2-3x mais quadros por segundo.
No modo `hle` o lock é uma flag com mutex e variável de condição: quem espera
acorda quando ele é solto (a thread da janela acorda também a cada 10 ms para
tratar as mensagens); antes ela tentava e dormia 1 ms em laço, com o lock livre.
Ficam fora do lock do HLE (`RE5_THUNK_NOLOCK`) os handlers que já se protegem:
seções críticas (estado no `CRITICAL_SECTION` do guest, protegido por 64 mutexes do
host por faixa de endereço, com fila e entrega direta ao primeiro que espera),
eventos/mutexes/semáforos e as esperas (mutex próprio do `kernel32_sync`),
`VirtualFree` (mutex da memória do guest), `Sleep` e os métodos de consulta e
controle dos buffers do DirectSound (`mix_lock`). A cópia do `Unlock` de buffers e
texturas do D3D9 roda com o lock solto. Medido no carregamento: de 58% das amostras
esperando o lock para 2-5%.

### D6 — Plataforma de desenvolvimento: Linux x64 headless

É onde a IA trabalha (container na nuvem). Tudo até o gameplay é validado aqui
primeiro (Vulkan por software para os quadros); Android é o alvo final e reusa o
mesmo runtime; Windows x64 é opcional (útil como oráculo).

### D7 — Código gerado e análises não vão para o git

`generated/src/` (dezenas de milhões de linhas) e `work/` são derivados do binário
da Capcom e determinísticos: regenerados a cada sessão. Só vão para o git os
scripts, a configuração e os **números** (relatórios em `docs/`). Os arquivos do
jogo (`re5dx9.exe`, o `re5dx9.clean.exe` despejado dele, dados) **nunca** vão para o
git, nem por LFS: ficam só no disco de quem tem o jogo, em `bin_input/` (ignorado).
Em 2026-10-22 o histórico foi reescrito para tirá-los de todos os commits.

### D8 — Falhar alto

`RECOMP_TODO` (instrução sem tradução), dispatch sem função gerada e import sem HLE
abortam com endereço/nome e a trilha dos últimos dispatches da thread. Um valor
plausível devolvido em silêncio vira um bug a mil funções de distância.

### D9 — Ponto flutuante como o pcrecomp modela

x87 é uma pilha de `double` (não 80 bits exatos); SSE escalar é `float` de verdade.
A palavra de controle do x87 é estado (`CPU.fpu_cw`): `fistp`/`frndint` arredondam
pelo campo RC, como o idioma `fnstcw; or 0xC00; fldcw; fistp` do MSVC espera (1222
`fistp` truncados e os floor/ceil inline do RE5; ignorá-lo deixava o som do jogo
chiado). O MXCSR é guardado e só FTZ/DAZ vão para o SSE do host (a thread de som
liga FTZ); o RC do SSE é só guardado. É o modelo que levou NFSU2 e outros a jogáveis. Se um bug for
rastreado até precisão, a correção vai para o pcrecomp com teste.

## Pipeline de ponta a ponta (já testado com o PE sintético)

```
scripts/recon.py <exe>            portão: só passa sem SteamStub/Enigma/packer
scripts/run_lift.py catalog       disasm32 → work/catalog.json
scripts/run_lift.py lift          lift32_cpu → generated/src/
cmake --preset linux-debug        gera cpu.h, compila runtime + código gerado
./build/linux-debug/re5           carrega a imagem, liga a IAT, roda o entry point
```

`tests/e2e/` roda exatamente isso num PE32 compilado com clang a cada `ctest`.
