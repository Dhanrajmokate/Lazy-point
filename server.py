import os
import sys
import json
import socket
import asyncio

# Ensure UTF-8 support on Windows terminals
if sys.platform == "win32":
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        if hasattr(sys.stderr, "reconfigure"):
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

import qrcode
from pathlib import Path
from starlette.applications import Starlette

from starlette.routing import Route, WebSocketRoute, Mount
from starlette.staticfiles import StaticFiles
from starlette.responses import FileResponse
from starlette.websockets import WebSocket, WebSocketDisconnect
import uvicorn

from controller import WindowsController

# Initialize controller
controller = WindowsController()

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

def get_local_ip() -> str:
    """Find the local network IP address of this machine."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Connect to public DNS to determine default routing interface
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip

async def index(request):
    """Serve the main remote controller webpage."""
    return FileResponse(STATIC_DIR / "index.html")

async def websocket_endpoint(websocket: WebSocket):
    """Handle ultra-low latency real-time commands from the mobile client."""
    await websocket.accept()
    client_ip = websocket.client.host if websocket.client else "Unknown"
    print(f"\n[+] Phone connected from: {client_ip}")

    try:
        while True:
            raw_data = await websocket.receive_text()
            try:
                msg = json.loads(raw_data)
            except json.JSONDecodeError:
                continue

            msg_type = msg.get("type")

            if msg_type == "move":
                dx = float(msg.get("dx", 0))
                dy = float(msg.get("dy", 0))
                sens = float(msg.get("sensitivity", 1.6))
                controller.move_cursor_relative(dx, dy, sensitivity=sens)

            elif msg_type == "click":
                button = msg.get("button", "left")
                controller.mouse_click(button)

            elif msg_type == "down":
                button = msg.get("button", "left")
                controller.mouse_down(button)

            elif msg_type == "up":
                button = msg.get("button", "left")
                controller.mouse_up(button)

            elif msg_type == "scroll":
                delta_y = int(msg.get("delta_y", 0))
                controller.scroll(delta_y)

            elif msg_type == "volume":
                action = msg.get("action")
                if action == "up":
                    controller.volume_up()
                elif action == "down":
                    controller.volume_down()
                elif action == "mute":
                    controller.volume_mute()

            elif msg_type == "media":
                action = msg.get("action")
                if action == "playpause":
                    controller.media_play_pause()
                elif action == "seek_fwd":
                    controller.media_seek_forward()
                elif action == "seek_bwd":
                    controller.media_seek_backward()
                elif action == "fullscreen":
                    controller.toggle_fullscreen()

            elif msg_type == "key":
                key = msg.get("key", "")
                if key:
                    controller.press_key(key)

            elif msg_type == "type":
                text = msg.get("text", "")
                if text:
                    controller.type_unicode_text(text)

    except WebSocketDisconnect:
        print(f"[-] Phone disconnected: {client_ip}")
    except Exception as e:
        print(f"[!] Error in connection: {e}")

routes = [
    Route("/", endpoint=index),
    WebSocketRoute("/ws", endpoint=websocket_endpoint),
    Mount("/", app=StaticFiles(directory=STATIC_DIR), name="static"),
]

app = Starlette(routes=routes)

def print_banner(host_ip: str, port: int):
    url = f"http://{host_ip}:{port}"
    print("=" * 60)
    print("           🛋️  LAZY POINT — PC REMOTE SERVER")
    print("=" * 60)
    print(f"\n👉 Connect your phone on the same Wi-Fi:\n")
    print(f"   URL: {url}\n")
    print("📱 Scan this QR code with your Android Camera / Google Lens:")
    print("-" * 60)
    try:
        qr = qrcode.QRCode(box_size=1, border=1)
        qr.add_data(url)
        qr.print_ascii(invert=True)
    except Exception as e:
        pass
    print("-" * 60)
    print("Tips:")
    print("  • Keep your laptop and phone connected to the SAME Wi-Fi network.")
    print("  • In Chrome on Android, tap 'Add to Home Screen' for fullscreen app mode.")
    print("  • Press Ctrl+C in this terminal to stop the server.")
    print("=" * 60 + "\n")

if __name__ == "__main__":
    PORT = 8000
    host_ip = get_local_ip()
    print_banner(host_ip, PORT)
    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="warning")
