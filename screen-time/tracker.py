"""EinkDash: mede o tempo de uso de aplicativos em primeiro plano no Windows.

Amostra a janela ativa a cada 1 segundo e sincroniza um resumo cumulativo
por dia/dispositivo com o Cloud Firestore a cada 5 minutos.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from concurrent.futures import Future, ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import socket
import sys
import time

import psutil
from dotenv import load_dotenv

if sys.platform != "win32":
    raise SystemExit("Este programa foi criado para Windows.")

load_dotenv(Path(__file__).with_name(".env"))

USER_UID = os.getenv("EINKDASH_USER_UID", "").strip()
SERVICE_ACCOUNT = os.getenv("EINKDASH_SERVICE_ACCOUNT", "").strip()
DEVICE_ID = re.sub(r"[^a-zA-Z0-9_-]", "-", os.getenv("EINKDASH_DEVICE_ID", socket.gethostname())).lower()
IDLE_SECONDS = int(os.getenv("EINKDASH_IDLE_SECONDS", "0"))  # 0 = não interromper por inatividade
SYNC_SECONDS = 300
SAVE_SECONDS = 15
SAMPLE_SECONDS = 1

DATA_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "EinkDashScreenTime"
DATA_DIR.mkdir(parents=True, exist_ok=True)
STATE_PATH = DATA_DIR / "usage.json"

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


user32.GetLastInputInfo.argtypes = [ctypes.POINTER(LASTINPUTINFO)]
user32.GetLastInputInfo.restype = wintypes.BOOL
kernel32.GetTickCount.argtypes = []
kernel32.GetTickCount.restype = wintypes.DWORD

IGNORED = {"logonui.exe", "lockapp.exe", "winlogon.exe"}


def idle_seconds() -> float:
    last = LASTINPUTINFO()
    last.cbSize = ctypes.sizeof(last)
    if not user32.GetLastInputInfo(ctypes.byref(last)):
        return 0.0
    # DWORDs são de 32 bits; a máscara lida com o wrap de 49,7 dias.
    return ((kernel32.GetTickCount() - last.dwTime) & 0xFFFFFFFF) / 1000.0


def foreground_program() -> str | None:
    if IDLE_SECONDS > 0 and idle_seconds() >= IDLE_SECONDS:
        return None
    handle = user32.GetForegroundWindow()
    if not handle:
        return None
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(handle, ctypes.byref(pid))
    if not pid.value or pid.value == os.getpid():
        return None
    try:
        name = psutil.Process(pid.value).name().lower()
    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
        return None
    return name if name not in IGNORED else None


def load_state() -> tuple[dict[str, dict[str, float]], set[str]]:
    if not STATE_PATH.exists():
        return {}, set()
    try:
        obj = json.loads(STATE_PATH.read_text(encoding="utf-8"))
        days = obj.get("days", {})
        dirty = set(obj.get("dirty", []))
        if not isinstance(days, dict):
            raise ValueError("Formato local inválido")
        return days, dirty
    except (ValueError, OSError) as exc:
        # Não sobrescreve o arquivo com problemas.
        raise RuntimeError(f"Não foi possível ler {STATE_PATH}: {exc}") from exc


def save_state(days: dict[str, dict[str, float]], dirty: set[str]) -> None:
    temp = STATE_PATH.with_suffix(".tmp")
    temp.write_text(
        json.dumps({"days": days, "dirty": sorted(dirty)}, ensure_ascii=False),
        encoding="utf-8",
    )
    os.replace(temp, STATE_PATH)


def send_to_firestore(snapshot: dict[str, dict[str, float]]) -> None:
    if not USER_UID:
        raise RuntimeError("Configure EINKDASH_USER_UID no arquivo .env")
    if not SERVICE_ACCOUNT or not Path(SERVICE_ACCOUNT).is_file():
        raise RuntimeError("Configure EINKDASH_SERVICE_ACCOUNT com a localização da chave JSON")

    # SDK Admin para uso PRIVADO em computador confiável; jamais distribuir a chave.
    import firebase_admin
    from firebase_admin import credentials, firestore

    try:
        firebase_admin.get_app()
    except ValueError:
        firebase_admin.initialize_app(credentials.Certificate(SERVICE_ACCOUNT))
    db = firestore.client()
    for day, apps in snapshot.items():
        rounded_apps = {app: round(seconds) for app, seconds in apps.items()}
        db.collection("users").document(USER_UID).collection("screenTime").document(day) \
            .collection("devices").document(DEVICE_ID).set({
                "platform": "windows",
                "deviceId": DEVICE_ID,
                "date": day,
                "apps": rounded_apps,
                "totalSeconds": round(sum(apps.values())),
                "updatedAt": firestore.SERVER_TIMESTAMP,
            })


def main() -> None:
    days, dirty = load_state()
    print(f"EinkDash Screen Time | PC: {DEVICE_ID}")
    print(f"Estado local: {STATE_PATH}")
    print(f"Amostragem: {SAMPLE_SECONDS}s | Firebase: {SYNC_SECONDS}s")
    print("Pressione Ctrl+C para encerrar.\n")

    last_tick = time.monotonic()
    next_save = last_tick + SAVE_SECONDS
    next_sync = last_tick + SYNC_SECONDS
    pending: Future[None] | None = None
    submitted: dict[str, dict[str, float]] | None = None
    last_program: str | None = None

    with ThreadPoolExecutor(max_workers=1) as pool:
        try:
            while True:
                time.sleep(SAMPLE_SECONDS)
                now = time.monotonic()
                duration = min(max(now - last_tick, 0.0), 2.0)  # evita contar suspensão como uso
                last_tick = now

                app = foreground_program()
                if app != last_program:
                    print(f"{datetime.now():%H:%M:%S} | {app or '(sem atividade contabilizada)'}")
                    last_program = app
                if app:
                    day = datetime.now().astimezone().date().isoformat()
                    day_apps = days.setdefault(day, {})
                    day_apps[app] = day_apps.get(app, 0.0) + duration
                    dirty.add(day)

                if pending is not None and pending.done():
                    try:
                        pending.result()
                        for day, uploaded_apps in (submitted or {}).items():
                            # Se houve novos segundos enquanto enviávamos, mantém como pendente.
                            if days.get(day) == uploaded_apps:
                                dirty.discard(day)
                        print(f"{datetime.now():%H:%M:%S} | Sincronização concluída")
                    except Exception as exc:
                        print(f"{datetime.now():%H:%M:%S} | Falha no Firebase: {exc}")
                    finally:
                        pending = None
                        submitted = None
                        save_state(days, dirty)

                if now >= next_save:
                    save_state(days, dirty)
                    next_save = now + SAVE_SECONDS

                if now >= next_sync and pending is None:
                    save_state(days, dirty)
                    if dirty:
                        submitted = {day: days[day].copy() for day in sorted(dirty)}
                        pending = pool.submit(send_to_firestore, submitted)
                        print(f"{datetime.now():%H:%M:%S} | Enviando {len(submitted)} dia(s)...")
                    next_sync = now + SYNC_SECONDS
        except KeyboardInterrupt:
            print("\nEncerrando. Salvando dados locais...")
        finally:
            save_state(days, dirty)


if __name__ == "__main__":
    main()
