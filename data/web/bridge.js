// Bridge.js - Self-driving Lottie transition renderer
// Since exec_browser_js doesn't work for private browser sources,
// the animation auto-plays from page load using requestAnimationFrame.

(function() {
  var cfg = window._obsConfig || {};
  var params = {};
  try { params = new URLSearchParams(window.location.search); } catch(e) {}
  const WIDTH = cfg.width || parseInt(params.get && params.get('width')) || 1920;
  const HEIGHT = cfg.height || parseInt(params.get && params.get('height')) || 1080;
  const DATA_STRIP_HEIGHT = cfg.dataStripHeight || parseInt(params.get && params.get('dataStripHeight')) || 2;
  const CANVAS_HEIGHT = HEIGHT * 3 + DATA_STRIP_HEIGHT;

  const canvas = document.getElementById('lottie-canvas');
  canvas.width = WIDTH;
  canvas.height = CANVAS_HEIGHT;
  const ctx = canvas.getContext('2d');

  // DIAGNOSTIC: Paint status flags as pixel colors at (0,0).
  // Each stage overwrites with a new color so C can read the last-reached stage.
  // Stage 1 (bridge init): Red
  // Stage 2 (lottie loaded): Yellow
  // Stage 3 (anim DOMLoaded): Cyan
  // Stage 4 (seekAndRender ok): Magenta
  // Stage 5 (playback running): White
  // Error: Orange (255,165,0)
  function paintStatus(r, g, b) {
    // Paint a 4px wide status bar at the very top-left of the canvas
    ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
    ctx.fillRect(0, 0, 20, 20);
  }

  // Stage 1: bridge.js started
  ctx.fillStyle = 'red';
  ctx.fillRect(0, 0, WIDTH, CANVAS_HEIGHT);
  paintStatus(255, 0, 0);

  // Lottie loads async via dynamic <script> — we need to wait for it

  let slotA = null, slotB = null, matteA = null, matteB = null;
  let animInstance = null;
  let hasSlots = false, hasMattes = false;
  let lottieCanvas = null;
  let isPlaying = false;
  let startTime = 0;
  let durationMs = 0;

  function loadAnimationData(jsonData) {
    if (animInstance) {
      animInstance.destroy();
      animInstance = null;
    }
    slotA = slotB = matteA = matteB = null;
    hasSlots = hasMattes = false;

    // Container must have real dimensions — lottie creates its own canvas inside it
    const container = document.createElement('div');
    container.style.cssText = 'position:absolute;left:-9999px;width:' + WIDTH + 'px;height:' + HEIGHT + 'px;overflow:hidden';
    document.body.appendChild(container);

    animInstance = lottie.loadAnimation({
      container: container,
      renderer: 'canvas',
      rendererSettings: {
        clearCanvas: true,
        preserveAspectRatio: 'xMidYMid slice'
      },
      loop: false,
      autoplay: false,
      animationData: jsonData
    });

    // Stage 2: lottie.loadAnimation called
    paintStatus(255, 255, 0);
    animInstance.addEventListener('DOMLoaded', onAnimLoaded);
    animInstance.addEventListener('error', function() {
      ctx.fillStyle = 'orange';
      ctx.fillRect(0, 0, WIDTH, CANVAS_HEIGHT);
    });
  }

  function onAnimLoaded() {
    // Stage 3: DOMLoaded
    paintStatus(0, 255, 255);

    // Grab the canvas that lottie-web actually created inside the container
    lottieCanvas = animInstance.renderer.canvasContext.canvas;
    console.error('[bridge] lottieCanvas from renderer: ' + lottieCanvas.width + 'x' + lottieCanvas.height);

    var elements = animInstance.renderer.elements;
    console.error('[bridge] elements count:', elements ? elements.length : 'NULL');
    for (var i = 0; i < elements.length; i++) {
      var el = elements[i];
      if (!el || !el.data) continue;
      var name = el.data.nm || '';
      console.error('[bridge] element[' + i + '] name="' + name + '" type=' + el.data.ty);
      if (name === '[SlotA]') { slotA = el; el.hide(); }
      else if (name === '[SlotB]') { slotB = el; el.hide(); }
      else if (name === '[MatteA]') { matteA = el; }
      else if (name === '[MatteB]') { matteB = el; }
    }
    hasSlots = !!(slotA || slotB);
    hasMattes = !!(matteA || matteB);

    durationMs = (animInstance.totalFrames / animInstance.frameRate) * 1000;

    console.error('[bridge] Animation loaded. slots:', hasSlots, 'mattes:', hasMattes,
      'frames:', animInstance.totalFrames, 'fps:', animInstance.frameRate,
      'duration:', durationMs, 'ms');

    // Auto-start: render first frame, then begin playback loop
    try {
      seekAndRender(0);
      // Stage 4: seekAndRender completed
      paintStatus(255, 0, 255);
    } catch(e) {
      ctx.fillStyle = 'orange';
      ctx.fillRect(0, 0, WIDTH, CANVAS_HEIGHT);
      return;
    }
    startPlayback();
  }

  function extractTransform(element) {
    if (!element) return { pos_x: 0, pos_y: 0, scale_x: 1, scale_y: 1, rotation: 0, opacity: 1 };
    var mat = element.finalTransform.mat.props;
    return {
      pos_x: mat[12], pos_y: mat[13],
      scale_x: Math.sqrt(mat[0]*mat[0] + mat[1]*mat[1]),
      scale_y: Math.sqrt(mat[4]*mat[4] + mat[5]*mat[5]),
      rotation: Math.atan2(mat[1], mat[0]) * (180 / Math.PI),
      opacity: (function() { try { return element.finalTransform.mProp.o.v / 100; } catch(e) { return 1; } })()
    };
  }

  function encodeFloat(value, min, max) {
    var normalized = (value - min) / (max - min);
    return Math.round(Math.max(0, Math.min(1, normalized)) * 65535);
  }

  function encodeDataStrip() {
    var transformA = extractTransform(slotA);
    var transformB = extractTransform(slotB);
    var imageData = ctx.createImageData(WIDTH, DATA_STRIP_HEIGHT);
    var data = imageData.data;
    var ranges = [[-4096,4096],[-4096,4096],[0,10],[0,10],[-360,360],[0,1]];

    function encodeSlot(transform, pixelOffset) {
      var values = [transform.pos_x, transform.pos_y, transform.scale_x, transform.scale_y, transform.rotation, transform.opacity];
      for (var i = 0; i < 6; i += 2) {
        var px = pixelOffset + (i / 2);
        var idx = px * 4;
        var enc1 = encodeFloat(values[i], ranges[i][0], ranges[i][1]);
        var enc2 = encodeFloat(values[i+1], ranges[i+1][0], ranges[i+1][1]);
        data[idx + 0] = (enc1 >> 8) & 0xFF;
        data[idx + 1] = enc1 & 0xFF;
        data[idx + 2] = (enc2 >> 8) & 0xFF;
        data[idx + 3] = enc2 & 0xFF;
      }
    }

    encodeSlot(transformA, 0);
    encodeSlot(transformB, 3);

    data[24] = 0xCA; data[25] = 0xFE; data[26] = 0xBA; data[27] = 0xBE;
    ctx.putImageData(imageData, 0, HEIGHT * 3);
  }

  var _renderCallCount = 0;
  function renderLottieWithLayers(visibleLayers, targetY) {
    if (!animInstance || !animInstance.renderer) {
      console.error('[bridge] renderLottie: no renderer!');
      return;
    }
    var elements = animInstance.renderer.elements;
    var savedVisibility = [];
    for (var i = 0; i < elements.length; i++) {
      var el = elements[i];
      if (!el || !el.data) continue;
      var name = el.data.nm || '';
      savedVisibility.push({ el: el, hidden: el.isInvisible });
      if (name === '[SlotA]' || name === '[SlotB]') {
        el.hide();
      } else if (name === '[MatteA]' || name === '[MatteB]') {
        if (visibleLayers.indexOf(name) >= 0) el.show();
        else el.hide();
      } else {
        if (visibleLayers.indexOf('decorative') >= 0) el.show();
        else el.hide();
      }
    }
    animInstance.renderer.renderFrame(animInstance.currentFrame);

    // Check if lottieCanvas has any non-transparent pixels
    _renderCallCount++;
    if (_renderCallCount <= 6) {
      var lctx = lottieCanvas.getContext('2d');
      var sample = lctx.getImageData(WIDTH/2, HEIGHT/2, 1, 1).data;
      console.error('[bridge] renderLottie(' + visibleLayers + ' → y=' + targetY + '): lottieCanvas center pixel RGBA(' + sample[0] + ',' + sample[1] + ',' + sample[2] + ',' + sample[3] + ')');
    }

    ctx.drawImage(lottieCanvas, 0, 0, WIDTH, HEIGHT, 0, targetY, WIDTH, HEIGHT);
    for (var j = 0; j < savedVisibility.length; j++) {
      if (savedVisibility[j].hidden) savedVisibility[j].el.hide();
      else savedVisibility[j].el.show();
    }
  }

  function seekAndRender(frame) {
    if (!animInstance || !animInstance.isLoaded) return;
    animInstance.goToAndStop(frame, true);
    // Clear canvas but preserve status pixel area (top-left 20x20)
    ctx.clearRect(20, 0, WIDTH - 20, CANVAS_HEIGHT);
    ctx.clearRect(0, 20, 20, CANVAS_HEIGHT - 20);

    // DIAGNOSTIC: Render all layers (no hide/show) to see if lottie outputs anything
    var allElements = animInstance.renderer.elements;
    for (var i = 0; i < allElements.length; i++) {
      if (allElements[i]) allElements[i].show();
    }
    animInstance.renderer.renderFrame(animInstance.currentFrame);

    // Use console.error (not console.log) — OBS only forwards error/fatal to logs
    _renderCallCount++;
    if (_renderCallCount <= 5) {
      var lctx = lottieCanvas.getContext('2d');
      var centerPx = lctx.getImageData(Math.floor(WIDTH/2), Math.floor(HEIGHT/2), 1, 1).data;
      var cornerPx = lctx.getImageData(100, 100, 1, 1).data;
      console.error('[bridge] lottieCanvas center=RGBA(' + centerPx[0] + ',' + centerPx[1] + ',' + centerPx[2] + ',' + centerPx[3] + ') corner=RGBA(' + cornerPx[0] + ',' + cornerPx[1] + ',' + cornerPx[2] + ',' + cornerPx[3] + ') size=' + lottieCanvas.width + 'x' + lottieCanvas.height);
      console.error('[bridge] frame=' + animInstance.currentFrame + '/' + animInstance.totalFrames + ' elems=' + (animInstance.renderer.elements ? animInstance.renderer.elements.length : 0) + ' canvasCtx=' + !!animInstance.renderer.canvasContext + ' loaded=' + animInstance.isLoaded);
      console.error('[bridge] renderer.canvas === lottieCanvas: ' + (animInstance.renderer.canvasContext === lctx));
      console.error('[bridge] renderer.renderConfig: ' + JSON.stringify(animInstance.renderer.renderConfig || {}));
    }

    // Copy all-layers render to all three regions for diagnostic
    ctx.drawImage(lottieCanvas, 0, 0, WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT);
    ctx.drawImage(lottieCanvas, 0, 0, WIDTH, HEIGHT, 0, HEIGHT, WIDTH, HEIGHT);
    ctx.drawImage(lottieCanvas, 0, 0, WIDTH, HEIGHT, 0, HEIGHT * 2, WIDTH, HEIGHT);

    if (hasSlots) {
      encodeDataStrip();
    }
  }

  function startPlayback() {
    if (isPlaying) return;
    isPlaying = true;
    startTime = performance.now();
    console.error('[bridge] Starting auto-playback');
    requestAnimationFrame(playbackLoop);
  }

  function playbackLoop(timestamp) {
    if (!isPlaying || !animInstance || !animInstance.isLoaded) return;

    var elapsed = timestamp - startTime;
    var progress = Math.min(elapsed / durationMs, 1.0);
    var frame = progress * (animInstance.totalFrames - 1);

    seekAndRender(frame);
    // Stage 5: playback running — paint green status
    paintStatus(0, 255, 0);

    if (progress < 1.0) {
      requestAnimationFrame(playbackLoop);
    } else {
      // Hold on last frame
      isPlaying = false;
      console.error('[bridge] Playback complete');
    }
  }

  // Public API (for exec_browser_js if it ever works)
  window.seekFrame = function(frame) { seekAndRender(frame); };
  window.transitionStart = function() {
    if (animInstance && animInstance.isLoaded) startPlayback();
  };
  window.transitionStop = function() {
    isPlaying = false;
    ctx.clearRect(0, 0, WIDTH, CANVAS_HEIGHT);
  };
  window.loadLottieData = function(jsonString) {
    try { loadAnimationData(JSON.parse(jsonString)); } catch(e) { console.error('[bridge] Parse error:', e); }
  };

  // Start animation once lottie library is available
  function tryStart() {
    if (typeof lottie === 'undefined') {
      // Still waiting for lottie — paint yellow status
      paintStatus(255, 255, 0);
      return;
    }
    // Stage 2: lottie available
    paintStatus(0, 128, 255);

    if (window._lottieData) {
      loadAnimationData(window._lottieData);
    }
  }

  // If lottie already loaded (sync script), start now
  if (typeof lottie !== 'undefined') {
    tryStart();
  } else {
    // Wait for async lottie load
    paintStatus(255, 255, 0);
    window._onLottieReady = tryStart;
  }
})();
