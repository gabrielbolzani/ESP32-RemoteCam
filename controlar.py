"""Painel PlatformIO: modo interativo e automação com código de saída real."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT_DIR = Path(__file__).resolve().parent
PIO_PATH = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"


def run_pio_command(command_args, project_dir=PROJECT_DIR):
    executable = os.environ.get("PLATFORMIO_EXE") or shutil.which("pio")
    if not executable and PIO_PATH.is_file():
        executable = str(PIO_PATH)
    if not executable:
        print("PlatformIO não encontrado.", file=sys.stderr)
        return 127
    command = [executable, *command_args]
    print("[INFO]", subprocess.list2cmdline(command), flush=True)
    return subprocess.run(command, cwd=project_dir, shell=False, check=False).returncode


def execute(action, port=None):
    commands = {
        "compile": ["run", "-e", "esp32cam"],
        "upload": ["run", "-e", "esp32cam", "-t", "upload"],
        "monitor": ["device", "monitor", "--baud", "115200"],
        "devices": ["device", "list"],
        "clean": ["run", "-e", "esp32cam", "-t", "clean"],
    }
    command = commands[action].copy()
    if port and action in ("upload", "monitor"):
        command.extend(["--upload-port" if action == "upload" else "--port", port])
    return run_pio_command(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=["compile", "upload", "monitor", "devices", "clean"])
    parser.add_argument("--port", help="Porta serial explícita, por exemplo COM5")
    arguments = parser.parse_args()
    if arguments.action:
        return execute(arguments.action, arguments.port)
    while True:
        print("\n1. COMPILAR\n2. UPLOAD\n3. COMPILAR + UPLOAD\n4. MONITOR SERIAL\n5. LIMPAR BUILD\n6. SAIR")
        choice = input("Escolha [1-6]: ").strip()
        if choice == "6":
            return 0
        if choice == "3":
            if execute("compile", arguments.port) == 0:
                execute("upload", arguments.port)
            else:
                print("Compilação falhou; upload não executado.")
        elif choice in {"1", "2", "4", "5"}:
            execute({"1": "compile", "2": "upload", "4": "monitor", "5": "clean"}[choice], arguments.port)
        else:
            print("Opção inválida.")


if __name__ == "__main__":
    raise SystemExit(main())
