#!/usr/bin/env python3
"""Buddy Agent - serve tokens de CLIs de IA instalados para o ESP8266 via HTTP.

O ESP busca http://<pc>:8787/data a cada poucos segundos.
Com --serial tambem empurra os dados pelo cabo USB (fallback).
"""

import argparse
import glob
import json
import os
import shutil
import socket
import sqlite3
import sys
import threading
import time
from datetime import date, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

STATE_PATH = Path(__file__).resolve().parent / ".buddy_state.json"
LIMITS_PATH = Path(__file__).resolve().parent / "limits.json"
DEFAULT_PORT = 8000
BROADCAST_PORT = 8788
BAUD = 115200
REFRESH_INTERVAL = 15.0

HOME = Path.home()
CODEX_DIR = HOME / ".codex" / "sessions"
CLAUDE_DIR = HOME / ".claude" / "projects"
OPENCODE_DB = HOME / ".local" / "share" / "opencode" / "opencode.db"

EMPTY_PAYLOAD = json.dumps({"cmd": "data", "total": 0, "today": 0, "tools": []}).encode()


def log(msg: str) -> None:
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def detect_tools() -> dict:
    return {
        "codex": shutil.which("codex") is not None or CODEX_DIR.exists(),
        "claude": shutil.which("claude") is not None or CLAUDE_DIR.exists(),
        "opencode": shutil.which("opencode") is not None or OPENCODE_DB.exists(),
    }


def load_state() -> dict:
    try:
        with open(STATE_PATH) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(state: dict) -> None:
    tmp = STATE_PATH.with_suffix(".tmp")
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, STATE_PATH)


def load_limits() -> dict:
    """Limites semanais manuais (tokens/semana) por ferramenta - limits.json."""
    try:
        with open(LIMITS_PATH) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def today_start() -> datetime:
    return datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)


def parse_iso(ts: str) -> datetime | None:
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone().replace(tzinfo=None)
    except (ValueError, AttributeError):
        return None


# ---------------------------------------------------------------- codex

def scan_codex_file(path: Path) -> tuple[int, int]:
    day0 = today_start()
    last_total = 0
    total_before_today = 0
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if '"token_count"' not in line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            payload = d.get("payload") or {}
            info = payload.get("info") or d.get("info") or {}
            t = (info.get("total_token_usage") or {}).get("total_tokens")
            if not t:
                continue
            ts = parse_iso(d.get("timestamp", ""))
            if ts is not None and ts < day0:
                total_before_today = t
            last_total = t
    today = max(0, last_total - total_before_today) if last_total else 0
    return last_total, today


def scan_codex(state: dict) -> tuple[int, int]:
    day = date.today().isoformat()
    files_state = state.setdefault("codex", {}).setdefault("files", {})
    if state.get("codex", {}).get("day") != day:
        files_state.clear()
        state["codex"]["day"] = day

    seen = set()
    for path in glob.glob(str(CODEX_DIR / "**" / "*.jsonl"), recursive=True):
        seen.add(path)
        st = os.stat(path)
        cached = files_state.get(path)
        if cached and cached.get("size") == st.st_size:
            continue
        total, today = scan_codex_file(Path(path))
        files_state[path] = {"size": st.st_size, "total": total, "today": today}

    for path in list(files_state):
        if path not in seen:
            del files_state[path]

    total = sum(v["total"] for v in files_state.values())
    today = sum(v["today"] for v in files_state.values())
    return total, today


# ---------------------------------------------------------------- claude

def scan_claude_file(path: Path) -> tuple[int, int, int]:
    day0 = today_start()
    week0 = datetime.now() - timedelta(days=7)
    total = 0
    today = 0
    week = 0
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if '"usage"' not in line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("type") != "assistant":
                continue
            u = (d.get("message") or {}).get("usage") or {}
            n = (
                u.get("input_tokens", 0)
                + u.get("cache_creation_input_tokens", 0)
                + u.get("cache_read_input_tokens", 0)
                + u.get("output_tokens", 0)
            )
            if not n:
                continue
            total += n
            ts = parse_iso(d.get("timestamp", ""))
            if ts is not None and ts >= day0:
                today += n
            if ts is not None and ts >= week0:
                week += n
    return total, today, week


