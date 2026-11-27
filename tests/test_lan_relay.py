"""
test_lan_relay.py - co-op LAN passando SO pelo servidor de retransmissao (tools/re5_server).

Sobe o re5_server, depois um host e um cliente do test_steam_lan com RE5_LAN_RELAY_ONLY=1:
sem broadcast e sem varredura de loopback, a sala so e achada e os pacotes P2P so chegam
se o servidor repassar. Uso: test_lan_relay.py <test_steam_lan> <re5_server>
"""
import os
import subprocess
import sys
import time

PORT = '27047'


def main():
    exe = sys.argv[1]
    srv_exe = sys.argv[2]
    for f in (exe, srv_exe):
        if not os.path.isfile(f):
            print(f"nao encontrado: {f}")
            return 1

    srv = subprocess.Popen([srv_exe, '--port', PORT], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.5)
    try:
        def env(name, port, sid, extra=None):
            e = os.environ.copy()
            e.update({'RE5_PLAYER_NAME': name, 'RE5_LAN_PORT': port, 'RE5_STEAM_ID': sid,
                      'RE5_LAN_SERVER': '127.0.0.1:' + PORT, 'RE5_LAN_RELAY_ONLY': '1',
                      'RE5_LAN_DISCOVERY_PORT': '27049' if sid == '101' else '27050'})
            if extra:
                e.update(extra)
            return e

        host = subprocess.Popen([exe, '--host'], env=env('Chris', '27040' if False else '27060', '101',
                                {'RE5_LAN_AUTO_ACCEPT': '1', 'RE5_LAN_TIMEOUT_MS': '1500'}),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.5)
        client = subprocess.Popen([exe, '--client'], env=env('Sheva', '27061', '102'),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        oc, ec = client.communicate(timeout=30)
        oh, eh = host.communicate(timeout=30)
        for tag, o, e in (('HOST', oh, eh), ('CLIENT', oc, ec)):
            print(f'--- {tag} STDOUT ---\n' + o.decode('latin1', 'ignore'))
            print(f'--- {tag} STDERR ---\n' + e.decode('latin1', 'ignore'))
        if host.returncode != 0 or client.returncode != 0:
            print(f'falhou: host={host.returncode} cliente={client.returncode}')
            return 1
        print('SUCESSO: sala achada e pacotes P2P trocados so pelo servidor')
        return 0
    finally:
        srv.kill()
        out = srv.communicate()[0].decode('latin1', 'ignore')
        print('--- SERVIDOR ---\n' + out)


if __name__ == '__main__':
    sys.exit(main())
