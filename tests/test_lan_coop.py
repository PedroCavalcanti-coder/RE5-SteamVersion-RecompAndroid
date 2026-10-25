"""
test_lan_coop.py - Teste de ponta a ponta do modo cooperativo local via rede UDP LAN.
Instancia dois processos do teste:
  - Processo 1: Host 'Chris' na porta UDP 27020
  - Processo 2: Cliente 'Sheva' na porta UDP 27021
Valida descoberta de lobby via UDP broadcast, conexão, entrada e troca P2P no canal 0x389.
"""
import os
import subprocess
import sys
import time

def main():
    exe_path = sys.argv[1] if len(sys.argv) > 1 else 'build/test-build/tests/test_steam_lan.exe'
    if not os.path.isfile(exe_path):
        print(f"Executável não encontrado: {exe_path}")
        return 1

    env_host = os.environ.copy()
    env_host['RE5_PLAYER_NAME'] = 'Chris'
    env_host['RE5_LAN_PORT'] = '27020'
    env_host['RE5_STEAM_ID'] = '101'
    env_host['RE5_LAN_DISCOVERY_PORT'] = '27015'
    # A cliente some sem sair da sala; o host tem que notar em 1,5 s (padrão 15 s).
    env_host['RE5_LAN_TIMEOUT_MS'] = '1500'

    env_client = os.environ.copy()
    env_client['RE5_PLAYER_NAME'] = 'Sheva'
    env_client['RE5_LAN_PORT'] = '27021'
    env_client['RE5_STEAM_ID'] = '102'
    env_client['RE5_LAN_DISCOVERY_PORT'] = '27015'

    print("Iniciando Host 'Chris'...")
    p_host = subprocess.Popen([exe_path, '--host'], env=env_host, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.3)

    print("Iniciando Cliente 'Sheva'...")
    p_client = subprocess.Popen([exe_path, '--client'], env=env_client, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    out_client, err_client = p_client.communicate(timeout=20)
    out_host, err_host = p_host.communicate(timeout=20)

    print("--- HOST STDOUT ---")
    print(out_host.decode('latin1', errors='ignore'))
    print("--- HOST STDERR ---")
    print(err_host.decode('latin1', errors='ignore'))

    print("--- CLIENT STDOUT ---")
    print(out_client.decode('latin1', errors='ignore'))
    print("--- CLIENT STDERR ---")
    print(err_client.decode('latin1', errors='ignore'))

    if p_host.returncode != 0:
        print(f"Host falhou com codigo {p_host.returncode}")
        return 1
    if p_client.returncode != 0:
        print(f"Cliente falhou com codigo {p_client.returncode}")
        return 1

    print("SUCESSO: Host e Cliente trocaram mensagens com sucesso em rede LAN local!")
    return 0

if __name__ == '__main__':
    sys.exit(main())
