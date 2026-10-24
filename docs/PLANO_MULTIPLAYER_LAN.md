# Plano de Implementação — Modo Multiplayer Local (LAN Co-op)

> **Projeto**: `re5-recompiled`  
> **Branch**: `feat/multiplayer-lan`  
> **Objetivo**: Habilitar modo cooperativo (Campanha e Mercenários) entre duas instâncias do jogo em rede local (LAN / Wi-Fi) sem depender de servidores da Valve/Steam, suportando **PC x PC**, **PC x Android** e **Android x Android**.

---

## 1. Visão Geral e Contexto da Arquitetura

O `re5dx9.exe` original (versão Steamworks, SDK 1.30) foi portado da antiga versão Games for Windows Live (GFWL). Na versão Steam, todo o tráfego de multiplayer (mesmo na opção *"Network Selection: LOCAL"*) é canalizado pelas interfaces de rede do Steamworks:

1. **`ISteamMatchmaking009`**: Gerencia criação, descoberta e entrada em lobbies, além de metadados da partida (capítulo, dificuldade, seleções de Chris e Sheva).
2. **`ISteamNetworking005`**: Gerencia o transporte P2P de pacotes de gameplay (`SendP2PPacket`, `ReadP2PPacket` no canal `0x389` / `905`), sincronizando posições, inputs, inventário, animações e danos.

Atualmente, `runtime/src/win32/steam_api.c` implementa o Steamworks de forma 100% offline (um único jogador fixo com `SteamID = 1`, lobbies desativados e `SendP2PPacket` retornando falso). Para permitir o multiplayer LAN, precisamos implementar um **emulador de rede P2P LAN embutido no runtime**.

```
+-----------------------------------------------------------------------+
|                       Código Liftado do Jogo                          |
|             (0x004130xx Matchmaking / 0x00413Fxx P2P)                 |
+-----------------------------------------------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
|                      HLE Steamworks do Runtime                        |
|        ISteamMatchmaking009          |       ISteamNetworking005      |
|  (Lobbies, Filtros, Beacons)         |  (Canais P2P, Filas, Frentes)  |
+--------------------------------------+--------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
|                    Subcamada LAN Host (steam_lan)                     |
|                                                                       |
|  - Descoberta UDP Broadcast (27015): Beacons de Lobbies               |
|  - Transporte P2P UDP (27016+): Datagramas Confiáveis e Rápido       |
|  - Mapeamento: CSteamID <---> IP:Porta do Host/Par                    |
|  - Suporte Sockets POSIX (Linux/Android) e Winsock (Windows)          |
+-----------------------------------------------------------------------+
```

---

## 2. Etapas de Execução

### Etapa 1: Identidade Dinâmica e Configuração de Instância
- **Problema atual**: O SteamID do jogador é fixo (`STEAM_ID_LO = 1`) e o nome é sempre `"Player"`. Se duas instâncias tiverem o mesmo ID, o jogo entra em conflito de identidade P2P.
- **Implementação**:
  - Leitura de variáveis de ambiente e arquivo de configuração (`config/re5_config.toml`):
    - `RE5_PLAYER_NAME` (padrão: "Player 1" / "Player 2" ou hostname)
    - `RE5_LAN_PORT` (porta base UDP, padrão `27015`)
    - `RE5_STEAM_ID` (SteamID derivado do IP/porta ou gerado aleatoriamente)
  - Funções de consulta e atribuição de identidade no início de `SteamAPI_Init()`.

### Etapa 2: Camada de Transporte de Rede do Host (`steam_lan`)
- **Arquivos**: `runtime/include/re5/steam_lan.h` e `runtime/src/win32/steam_lan.c`.
- **Implementação**:
  - Criação de socket UDP multiplataforma (suporte a `SO_BROADCAST`, `SO_REUSEADDR`, modo não-bloqueante).
  - Tabela interna de pares conhecidos:
    ```c
    typedef struct {
        uint64_t steam_id;
        uint32_t ip;      // IPv4
        uint16_t port;    // Porta de dados
        uint32_t last_seen_ms;
    } lan_peer;
    ```
  - Thread de recepção em segundo plano (ou pooling em `SteamAPI_RunCallbacks`) para processar pacotes UDP recebidos e colocá-los nas filas corretas.

