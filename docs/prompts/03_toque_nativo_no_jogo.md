# Prompt 3: sistema de toque nativo dentro do jogo

Cole o texto abaixo numa sessão nova de IA aberta na raiz do repositório. Base: `docs/prompts/00_BASE_LEAN_2026-11-23.md`.

---

Você vai substituir os controles na tela do app Android por um **sistema de toque nativo, dentro do próprio jogo**: o jogo (runtime C)
passa a reconhecer os toques e a desenhar os controles, sem a camada Java e **sem emular um controle de Xbox**. Leia primeiro
`docs/prompts/00_BASE_LEAN_2026-11-23.md`, depois `CLAUDE.md`, `docs/STATUS.md`, `runtime/include/re5/touch.h` (o modelo atual),
`docs/AUDITORIA_GHIDRA.md` e `docs/PLANO_APP_ANDROID.md`. Em português.

## Por que

Medido no A54 (360p, mesma parte do benchmark): **jogo sem nenhum Java 35 de pico; com o Java atual 29**. A camada de
controles (`TouchControlsView`, uma `View` do tamanho da tela por cima do `SurfaceView` do jogo) obriga o Android a compor duas
camadas e a redesenhar pela GPU (HWUI) a cada toque, e custa 4–6 fps. Controles desenhados **pelo próprio jogo, no mesmo
`Present`**, não criam camada nenhuma: a meta é **o mesmo fps do modo lean (35 de pico)** com os controles visíveis.

## Como é hoje (o que você substitui)

- `android/app/src/main/java/com/re5/recompiled/TouchControlsView.java` (~1.100 linhas): desenha botões e analógicos, trata
  multitoque, e envia o estado por JNI (`SDLActivity.nativeTouchPad`, `nativeTouchPointer`...) para
  `runtime/src/win32/touch.c`. Ali o estado vira um "**controle de toque**" somado ao usuário 0 do **XInput**, e o toque direto
  em menus vira **clique do mouse** (`WM_MOUSEMOVE`, `WM_LBUTTON*`, `GetCursorPos`, botão do mouse do DirectInput).
- Lógica de jogo que a `TouchControlsView` já resolve e precisa ser portada: Correr/Mirar/Faca como interruptores (um toque
  liga, outro desliga), a arma que atira mirando ou dá a facada com a faca pronta, recarregar, inventário (abre com o toque
  ligado, fecha tocando de novo), câmera arrastando o lado direito (sensibilidade, inverter Y), esconder os botões quando há
  controle físico e voltar no primeiro toque, "menus por botões", vibração, opacidade, escala, **layout editável**
  (`ControlsEditorActivity`, `ControlsOptions`, preferências `touch_*`).
- O jogo só conhece teclado/mouse e XInput. O RE5 mostra os ícones do controle e navega todos os menus pelo analógico.

## O que "nativo" quer dizer aqui (investigue e decida com evidência)

1. **Entrada**: os toques já chegam ao runtime pelo SDL (`SDLSurface` → `onNativeTouch` → eventos `SDL_FINGER*`), sem Java no meio;
   `runtime/src/win32/lan_menu.c` (menu LAN em imgui) **já trata `SDL_FINGERDOWN/UP/MOTION` com coordenadas normalizadas** e
   é o precedente de código e de desenho a seguir. Trate-os
   em C (`runtime/src/win32/touch.c`/`user32.c`), com `SDL_HINT_TOUCH_MOUSE_EVENTS` conforme a necessidade. Multitoque por dedo,
   zonas e arrasto em C, na thread que já bombeia a fila.
2. **Entrega ao jogo**: em vez de fingir um dispositivo, **escreva o estado direto na estrutura de entrada do jogo** no ponto em
   que ele consulta o controle (descubra no Ghidra/`docs/AUDITORIA_GHIDRA.md`: o gerenciador de entrada do MT Framework, a leitura
   do XInput, o cursor/mouse dos menus), ou mantenha o XInput como camada final se isso for o único caminho sem efeitos colaterais.
   Documente o que encontrou; **decida pela evidência**, não por preferência. Critério: o jogo reage igual, sem atraso extra,
   sem "controle conectado/desconectado" falso, e os ícones de botão corretos nos menus.
3. **Menus por toque**: se o jogo tem teste de acerto em itens de menu (cursor do mouse), converta o toque em posição do cursor
   do jogo; se não tem, mapeie o toque para a navegação do analógico/botões com a zona certa.
4. **Desenho**: pelo `IDirect3DDevice9` no `Present` (como `runtime/src/win32/lan_menu.c` faz com o imgui em `re5_lan_menu_draw`,
   hoje ligado só no PC; ligue no Android), **independente do renderizador** (hoje só a DXVK), com texturas/formas simples, alfa e
   escala da resolução real da tela (não do back buffer de 360p: os botões têm de ficar nítidos). Zero alocação por quadro.
5. **Configuração e editor**: o layout e as opções passam do app para o runtime por arquivo (ex.: `files/game/touch_layout.json`) ou
   variáveis; o **editor de layout** pode ficar no app Java (fora do jogo, na tela do launcher) desde que **o jogo em si não
   tenha nenhuma `View` por cima**. Vibração por JNI direto (1 chamada por toque, sem `View`).

## Restrições

- Em `RE5_LEAN=1` o jogo já não tem Java por cima: o toque nativo precisa funcionar nesse modo (é o ponto). A tela de carregamento,
  o monitor e o editor ficam como estão; não os reconstrua.
- Controle físico continua sendo o dispositivo ativo ao ser usado, e os botões somem; o primeiro toque os traz de volta
  (`re5_input_note`/`re5_input_last` em `touch.h`).
- Teste unitário em `tests/unit/` para a lógica pura (zonas, interruptores, mapeamento, multitoque). A parte gráfica e a entrada real
  só se verificam no celular: entregue um plano de teste curto para o usuário.

## Verificação (com autorização do usuário para usar o A54)

Fps do modo lean contra lean + toque nativo (pico e média, mesmo trecho, 3 rodadas); latência do toque (da ação ao movimento) por
captura de vídeo ou contagem de quadros; multitoque (andar + mirar + atirar); menus (tocar nos itens, inventário, mapa);
alternar controle físico e toque; girar a tela; ir ao segundo plano e voltar.

## Entrega

APK `work/android/app-toque-<n>.apk` com o toque nativo ligado por padrão e `RE5_TOUCH_JAVA=1` para voltar aos controles Java
(só durante a transição), plano de teste, e entrada no `docs/STATUS.md` com a decisão de entrada (o que o jogo lê, onde se escreve)
e os números de fps.
