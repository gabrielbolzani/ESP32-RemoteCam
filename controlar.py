import subprocess
import os
import sys

# Configuração do caminho do PlatformIO (encontrado no seu sistema)
PIO_PATH = r"C:\Users\Gabriel\.platformio\penv\Scripts\pio.exe"

def run_pio_command(command_args, project_dir="."):
    """Executa um comando pio e mantém a saída no terminal."""
    if not os.path.exists(PIO_PATH):
        print(f"\n[ERRO] PlatformIO não encontrado em {PIO_PATH}")
        return

    full_cmd = [PIO_PATH] + command_args
    print(f"\n[INFO] Executando: {' '.join(full_cmd)}\n")
    
    try:
        # Usamos check_call para que a saída apareça em tempo real
        subprocess.check_call(full_cmd, cwd=project_dir, shell=True)
    except subprocess.CalledProcessError as e:
        print(f"\n[ALERTA] O comando falhou com código: {e.returncode}")
    except Exception as e:
        print(f"\n[ERRO] Ocorreu um erro: {e}")

def main():
    while True:
        print("\n" + "="*35)
        print("   PAINEL DE CONTROLE ESP32-CAM   ")
        print("="*35)
        print("1. COMPILAR          (pio run)")
        print("2. UPLOAD            (pio run -t upload)")
        print("3. MONITOR SERIAL    (pio monitor)")
        print("4. LIMPAR BUILD      (pio clean)")
        print("5. SAIR")
        print("-" * 35)
        
        escolha = input("Escolha uma opção [1-5]: ").strip().lower()

        if escolha in ['1', 'compilar']:
            run_pio_command(["run"])
        elif escolha in ['2', 'upload']:
            print("\n[DICA] Garanta que o GPIO 0 esteja ligado ao GND e resete a placa para entrar em modo Upload.")
            run_pio_command(["run", "-t", "upload"])
        elif escolha in ['3', 'monitor']:
            run_pio_command(["device", "monitor", "--baud", "115200"])

        elif escolha in ['4', 'clean', 'limpar']:
            run_pio_command(["run", "-t", "clean"])
        elif escolha in ['5', 'sair', 'exit']:
            print("Saindo...")
            break
        else:
            print(f"\n[AVISO] Opção '{escolha}' inválida!")

if __name__ == "__main__":
    main()
