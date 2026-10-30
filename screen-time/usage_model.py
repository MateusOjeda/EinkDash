"""Categorias do EinkDash e resumo diário (independente do Windows/Firebase)."""

from __future__ import annotations

import re

TV_APPS = ("youtube", "mpc", "netflix")
PC_APPS = (
    "vscode", "photoshop", "explorer", "chatgpt", "chromeOutros",
    "amazonMusic", "whatsapp",
)

CHROME_SUFFIX = re.compile(r"\s+-\s+Google Chrome(?:\s+\([^)]*\))?\s*$", re.IGNORECASE)


def categorize_chrome_title(title: str) -> tuple[str, str]:
    """Heurística: identifica YouTube, Netflix e ChatGPT pelo título, NÃO pela URL."""
    normalized = " ".join(CHROME_SUFFIX.sub("", title or "").casefold().split())
    if re.search(r"\byoutube\b", normalized):
        return "tv", "youtube"
    if re.search(r"\bnetflix\b", normalized):
        return "tv", "netflix"
    if re.search(r"\bchatgpt\b", normalized):
        return "pc", "chatgpt"
    return "pc", "chromeOutros"


def categorize_program(executable: str, title: str = "") -> tuple[str, str] | None:
    """Cada programa ativo pertence a uma única categoria."""
    name = (executable or "").strip().casefold()
    if not name or name in {"logonui.exe", "lockapp.exe", "winlogon.exe"}:
        return None
    if name == "chrome.exe":
        return categorize_chrome_title(title)
    if name in {"mpc-hc64.exe", "mpc-hc.exe"}:
        return "tv", "mpc"
    if name == "netflix.exe":
        return "tv", "netflix"
    if name == "code.exe":
        return "pc", "vscode"
    if name == "photoshop.exe":
        return "pc", "photoshop"
    if name == "explorer.exe":
        return "pc", "explorer"
    if name == "chatgpt.exe":
        return "pc", "chatgpt"
    if name in {"amazon music.exe", "amazonmusic.exe"}:
        return "pc", "amazonMusic"
    if name in {"whatsapp.root.exe", "whatsapp.exe"}:
        return "pc", "whatsapp"
    # Outros programas ficam identificados individualmente pelo nome do executável.
    return "pc", name


def should_count(category: tuple[str, str] | None, seconds_since_input: float, pc_idle_limit: int) -> bool:
    """TV continua contabilizando sem interação; PC exige atividade recente."""
    if category is None:
        return False
    if category[0] == "tv":
        return True
    return 0 <= seconds_since_input < pc_idle_limit


def normalized_day(raw: dict) -> dict[str, dict[str, float]]:
    """Valida o estado diário local e mantém cada aplicativo individual."""
    if not isinstance(raw, dict):
        raise ValueError("Dia local inválido")
    converted = {"tv": {}, "pc": {}}
    for group in converted:
        origin = raw.get(group, {})
        if not isinstance(origin, dict):
            raise ValueError(f"Categoria local inválida: {group}")
        for key, value in origin.items():
            if not isinstance(key, str) or not key or key == "totalSeconds":
                raise ValueError(f"Nome de programa inválido em {group}")
            if group == "tv" and key not in TV_APPS:
                raise ValueError(f"Programa inválido em tv: {key}")
            if not isinstance(value, (int, float)) or isinstance(value, bool) or not 0 <= value < 1e12:
                raise ValueError(f"Valor inválido para {group}.{key}")
            if value:
                converted[group][key] = float(value)
    return converted


def firestore_day(day_data: dict[str, dict[str, float]]) -> dict:
    """Gera mapas tv/pc e totais separados na raiz do documento diário."""
    clean = normalized_day(day_data)
    output = {}
    for group in ("tv", "pc"):
        # Categorias principais existem mesmo quando ainda não foram utilizadas.
        keys = TV_APPS if group == "tv" else PC_APPS
        raw = clean[group]
        rounded = {key: round(raw.get(key, 0)) for key in keys}
        # Inclui os demais aplicativos pelo próprio nome do executável.
        for key, value in raw.items():
            if key not in rounded:
                rounded[key] = round(value)
        output[group] = rounded
        total_field = "totalSecondsTV" if group == "tv" else "totalSecondsPC"
        output[total_field] = sum(rounded.values())
    return output
