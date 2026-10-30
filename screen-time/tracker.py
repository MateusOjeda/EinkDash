"""EinkDash: tempo de tela Windows, separado em TV e PC no Firestore."""

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

from usage_model import categorize_program, firestore_day, normalized_day, should_count

if sys.platform != "win32":
    raise SystemExit("Este programa foi criado para Windows.")

load_dotenv(Path(__file__).with_name(".env"))
SERVICE_ACCOUNT = os.getenv("EINKDASH_SERVICE_ACCOUNT", "").strip()
DEVICE_ID = re.sub(r"[^a-zA-Z0-9_-]", "-", os.getenv("EINKDASH_DEVICE_ID", socket.gethostname())).lower()
# O antigo EINKDASH_IDLE_SECONDS não se aplica mais: agora a regra é exclusiva do PC.
PC_IDLE_SECONDS = int(os.getenv("EINKDASH_PC_IDLE_SECONDS", "60"))
if PC_IDLE_SECONDS < 1:
    raise SystemExit("EINKDASH_PC_IDLE_SECONDS precisa ser maior que zero.")
SYNC_SECONDS = 300
SAVE_SECONDS = 15
SAMPLE_SECONDS = 1

DATA_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "EinkDashScreenTime"
DATA_DIR.mkdir(parents=True, exist_ok=True)
STATE_PATH = DATA_DIR / "usage-v3.json"

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32.GetForegroundWindow.argtypes = []
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


user32.GetLastInputInfo.argtypes = [ctypes.POINTER(LASTINPUTINFO)]
user32.GetLastInputInfo.restype = wintypes.BOOL
kernel32.GetTickCount.argtypes = []
kernel32.GetTickCount.restype = wintypes.DWORD


def idle_seconds() -> float:
    info = LASTINPUTINFO()
    info.cbSize = ctypes.sizeof(info)
    if not user32.GetLastInputInfo(ctypes.byref(info)):
        return 0.0
    return ((kernel32.GetTickCount() - info.dwTime) & 0xFFFFFFFF) / 1000.0


def window_title(handle: int) -> str:
    length = user32.GetWindowTextLengthW(handle)
    if length <= 0:
        return ""
    buffer = ctypes.create_unicode_buffer(min(length + 1, 2048))
    copied = user32.GetWindowTextW(handle, buffer, len(buffer))
    return buffer.value[:copied] if copied > 0 else ""


def foreground_category() -> tuple[str, str] | None:
    handle = user32.GetForegroundWindow()
    if not handle:
        return None
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(handle, ctypes.byref(pid))
    if not pid.value or pid.value == os.getpid():
        return None
    try:
        executable = psutil.Process(pid.value).name()
    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
        return None
    title = window_title(handle) if executable.casefold() == "chrome.exe" else ""
    return categorize_program(executable, title)


def read_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or not isinstance(data.get("days"), dict):
            raise ValueError("Formato de estado inválido")
        return data
    except (ValueError, OSError) as exc:
        raise RuntimeError(f"Não foi possível ler {path}: {exc}") from exc


def load_state() -> tuple[dict[str, dict[str, dict[str, float]]], set[str]]:
    if not STATE_PATH.exists():
        return {}, set()

    payload = read_json(STATE_PATH)
    days = {day: normalized_day(record) for day, record in payload["days"].items()}
    dirty = set(payload.get("dirty", []))
    return days, dirty


def save_state(days: dict, dirty: set[str]) -> None:
    temp = STATE_PATH.with_suffix(".tmp")
    temp.write_text(
        json.dumps({"days": days, "dirty": sorted(dirty)}, ensure_ascii=False),
        encoding="utf-8",
    )
    os.replace(temp, STATE_PATH)


def send_to_firestore(snapshot: dict) -> None:
    if not SERVICE_ACCOUNT or not Path(SERVICE_ACCOUNT).is_file():
        raise RuntimeError("Configure EINKDASH_SERVICE_ACCOUNT no arquivo .env")

    # Conta de serviço: usar apenas no próprio PC, não distribuir a chave.
    import firebase_admin
    from firebase_admin import credentials, firestore

    try:
        firebase_admin.get_app()
    except ValueError:
        firebase_admin.initialize_app(credentials.Certificate(SERVICE_ACCOUNT))
    db = firestore.client()

    for day, record in snapshot.items():
        summary = firestore_day(record)
        db.collection("screenTime").document(DEVICE_ID).collection("days").document(day).set({
            "date": day,
            "deviceId": DEVICE_ID,
            "platform": "windows",
            **summary,
            "updatedAt": firestore.SERVER_TIMESTAMP,
        })


def main() -> None:
    if not SERVICE_ACCOUNT or not Path(SERVICE_ACCOUNT).is_file():
        raise SystemExit("Não encontrei a chave. Confira EINKDASH_SERVICE_ACCOUNT no .env")

    days, dirty = load_state()
    print(f"EinkDash Screen Time | dispositivo: {DEVICE_ID}")
    print("Firestore: screenTime/{deviceId}/days/{AAAA-MM-DD}")
    print(f"Estado local: {STATE_PATH}")
    print(f"Amostragem: {SAMPLE_SECONDS}s | sincronização: {SYNC_SECONDS}s")
    print(f"PC: para de contar após {PC_IDLE_SECONDS}s sem mouse/teclado | TV: conta sempre")
    print("Pressione Ctrl+C para encerrar.\n")

    last_tick = time.monotonic()
    next_save = last_tick + SAVE_SECONDS
    next_sync = last_tick + SYNC_SECONDS
    pending: Future[None] | None = None
    submitted: dict | None = None
    last_category: tuple[str, str] | None = None

    with ThreadPoolExecutor(max_workers=1) as pool:
        try:
            while True:
                time.sleep(SAMPLE_SECONDS)
                now = time.monotonic()
                duration = min(max(now - last_tick, 0.0), 2.0)
                last_tick = now

                foreground = foreground_category()
                category = foreground if should_count(foreground, idle_seconds(), PC_IDLE_SECONDS) else None
                if category != last_category:
                    label = f"{category[0]}.{category[1]}" if category else "(sem atividade contabilizada)"
                    print(f"{datetime.now():%H:%M:%S} | {label}")
                    last_category = category

                if category:
                    day = datetime.now().astimezone().date().isoformat()
                    group, key = category
                    group_map = days.setdefault(day, {"tv": {}, "pc": {}}).setdefault(group, {})
                    group_map[key] = group_map.get(key, 0.0) + duration
                    dirty.add(day)

                if pending is not None and pending.done():
                    try:
                        pending.result()
                        for day, uploaded in (submitted or {}).items():
                            # Se surgiram mais segundos durante o envio, mantém pendente.
                            if days.get(day) == uploaded:
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
                        # Cópia profunda para não alterar dados enquanto o envio ocorre.
                        submitted = {
                            day: {group: values.copy() for group, values in days[day].items()}
                            for day in sorted(dirty)
                        }
                        pending = pool.submit(send_to_firestore, submitted)
                        print(f"{datetime.now():%H:%M:%S} | Enviando {len(submitted)} dia(s)...")
                    next_sync = now + SYNC_SECONDS
        except KeyboardInterrupt:
            print("\nEncerrando. Salvando dados locais...")
        finally:
            save_state(days, dirty)


if __name__ == "__main__":
    main()
