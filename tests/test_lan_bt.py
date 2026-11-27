"""
test_lan_bt.py - co-op LAN pelo enlace ponto a ponto (runtime/src/win32/bt_link.c), sobre TCP de loopback.

O enlace Bluetooth real (RFCOMM) usa os mesmos quadros e a mesma integracao com o steam_lan; aqui o
transporte e TCP ("tcp-host:PORTA" / "tcp:IP:PORTA") para rodar em qualquer maquina. Sem broadcast
(RE5_LAN_RELAY_ONLY=1): a sala so e achada e os pacotes P2P so chegam pelo enlace.
Uso: test_lan_bt.py <test_steam_lan>
"""
import os
import subprocess
import sys
import time

PORT = '27071'


def env(name, port, sid, bt, extra=None):
    e = os.environ.copy()
    e.update({'RE5_PLAYER_NAME': name, 'RE5_LAN_PORT': port, 'RE5_STEAM_ID': sid, 'RE5_LAN_BT': bt,
              'RE5_LAN_RELAY_ONLY': '1', 'RE5_LAN_DISCOVERY_PORT': '27049' if sid == '101' else '27050'})
    e.pop('RE5_LAN_SERVER', None)
    if extra:
        e.update(extra)
    return e


def main():
    exe = sys.argv[1]
    if not os.path.isfile(exe):
        print(f'nao encontrado: {exe}')
        return 1
    host = subprocess.Popen([exe, '--host'], env=env('Chris', '27080', '101', 'tcp-host:' + PORT,
                            {'RE5_LAN_AUTO_ACCEPT': '1', 'RE5_LAN_TIMEOUT_MS': '1500'}),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.5)
    client = subprocess.Popen([exe, '--client'], env=env('Sheva', '27081', '102', 'tcp:127.0.0.1:' + PORT),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    oc, ec = client.communicate(timeout=40)
    oh, eh = host.communicate(timeout=40)
    for tag, o, e in (('HOST', oh, eh), ('CLIENT', oc, ec)):
        print(f'--- {tag} STDOUT ---\n' + o.decode('latin1', 'ignore'))
        print(f'--- {tag} STDERR ---\n' + e.decode('latin1', 'ignore'))
    if host.returncode != 0 or client.returncode != 0:
        print(f'falhou: host={host.returncode} cliente={client.returncode}')
        return 1
    print('SUCESSO: sala achada e pacotes P2P trocados so pelo enlace')
    return 0


if __name__ == '__main__':
    sys.exit(main())
