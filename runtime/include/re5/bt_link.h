/*
 * bt_link.h - enlace ponto a ponto entre dois aparelhos para o multiplayer LAN (Bluetooth RFCOMM).
 *
 * O jogo e de dois jogadores: um enlace basta. Cada quadro e um datagrama (um pacote do steam_lan) com
 * 2 bytes de tamanho na frente. O steam_lan trata o enlace como mais um caminho, como o servidor de
 * retransmissao: busca de sala, entrada, P2P confiavel e ping funcionam sem mudanca.
 *
 * Configuracao (RE5_LAN_BT no re5_env.txt, no ambiente, ou "bluetooth=" no re5_lan.ini):
 *   host              este aparelho espera o outro (RFCOMM, UUID do RE5);
 *   <nome ou MAC>     conecta ao aparelho PAREADO com esse nome (parte do nome serve) ou endereco AA:BB:...;
 *   tcp-host:PORTA / tcp:IP:PORTA   o mesmo enlace sobre TCP (testes e redes sem Bluetooth).
 * Os aparelhos precisam estar pareados nas configuracoes do sistema.
 *
 * Backends: Windows (Winsock AF_BTH), Android (GameActivity.btOpen/btRead/btWrite/btClose via JNI), TCP.
 */
#ifndef RE5_BT_LINK_H
#define RE5_BT_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE5_BT_MAX_FRAME 1500

/* Inicia a thread do enlace (conecta/aceita e reconecta sozinha). 0 = ok. */
int  re5_bt_start(const char *cfg);
void re5_bt_stop(void);
/* 1 com o outro aparelho ligado. */
int  re5_bt_connected(void);
/* Enfileira um quadro; 0 se desligado ou fila cheia (o canal confiavel reenvia). Nao bloqueia. */
int  re5_bt_send(const void *data, size_t len);
/* Proximo quadro recebido (sem esperar): tamanho, ou 0 se nao ha. */
size_t re5_bt_recv(void *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RE5_BT_LINK_H */