### Etapa 3: Implementação de `ISteamMatchmaking009` (Lobbies LAN)
- **Métodos principais a implementar**:
  - `CreateLobby(eLobbyType, cMaxMembers)`:
    - Cria estrutura local de lobby com ID único (`CSteamID` do tipo Lobby).
    - Registra a instância local como `LobbyOwner`.
    - Inicia envio periódico (ex: a cada 1 segundo) de pacote UDP Broadcast:
      `[MAGIC "RE5LAN"][LOBBY_ID][HOST_STEAM_ID][HOST_NAME][LOBBY_DATA...]`.
    - Dispara callback `LobbyCreated_t` e `LobbyEnter_t`.
  - `RequestLobbyList()`:
    - Escuta pacotes de broadcast na rede local por um curto período (ou usa cache de beacons recentes).
    - Popula a lista de lobbies disponíveis.
    - Dispara callback `LobbyMatchList_t` com o número de salas encontradas.
  - `GetLobbyByIndex(iLobby)`:
    - Retorna o `CSteamID` da sala pelo índice.
  - `GetLobbyData(steamIDLobby, pchKey)` / `SetLobbyData(steamIDLobby, pchKey, pchValue)`:
    - Armazena e sincroniza metadados da sala (nome do capítulo, dificuldade, estado da sessão).
  - `JoinLobby(steamIDLobby)`:
    - Envia requisição unicast `JOIN_REQUEST` para o IP do host da sala.
    - O host valida os slots disponíveis, registra o novo membro e responde com `JOIN_RESPONSE`.
    - O cliente recebe confirmação e dispara `LobbyEnter_t`.
    - Ambos disparam `LobbyChatUpdate_t` para notificar entrada/saída de membros.
  - `LeaveLobby(steamIDLobby)`:
    - Notifica o par da saída e limpa o estado do lobby.

### Etapa 4: Implementação de `ISteamNetworking005` (Transporte P2P de Gameplay)
- **Métodos principais a implementar**:
  - `SendP2PPacket(steamIDRemote, pubData, cubData, eP2PSendType, nChannel)`:
    - Busca o IP e a porta de `steamIDRemote` na tabela de pares LAN.
    - Encapsula o pacote com cabeçalho simples:
      `[PACKET_TYPE][CHANNEL][SEQUENCE_NUM][PAYLOAD]`.
    - Envia via socket UDP.
    - Se `eP2PSendType == k_EP2PSendReliable`, enfileira para retransmissão até receber ACK.
  - `IsP2PPacketAvailable(pcubMsgSize, nChannel)`:
    - Verifica se a fila de recepção do canal `nChannel` (ex: canal `0x389` do RE5) contém mensagens.
    - Preenche `pcubMsgSize` com o tamanho do próximo pacote disponível.
  - `ReadP2PPacket(pubDest, cubDest, pcubMsgSize, psteamIDRemote, nChannel)`:
    - Desenfileira o pacote do canal `nChannel`.
    - Copia os dados para `pubDest` e preenche `psteamIDRemote` com o SteamID do emissor.
  - `AcceptP2PSessionWithUser(steamIDRemote)`:
    - Aceita conexão P2P com o par.
  - `GetP2PSessionState(steamIDRemote, pConnectionState)`:
    - Preenche estado ativo (`m_bConnectionActive = 1`).

### Etapa 5: Validação, Testes e Verificação
1. **Teste Unitário Local (`test_steam_lan`)**:
   - Criação de teste C em `tests/unit/test_steam_lan.c` que instancia dois agentes de rede em loopback (`127.0.0.1`), um como Host e outro como Cliente.
   - Valida:
     - Criação do lobby pelo Host.
     - Descoberta do lobby pelo Cliente (`RequestLobbyList`).
     - Entrada no lobby pelo Cliente (`JoinLobby`).
     - Troca bidirecional de pacotes P2P no canal `0x389`.
     - Desconexão limpa (`LeaveLobby`).
2. **Teste End-to-End no Jogo**:
   - Abrir duas janelas do `re5.exe` na mesma máquina com portas e identidades diferentes:
     - Instância 1: `RE5_PLAYER_NAME="Chris" RE5_LAN_PORT=27015 re5.exe`
     - Instância 2: `RE5_PLAYER_NAME="Sheva" RE5_LAN_PORT=27017 re5.exe`
   - Testar o menu de Co-op / Network Selection: "LOCAL" ou "NO LIMITS" e entrar no Capítulo 1-1.

### Etapa 6: Compatibilidade com Android
- Garantir que as chamadas de socket utilizem a API padrão POSIX compatível com Linux e Bionic (Android NDK).
- Configurar permissões no `AndroidManifest.xml` do projeto Android:
  - `android.permission.INTERNET`
  - `android.permission.ACCESS_NETWORK_STATE`
  - `android.permission.CHANGE_WIFI_MULTICAST_STATE` (necessário no Android para receber pacotes de broadcast/multicast em Wi-Fi).

---

## 3. Cronograma de Entregas e Critérios de Aceite

| Marco | Entregável | Critério de Aceite |
|---|---|---|
| **M-LAN 1** | Identidade e Infraestrutura de Sockets (`steam_lan`) | Duas instâncias geram SteamIDs distintos e sockets UDP operam em Windows/Linux. |
| **M-LAN 2** | `ISteamMatchmaking009` funcional em LAN | Host cria lobby e Cliente descobre a sala via busca no menu do jogo. |
| **M-LAN 3** | Entrada no Lobby e Sincronização de Metadados | Cliente conecta ao lobby, ambos aparecem na tela de seleção de personagens e inventário. |
| **M-LAN 4** | `ISteamNetworking005` P2P e Gameplay Co-op | Partida inicia com Chris e Sheva controlados por instâncias diferentes com sincronia em tempo real. |
| **M-LAN 5** | Testes automatizados e suporte Android | `test_steam_lan` verde no `ctest` e suporte compilado para Android ARM64. |
