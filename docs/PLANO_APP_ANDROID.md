# Plano do app Android (Fases 5 e 6)

Detalha as etapas 5.6 (app) e 6.1 (controles touch) do `PLANO_DO_PROJETO.md`.

## 1. O que o APK pode e não pode levar

Isto não é parecer jurídico; antes de publicar qualquer coisa, vale consultar um
advogado de propriedade intelectual. O desenho abaixo segue a regra que o projeto já
segue no git (D7): **nada derivado do jogo sai daqui**.

| Pode ir no APK | Não pode ir no APK |
|---|---|
| Runtime (`runtime/`), HLE Win32, ponte D3D9, launcher, controles touch | Arquivos do jogo (exe, `nativePC_MT`, vídeos, saves) |
| DXVK, SDL2, FAudio, FFmpeg (LGPL, ligado dinamicamente), ImGui, libadrenotools, libarchive, com as licenças | `generated/src` e qualquer `.so` compilado a partir dele: é tradução do código da Capcom (obra derivada) |
| Nome e ícone próprios, sem logotipos/arte da Capcom | Ferramenta que remove o DRM da Steam (SteamStub): distribuir isso é o ponto mais arriscado (Lei 9.610/98 art. 107; DMCA §1201) |

Consequência: **o código do jogo traduzido tem de ser produzido na máquina do usuário,
a partir da cópia dele**. Duas formas:

- **A (primeira versão): pacote feito no PC do usuário.** Um script do repositório
  (`scripts/make_android_pack.py`, a fazer) roda o pipeline com o exe do próprio usuário,
  compila `libre5_game.so` para `arm64-v8a` com o NDK e gera um `.zip` com essa `.so`, os
  dados do jogo e um `manifest.json`. É esse zip que o app importa. Nada disso é
  distribuído por nós; o usuário faz uma vez.
- **B (pesquisa, depois): tradução no próprio celular.** O app leva o lifter e um
  compilador (Clang do NDK como `.so`, ou TinyCC, que compila em segundos mas gera código
  ~2x mais lento) e traduz na primeira execução. Sem PC, mas o APK fica grande e a
  primeira abertura leva de dezenas de minutos a horas (o build no PC de 16 threads leva
  ~15 min).

O DRM: o pipeline precisa do exe sem a camada da Steam, e hoje isso sai do
`tools/re5_logger` no PC do usuário. Isso fica fora do APK e fora do pacote (só a `.so`
traduzida e os dados entram), e a documentação diz o que é preciso sem distribuir a
ferramenta pronta para outros jogos.

Distribuição: Google Play recusaria (marca de terceiros, conteúdo de terceiros); o
caminho é APK assinado nos Releases do GitHub, com o código-fonte do runtime aberto e o
aviso "requer cópia legítima do jogo na Steam".

## 2. Fluxo do app

```
abrir app
 ├─ install.json não existe ──> Tela "Importar jogo"
 │      botão "Escolher arquivo" (SAF: ACTION_OPEN_DOCUMENT, .zip / .rar / .7z)
 │      checa espaço livre (tamanho descompactado do manifest + 10%)
 │      extrai em streaming (libarchive) para getExternalFilesDir()/game
 │        - barra de progresso por arquivo e por bytes; pode cancelar e retomar
 │        - sem permissão de armazenamento (pasta do próprio app)
 │      valida: SHA-256 do exe contra config/known_builds.json; lista do manifest
 │        (nome, tamanho, CRC) e libre5_game.so com a ABI e a versão do runtime certas
 │      grava install.json (versões, tamanhos, data) ──> Iniciar
 └─ install.json existe ──> verificação rápida (segundos)
        arquivos-chave existem, tamanhos batem, .so compatível com o runtime do APK
        falhou? ──> oferece "Verificar tudo" (CRC completo) ou "Reimportar"
        ok ──> Iniciar
```