def scan_claude(state: dict) -> tuple[int, int, int]:
    day = date.today().isoformat()
    files_state = state.setdefault("claude", {}).setdefault("files", {})
    if state.get("claude", {}).get("day") != day:
        files_state.clear()
        state["claude"]["day"] = day

    seen = set()
    for path in glob.glob(str(CLAUDE_DIR / "**" / "*.jsonl"), recursive=True):
        seen.add(path)
        st = os.stat(path)
        cached = files_state.get(path)
        if cached and cached.get("size") == st.st_size:
            continue
        total, today, week = scan_claude_file(Path(path))
        files_state[path] = {"size": st.st_size, "total": total, "today": today, "week": week}

    for path in list(files_state):
        if path not in seen:
            del files_state[path]

    total = sum(v["total"] for v in files_state.values())
    today = sum(v["today"] for v in files_state.values())
    week = sum(v.get("week", 0) for v in files_state.values())
    return total, today, week


# ---------------------------------------------------------------- opencode

def scan_opencode() -> tuple[int, int, int]:
    if not OPENCODE_DB.exists():
        return 0, 0, 0
    day0_ms = int(today_start().timestamp() * 1000)
    week0_ms = int((datetime.now() - timedelta(days=7)).timestamp() * 1000)
    uri = f"file:{OPENCODE_DB}?mode=ro"
    conn = sqlite3.connect(uri, uri=True, timeout=5)
    try:
        row = conn.execute(
            """
            SELECT
              COALESCE(SUM(json_extract(data, '$.tokens.total')), 0),
              COALESCE(SUM(CASE WHEN time_created >= ? THEN json_extract(data, '$.tokens.total') ELSE 0 END), 0),
              COALESCE(SUM(CASE WHEN time_created >= ? THEN json_extract(data, '$.tokens.total') ELSE 0 END), 0)
            FROM message
            WHERE json_extract(data, '$.role') = 'assistant'
              AND json_extract(data, '$.tokens.total') IS NOT NULL
            """,
            (day0_ms, week0_ms),
        ).fetchone()
        return int(row[0]), int(row[1]), int(row[2])
    finally:
        conn.close()


# ---------------------------------------------------------------- limites

