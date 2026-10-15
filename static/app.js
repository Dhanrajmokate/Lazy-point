(() => {
  // Elements
  const statusBadge = document.getElementById('statusBadge');
  const statusText = document.getElementById('statusText');
  const trackpad = document.getElementById('trackpad');
  const trackpadHint = document.getElementById('trackpadHint');
  const touchPointer = document.getElementById('touchPointer');
  const settingsToggle = document.getElementById('settingsToggle');
  const settingsDrawer = document.getElementById('settingsDrawer');
  const sensSlider = document.getElementById('sensSlider');
  const sensValue = document.getElementById('sensValue');
  const hapticToggle = document.getElementById('hapticToggle');

  // Media buttons
  const btnLeftClick = document.getElementById('btnLeftClick');
  const btnRightClick = document.getElementById('btnRightClick');
  const btnVolDown = document.getElementById('btnVolDown');
  const btnMute = document.getElementById('btnMute');
  const btnVolUp = document.getElementById('btnVolUp');
  const btnSeekBack = document.getElementById('btnSeekBack');
  const btnPlayPause = document.getElementById('btnPlayPause');
  const btnSeekFwd = document.getElementById('btnSeekFwd');
  const btnFullscreen = document.getElementById('btnFullscreen');
  const btnSubtitles = document.getElementById('btnSubtitles');
  const btnTextKeyboard = document.getElementById('btnTextKeyboard');
  const btnEsc = document.getElementById('btnEsc');

  // Modal elements
  const typeModal = document.getElementById('typeModal');
  const closeTypeModal = document.getElementById('closeTypeModal');
  const pcTextInput = document.getElementById('pcTextInput');
  const btnSendText = document.getElementById('btnSendText');
  const btnSendEnter = document.getElementById('btnSendEnter');
  const btnSendBackspace = document.getElementById('btnSendBackspace');

  // State
  let ws = null;
  let sensitivity = parseFloat(localStorage.getItem('lazy_sens') || '1.6');
  let hapticsEnabled = localStorage.getItem('lazy_haptics') !== 'false';
  let isMuted = false;

  sensSlider.value = sensitivity;
  sensValue.textContent = `${sensitivity}x`;
  hapticToggle.checked = hapticsEnabled;

  function vibrate(ms = 15) {
    if (hapticsEnabled && window.navigator && window.navigator.vibrate) {
      try { window.navigator.vibrate(ms); } catch (e) {}
    }
  }

  // Settings Handlers
  settingsToggle.addEventListener('click', () => {
    vibrate(10);
    settingsDrawer.classList.toggle('open');
  });

  sensSlider.addEventListener('input', (e) => {
    sensitivity = parseFloat(e.target.value);
    sensValue.textContent = `${sensitivity.toFixed(1)}x`;
    localStorage.setItem('lazy_sens', sensitivity);
  });

  hapticToggle.addEventListener('change', (e) => {
    hapticsEnabled = e.target.checked;
    localStorage.setItem('lazy_haptics', hapticsEnabled);
    if (hapticsEnabled) vibrate(20);
  });

  // WebSocket Connection
  function connectWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;

    statusText.textContent = 'Connecting...';
    statusBadge.classList.remove('connected');

    ws = new WebSocket(wsUrl);

    ws.onopen = () => {
      statusText.textContent = 'Connected';
      statusBadge.classList.add('connected');
      vibrate(30);
    };

    ws.onclose = () => {
      statusText.textContent = 'Disconnected';
      statusBadge.classList.remove('connected');
      // Retry in 2 seconds
      setTimeout(connectWebSocket, 2000);
    };

    ws.onerror = () => {
      if (ws) ws.close();
    };
  }

  function send(data) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(data));
    }
  }

  // Touchpad Gesture Tracking
  let touchStartTime = 0;
  let startX = 0;
  let startY = 0;
  let lastX = 0;
  let lastY = 0;
  let hasMoved = false;
  let touchCount = 1;

  // Scroll tracking for 2 fingers
  let lastTwoFingerY = 0;

  trackpad.addEventListener('touchstart', (e) => {
    touchCount = e.touches.length;
    touchStartTime = Date.now();
    hasMoved = false;

    if (touchCount === 1) {
      const touch = e.touches[0];
      const rect = trackpad.getBoundingClientRect();
      startX = touch.clientX;
      startY = touch.clientY;
      lastX = touch.clientX;
      lastY = touch.clientY;

      // Position visual pointer
      touchPointer.style.left = `${touch.clientX - rect.left}px`;
      touchPointer.style.top = `${touch.clientY - rect.top}px`;
      touchPointer.classList.add('active');
      trackpadHint.style.opacity = '0';
    } else if (touchCount === 2) {
      lastTwoFingerY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
      touchPointer.classList.remove('active');
    }
  }, { passive: false });

  trackpad.addEventListener('touchmove', (e) => {
    e.preventDefault(); // Prevent scrolling the web page
    const currentTouches = e.touches.length;

    if (currentTouches === 1) {
      const touch = e.touches[0];
      const rect = trackpad.getBoundingClientRect();

      const dx = touch.clientX - lastX;
      const dy = touch.clientY - lastY;

      // Update pointer visual
      touchPointer.style.left = `${touch.clientX - rect.left}px`;
      touchPointer.style.top = `${touch.clientY - rect.top}px`;

      if (Math.abs(touch.clientX - startX) > 5 || Math.abs(touch.clientY - startY) > 5) {
        hasMoved = true;
      }

      lastX = touch.clientX;
      lastY = touch.clientY;

      send({
        type: 'move',
        dx: dx,
        dy: dy,
        sensitivity: sensitivity
      });
    } else if (currentTouches === 2) {
      // 2 finger scrolling
      const currentTwoFingerY = (e.touches[0].clientY + e.touches[1].clientY) / 2;
      const diffY = currentTwoFingerY - lastTwoFingerY;

      if (Math.abs(diffY) > 6) {
        hasMoved = true;
        send({
          type: 'scroll',
          delta_y: diffY > 0 ? 1 : -1
        });
        lastTwoFingerY = currentTwoFingerY;
      }
    }
  }, { passive: false });

  trackpad.addEventListener('touchend', (e) => {
    touchPointer.classList.remove('active');
    const touchDuration = Date.now() - touchStartTime;

    // Detect Tap (short duration, minimal movement)
    if (!hasMoved && touchDuration < 250) {
      if (touchCount === 1) {
        // Single finger tap = Left Click
        vibrate(20);
        send({ type: 'click', button: 'left' });
      } else if (touchCount === 2) {
        // Two finger tap = Right Click
        vibrate(35);
        send({ type: 'click', button: 'right' });
      }
    }

    if (e.touches.length === 0) {
      setTimeout(() => {
        if (!touchPointer.classList.contains('active')) {
          trackpadHint.style.opacity = '1';
        }
      }, 1500);
    }
  });

  // Physical Button Listeners
  btnLeftClick.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'click', button: 'left' });
  });

  btnRightClick.addEventListener('click', () => {
    vibrate(30);
    send({ type: 'click', button: 'right' });
  });

  // Volume Controls
  btnVolDown.addEventListener('click', () => {
    vibrate(15);
    send({ type: 'volume', action: 'down' });
  });

  btnVolUp.addEventListener('click', () => {
    vibrate(15);
    send({ type: 'volume', action: 'up' });
  });

  btnMute.addEventListener('click', () => {
    vibrate(25);
    isMuted = !isMuted;
    document.getElementById('muteIcon').textContent = isMuted ? '🔊' : '🔇';
    send({ type: 'volume', action: 'mute' });
  });

  // Playback Controls
  btnPlayPause.addEventListener('click', () => {
    vibrate(30);
    send({ type: 'media', action: 'playpause' });
  });

  btnSeekBack.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'media', action: 'seek_bwd' });
  });

  btnSeekFwd.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'media', action: 'seek_fwd' });
  });

  // Shortcuts
  btnFullscreen.addEventListener('click', () => {
    vibrate(25);
    send({ type: 'media', action: 'fullscreen' });
  });

  btnSubtitles.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'key', key: 'c' }); // 'c' toggles subtitles in YouTube / VLC
  });

  btnEsc.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'key', key: 'esc' });
  });

  // Text Drawer
  btnTextKeyboard.addEventListener('click', () => {
    vibrate(15);
    typeModal.classList.add('open');
    setTimeout(() => pcTextInput.focus(), 100);
  });

  closeTypeModal.addEventListener('click', () => {
    typeModal.classList.remove('open');
  });

  typeModal.addEventListener('click', (e) => {
    if (e.target === typeModal) {
      typeModal.classList.remove('open');
    }
  });

  btnSendText.addEventListener('click', () => {
    const text = pcTextInput.value;
    if (text) {
      vibrate(25);
      send({ type: 'type', text: text });
      pcTextInput.value = '';
      typeModal.classList.remove('open');
    }
  });

  btnSendEnter.addEventListener('click', () => {
    vibrate(20);
    send({ type: 'key', key: 'enter' });
  });

  btnSendBackspace.addEventListener('click', () => {
    vibrate(15);
    send({ type: 'key', key: 'backspace' });
  });

  pcTextInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      btnSendText.click();
    }
  });

  // Connect on load
  connectWebSocket();
})();
