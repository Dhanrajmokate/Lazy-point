# 🛋️ Lazy Point — Phone PC Remote Controller

**Lazy Point** turns your Android phone into an ultra-responsive wireless trackpad, keyboard, and media controller for your laptop/PC. Built specifically for when you're lying in bed or sitting on the couch watching movies and don't want to get up just to tweak the volume or skip an intro!

---

## ✨ Features

- 👆 **Smooth Virtual Trackpad:**
  - Single finger glide = Move mouse cursor (with dynamic acceleration).
  - Single tap = Left Click.
  - Two-finger tap = Right Click.
  - Two-finger vertical drag = Mouse scroll wheel.
  - Dedicated Left / Right Click buttons for tactile control.
- 🔊 **Master Volume Control:**
  - One-tap Volume Up, Volume Down, and Mute (directly adjusts Windows Master Volume).
- 🎬 **Couch Potato Media Deck:**
  - **Play / Pause** toggle (sends Spacebar / Media Play).
  - **-10s / +10s Seek** buttons (sends Left / Right arrow keys).
  - **Fullscreen (F)** toggle (works instantly in YouTube, VLC, Netflix, etc.).
  - **Subtitles (C)** toggle.
  - **Escape (Esc)** to exit menus or players.
- ⌨️ **Text & Search Input:**
  - Type text on your phone and beam it straight to the PC search bar.
  - Quick **Enter** and **Backspace** buttons.
- 📳 **Haptic Feedback:**
  - Vibrations on button presses for a physical remote feel (toggleable in settings).
- ⚡ **Zero Installation on Phone:**
  - Connects instantly in any mobile browser (Chrome, Firefox).
  - Can be added to your Android Home Screen as a standalone fullscreen PWA app.

---

## 🚀 How to Run

### Option 1: One-Click Launcher
Double-click [`start.bat`](start.bat).

### Option 2: Terminal
```bash
python server.py
```

---

## 📱 How to Connect Your Android Phone

1. Make sure your **Laptop** and **Android Phone** are connected to the **same Wi-Fi network**.
2. Run `python server.py` (or double-click `start.bat`).
3. You will see a **QR code** in the terminal:
   - Open your Android Camera or Google Lens.
   - Point it at the QR code and tap the link (e.g., `http://192.168.x.x:8000`).
4. **(Optional Pro Tip):** In Chrome on Android, tap the three dots `⋮` in the top right and select **"Add to Home Screen"** or **"Install app"**. Lazy Point will open as a standalone fullscreen app without any browser URL bars!

---

## 🛠️ Project Structure

```
c:\lazy point\
├── server.py              # WebSocket & HTTP server + QR code terminal generator
├── controller.py          # Native Windows input controller (mouse, volume, keys)
├── start.bat              # One-click Windows launcher
├── requirements.txt       # Dependencies
├── README.md              # Documentation
└── static\
    ├── index.html         # Responsive touch UI layout
    ├── style.css          # Dark-mode styling and animations
    ├── app.js             # Touch gesture recognition & WebSocket client
    ├── manifest.json      # PWA metadata for Android installation
    └── icon.svg           # Remote controller icon
```

---

## 🔧 Architecture & Tech Stack

- **Backend:** Python + Starlette + Uvicorn + WebSockets
- **Hardware Automation:** Native Windows User32 API via `ctypes` (under 1ms input latency)
- **Frontend:** HTML5, CSS3, JavaScript (Pointer & Touch Events API, Vibration API)