def scan_codex_limit() -> dict | None:
    """Le o rate_limit atual (used_percent/resets_at) dos arquivos mais recentes do Codex."""
    try:
        files = sorted(
            glob.glob(str(CODEX_DIR / "**" / "*.jsonl"), recursive=True),
            key=os.path.getmtime,
            reverse=True,
        )[:3]
    except OSError:
        return None
    for path in files:
        last = None
        try:
            with open(path, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if '"rate_limits"' not in line:
                        continue
                    try:
                        d = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    rl = (d.get("payload") or {}).get("rate_limits") or d.get("rate_limits")
                    if rl and rl.get("primary"):
                        last = rl
        except OSError:
            continue
        if last:
            return last
    return None


def scan_activity(installed: dict) -> tuple[bool, str | None]:
    """(ativo, ferramenta) - algum log de CLI escrito nos ultimos 90s = trabalhando."""
    cutoff = time.time() - 90
    best_mtime = 0.0
    best_tool = None
    sources = []
    if installed.get("codex"):
        sources.append(("codex", str(CODEX_DIR / "**" / "*.jsonl")))
    if installed.get("claude"):
        sources.append(("claude", str(CLAUDE_DIR / "**" / "*.jsonl")))
    if installed.get("opencode") and OPENCODE_DB.exists():
        sources.append(("opencode", str(OPENCODE_DB) + "*"))
    for tid, pattern in sources:
        for path in glob.glob(pattern, recursive=True):
            try:
                m = os.path.getmtime(path)
            except OSError:
                continue
            if m > best_mtime:
                best_mtime = m
                best_tool = tid
    return best_mtime >= cutoff, (best_tool if best_mtime >= cutoff else state_last_tool())


_last_active_tool_holder: dict = {}


def state_last_tool() -> str | None:
    return _last_active_tool_holder.get("tool")


# ---------------------------------------------------------------- alertas

def band_for(pct: float) -> int:
    if pct >= 100:
        return 100
    if pct >= 90:
        return 90
    if pct >= 80:
        return 80
    return 0


def build_alerts(state: dict, tools: list, active: bool, active_tool: str | None) -> list:
    """Gera alertas com dedupe por banda/janela; fila de /alert e transicao ativo->parado."""
    alerts = []
    now = int(time.time())

    # fila recebida dos hooks via POST /alert
    alerts.extend(BuddyHandler.drain_alerts())

    # faixas de limite (80/90/100) por ferramenta, re-armam quando a janela muda
    bands = state.setdefault("alert_bands", {})
    for t in tools:
        pct = t.get("pct", -1)
        if pct is None or pct < 0:
            continue
        st = bands.setdefault(t["id"], {"band": 0, "resets_at": None})
        if st.get("resets_at") != t.get("resetAt"):
            st["band"] = 0
            st["resets_at"] = t.get("resetAt")
        nb = band_for(pct)
        if nb > st["band"]:
            st["band"] = nb
            alerts.append({"k": "limit", "tool": t["id"], "band": nb, "pct": int(pct), "ts": now})
            log(f"ALERTA limite {t['id']} {pct:.0f}% >= {nb}%")

    # agente terminou de trabalhar (ativo -> parado)
    act = state.setdefault("activity", {"was_active": False, "tool": None})
    if act.get("was_active") and not active and act.get("tool"):
        alerts.append({"k": "done", "tool": act["tool"], "ts": now})
        log(f"ALERTA {act['tool']} terminou")
    act["was_active"] = active
    act["tool"] = active_tool or act.get("tool")
    _last_active_tool_holder["tool"] = active_tool or _last_active_tool_holder.get("tool")

    return alerts[-6:]


# ---------------------------------------------------------------- payload

def collect(installed: dict, state: dict) -> dict:
    tools = []
    limits = load_limits()
    meta = [
        ("codex", "Codex", scan_codex),
        ("claude", "Claude", scan_claude),
        ("opencode", "OpenCode", None),
    ]
    for tid, name, fn in meta:
        if not installed.get(tid):
            continue
        week = None
        try:
            if tid == "opencode":
                total, today, week = scan_opencode()
            elif tid == "claude":
                total, today, week = fn(state)
            else:
                total, today = fn(state)
        except Exception as e:
            log(f"erro ao ler {tid}: {e}")
            total, today, week = 0, 0, None
        entry = {"id": tid, "name": name, "total": total, "today": today, "pct": -1, "resetAt": 0}

        # limite semanal manual (limits.json) tem prioridade
        manual = limits.get(tid)
        if isinstance(manual, (int, float)) and manual > 0 and week is not None:
            entry["pct"] = min(100, int(round(week / manual * 100)))
            entry["week"] = week

        # Codex: rate limit oficial dos logs (janela de 7 dias)
        if tid == "codex" and entry["pct"] < 0:
            try:
                codex_rl = scan_codex_limit()
            except Exception:
                codex_rl = None
            if codex_rl and codex_rl.get("primary"):
                raw = float(codex_rl["primary"].get("used_percent", -1))
                entry["pct"] = int(round(raw)) if raw >= 0 else -1
                entry["resetAt"] = int(codex_rl["primary"].get("resets_at") or 0)

        tools.append(entry)

    active, active_tool = scan_activity(installed)
    alerts = build_alerts(state, tools, active, active_tool)

    return {
        "cmd": "data",
        "total": sum(t["total"] for t in tools),
        "today": sum(t["today"] for t in tools),
        "active": active,
        "tools": tools,
        "alerts": alerts,
    }


# ---------------------------------------------------------------- broadcast

def get_lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def broadcast_loop(http_port: int) -> None:
    """Anuncia o IP do agent na rede para o ESP se reencontrar quando o IP mudar."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    last_ip = None
    while True:
        try:
            ip = get_lan_ip()
            if ip != last_ip:
                log(f"agent em http://{ip}:{http_port}/data (broadcast udp {BROADCAST_PORT})")
                last_ip = ip
            msg = f"BUDDY {ip} {http_port}".encode()
            for target in ("255.255.255.255", "192.168.1.255"):
                try:
                    sock.sendto(msg, (target, BROADCAST_PORT))
                except OSError:
                    pass
        except Exception:
            pass
        time.sleep(5)


# ---------------------------------------------------------------- http

class BuddyHandler(BaseHTTPRequestHandler):
    payload: bytes = EMPTY_PAYLOAD
    served = 0
    _alert_lock = threading.Lock()
    _alert_queue: list = []

    @classmethod
    def drain_alerts(cls) -> list:
        with cls._alert_lock:
            items = list(cls._alert_queue)
            cls._alert_queue.clear()
        return items

    def do_POST(self):
        if self.path == "/alert":
            try:
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
            except (ValueError, json.JSONDecodeError):
                self.send_error(400)
                return
            kind = body.get("k") or body.get("kind") or "attention"
            if kind not in ("attention", "done"):
                kind = "attention"
            alert = {"k": kind, "tool": body.get("tool") or "claude", "ts": int(time.time())}
            with BuddyHandler._alert_lock:
                BuddyHandler._alert_queue.append(alert)
            log(f"alerta recebido via hook: {kind} ({alert['tool']})")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')
        else:
            self.send_error(404)

    def do_GET(self):
        if self.path == "/data":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(BuddyHandler.payload)))
            self.end_headers()
            self.wfile.write(BuddyHandler.payload)
            BuddyHandler.served += 1
        elif self.path == "/":
            body = (
                "<!doctype html><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Buddy Agent</title>"
                "<body style='font-family:system-ui;background:#111;color:#eee;padding:24px'>"
                "<h1>&#129302; Buddy Agent</h1>"
                f"<p>Payloads servidos: {BuddyHandler.served}</p>"
                "<p><a style='color:#8cf' href='/data'>/data</a></p>"
                f"<pre style='color:#8f8'>{BuddyHandler.payload.decode()}</pre></body>"
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        pass  # silencia log padrao do http.server


# ---------------------------------------------------------------- serial

def find_serial_port(preferred: str | None) -> str | None:
    if preferred:
        return preferred
    candidates = sorted(glob.glob("/dev/ttyUSB*")) + sorted(glob.glob("/dev/ttyACM*"))
    return candidates[0] if candidates else None


def open_serial(port: str):
    import serial
    ser = serial.Serial(port, BAUD, timeout=1)
    time.sleep(1.8)  # ESP8266 reseta ao abrir a porta
    ser.reset_input_buffer()
    return ser


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description="Buddy Agent - tokens de IA para o ESP8266")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="porta do servidor HTTP")
    ap.add_argument("--interval", type=float, default=REFRESH_INTERVAL)
    ap.add_argument("--once", action="store_true", help="coleta uma vez, imprime e sai")
    ap.add_argument("--dry-run", action="store_true", help="so imprime o payload")
    ap.add_argument("--serial", action="store_true", help="tambem empurra via cabo USB")
    ap.add_argument("--serial-port", help="porta serial (auto-detect se omitido)")
    args = ap.parse_args()

    installed = detect_tools()
    found = [k for k, v in installed.items() if v]
    log(f"CLIs detectados: {', '.join(found) or 'nenhum'}")
    if not found:
        log("nenhum CLI encontrado - nada a mostrar")
        return 1

    state = load_state()

    if args.once or args.dry_run:
        payload = collect(installed, state)
        save_state(state)
        log(json.dumps(payload))
        return 0

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), BuddyHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    threading.Thread(target=broadcast_loop, args=(args.port,), daemon=True).start()
    log(f"servindo http://0.0.0.0:{args.port}/data (ESP busca aqui)")

    ser = None
    while True:
        payload = collect(installed, state)
        save_state(state)
        BuddyHandler.payload = json.dumps(payload, separators=(",", ":")).encode()
        tools_str = ", ".join(f"{t['id']}={t['total']}" for t in payload["tools"])
        log(f"atualizado: total={payload['total']} hoje={payload['today']} | {tools_str}")

        if args.serial:
            if ser is None or not ser.is_open:
                port = find_serial_port(args.serial_port)
                if port is None:
                    log("nenhuma porta serial, tentando de novo em 5s")
                else:
                    try:
                        ser = open_serial(port)
                        log(f"serial conectada em {port}")
                    except Exception as e:
                        log(f"falha ao abrir {port}: {e}")
            if ser is not None and ser.is_open:
                try:
                    ser.write(BuddyHandler.payload + b"\n")
                    ser.flush()
                except Exception as e:
                    log(f"erro serial: {e}")
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None

        time.sleep(args.interval)

    return 0


if __name__ == "__main__":
    sys.exit(main())