- O jogo roda numa Activity própria (SDL) em processo separado (`:game`): sair do jogo
  volta ao launcher limpo, e um crash não derruba o launcher (que mostra o fim do log).
- Configurações passam ao runtime por variáveis de ambiente (as mesmas do `.bat`:
  `RE5_CPUS`, `RE5_DXVK`, resolução...) e pelo `config.ini` do jogo.
- Telas do launcher (Kotlin + Compose): Iniciar, Configurações, Controles, Arquivos
  (verificar, reimportar, apagar), Sobre (licenças, aviso legal).

## 3. Configurações

| Grupo | Itens |
|---|---|
| Vídeo | escala de resolução (50-100%), limite de FPS (30/60), driver Vulkan (sistema ou Turnip via libadrenotools), HUD de FPS |
| Desempenho | núcleos para o jogo (`RE5_CPUS`; padrão: núcleos grandes menos um), `JobThread` |
| Controles | ver seção 4 |
| Áudio | volume, latência (AAudio) |
| Arquivos | verificar, reimportar, espaço usado |

## 4. Controles na tela (estilo jogo de celular)

Nada de "controle de Xbox desenhado": botões grandes, contextuais, e câmera por arrasto.

Layout padrão (paisagem):

- **Esquerda:** joystick flutuante: aparece onde o dedo toca na metade esquerda.
  Empurrar até a borda = correr (ou botão Correr separado, opção).
- **Direita, área livre:** arrastar = girar câmera; durante a mira, arrastar = mirar
  (sensibilidade própria; giroscópio opcional para ajuste fino).
- **Botões:** Mirar (segurar ou alternar), Atirar (só aparece mirando), Recarregar,
  Faca, Ação/Interagir (contextual: só aparece quando o jogo mostra o prompt),
  Inventário, Parceira (toque = chamar, segurar = roda de comandos), Giro 180°, Pausa.

Editor (Configurações > Controles > Editar layout), por botão:
ativar/desativar, arrastar para mover, pinça ou controle deslizante para o tamanho,
opacidade, e modo (segurar/alternar). Também: sensibilidade da câmera e da mira (X e Y
separados), inverter Y, vibração, modo canhoto (espelha), restaurar padrão, perfis
salvos. Tudo num `controls.json` na pasta do app.

Como chega ao jogo: o overlay traduz toques em entrada de **teclado e mouse** no HLE
(`dinput8`/`user32`): a câmera e a mira do RE5 de PC com mouse são contínuas e
precisas, melhores para arrasto que o analógico emulado. Movimento usa as teclas de
direção com intensidade pelo raio do joystick (ou XInput virtual se o andar analógico
fizer falta). Gamepad físico continua funcionando e esconde o overlay.

Desenho: ImGui com o backend D3D9, desenhado no `Present` da ponte D3D9 (passa pela
DXVK como o resto do quadro); o editor de layout é o mesmo overlay em modo edição.

## 5. Etapas

| # | Etapa | Pronto quando |
|---|---|---|
| A1 | `make_android_pack.py` + preset `android-arm64` gerando `libre5_game.so` | zip de pacote gerado no PC, validado por script |
| A2 | Runtime como `libre5_runtime.so` que carrega `libre5_game.so` (`dlopen`), checando versão/ABI | binário de teste carrega no aparelho via adb |
| A3 | Launcher Compose: importar (SAF + libarchive), validar, `install.json`, verificação rápida | importa e valida um pacote real |
| A4 | Activity do jogo (SDL, processo `:game`), DXVK Android, AAudio | chega ao menu (M13) |
| A5 | Overlay touch + editor + `controls.json` | joga o capítulo 1-1 só com toque |
| A6 | Configurações, Turnip, desempenho (lock relaxado no ARM64, etapa 6.2) | 30 FPS estáveis (M14) |
| A7 | Sobre/licenças, assinatura, Release no GitHub | APK publicado sem nenhum arquivo do jogo |
