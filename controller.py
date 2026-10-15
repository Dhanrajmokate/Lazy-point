import ctypes
import time
from ctypes import wintypes

# Windows API constants
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_WHEEL = 0x0800

KEYEVENTF_KEYDOWN = 0x0000
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

# Virtual key codes
VK_BACK = 0x08
VK_TAB = 0x09
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF
VK_MEDIA_NEXT_TRACK = 0xB0
VK_MEDIA_PREV_TRACK = 0xB1
VK_MEDIA_STOP = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3

KEY_MAP = {
    "space": VK_SPACE,
    "enter": VK_RETURN,
    "return": VK_RETURN,
    "esc": VK_ESCAPE,
    "escape": VK_ESCAPE,
    "backspace": VK_BACK,
    "tab": VK_TAB,
    "left": VK_LEFT,
    "right": VK_RIGHT,
    "up": VK_UP,
    "down": VK_DOWN,
    "volup": VK_VOLUME_UP,
    "voldown": VK_VOLUME_DOWN,
    "mute": VK_VOLUME_MUTE,
    "playpause": VK_MEDIA_PLAY_PAUSE,
    "next": VK_MEDIA_NEXT_TRACK,
    "prev": VK_MEDIA_PREV_TRACK,
}

# Structures for SendInput
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG)),
    ]

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG)),
    ]

class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]

class INPUT_UNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
        ("hi", HARDWAREINPUT),
    ]

class INPUT(ctypes.Structure):
    _fields_ = [
        ("type", wintypes.DWORD),
        ("u", INPUT_UNION),
    ]

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

def send_input_single(inp):
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

class WindowsController:
    """Ultra-responsive native Windows controller for cursor, keyboard, and volume."""

    def __init__(self):
        # Fractional remainder accumulators for smooth sub-pixel tracking
        self.rem_x = 0.0
        self.rem_y = 0.0

    def move_cursor_relative(self, dx: float, dy: float, sensitivity: float = 1.6, acceleration: bool = True):
        """Move cursor relative to current position with optional acceleration and smoothing."""
        # Optional velocity-based curve (acceleration)
        dist = (dx * dx + dy * dy) ** 0.5
        accel_factor = 1.0
        if acceleration and dist > 2.0:
            accel_factor = 1.0 + min((dist / 30.0), 1.5)

        total_x = (dx * sensitivity * accel_factor) + self.rem_x
        total_y = (dy * sensitivity * accel_factor) + self.rem_y

        step_x = int(total_x)
        step_y = int(total_y)

        self.rem_x = total_x - step_x
        self.rem_y = total_y - step_y

        if step_x != 0 or step_y != 0:
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_MOVE, step_x, step_y, 0, 0)

    def mouse_click(self, button: str = "left"):
        """Perform mouse clicks: left, right, middle, or double."""
        btn = button.lower()
        if btn == "left":
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
            time.sleep(0.015)
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        elif btn == "right":
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
            time.sleep(0.015)
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
        elif btn == "middle":
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0)
            time.sleep(0.015)
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0)
        elif btn == "double":
            self.mouse_click("left")
            time.sleep(0.05)
            self.mouse_click("left")

    def mouse_down(self, button: str = "left"):
        """Hold down mouse button (for drag and drop)."""
        if button.lower() == "left":
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)

    def mouse_up(self, button: str = "left"):
        """Release mouse button."""
        if button.lower() == "left":
            ctypes.windll.user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)

    def scroll(self, delta_y: int):
        """Scroll vertical wheel: positive = up, negative = down."""
        # 120 is standard WHEEL_DELTA in Windows
        amount = int(delta_y * 120)
        ctypes.windll.user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, amount, 0)

    def press_key(self, key_name: str):
        """Press and release a mapped key or single character."""
        lower = key_name.lower()
        vk = KEY_MAP.get(lower)

        if vk is not None:
            ctypes.windll.user32.keybd_event(vk, 0, KEYEVENTF_KEYDOWN, 0)
            time.sleep(0.01)
            ctypes.windll.user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
        elif len(key_name) == 1:
            # Single character (e.g. 'f' for fullscreen, 'c' for captions)
            char_code = ord(key_name.upper())
            ctypes.windll.user32.keybd_event(char_code, 0, KEYEVENTF_KEYDOWN, 0)
            time.sleep(0.01)
            ctypes.windll.user32.keybd_event(char_code, 0, KEYEVENTF_KEYUP, 0)

    def volume_up(self, steps: int = 1):
        for _ in range(steps):
            self.press_key("volup")
            time.sleep(0.01)

    def volume_down(self, steps: int = 1):
        for _ in range(steps):
            self.press_key("voldown")
            time.sleep(0.01)

    def volume_mute(self):
        self.press_key("mute")

    def media_play_pause(self):
        self.press_key("playpause")

    def media_seek_forward(self):
        """Seek forward in media players (sends Right arrow)."""
        self.press_key("right")

    def media_seek_backward(self):
        """Seek backward in media players (sends Left arrow)."""
        self.press_key("left")

    def toggle_fullscreen(self):
        """Toggle fullscreen (sends 'f' key used by YouTube, VLC, Netflix)."""
        self.press_key("f")

    def type_unicode_text(self, text: str):
        """Type full Unicode string into currently focused application."""
        for char in text:
            # Key down
            inp_down = INPUT()
            inp_down.type = INPUT_KEYBOARD
            inp_down.u.ki = KEYBDINPUT(
                wVk=0,
                wScan=ord(char),
                dwFlags=KEYEVENTF_UNICODE,
                time=0,
                dwExtraInfo=None,
            )
            send_input_single(inp_down)

            # Key up
            inp_up = INPUT()
            inp_up.type = INPUT_KEYBOARD
            inp_up.u.ki = KEYBDINPUT(
                wVk=0,
                wScan=ord(char),
                dwFlags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP,
                time=0,
                dwExtraInfo=None,
            )
            send_input_single(inp_up)
            time.sleep(0.005)
