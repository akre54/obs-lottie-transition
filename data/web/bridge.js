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

    lottieCanvas = document.createElement('canvas');
    lottieCanvas.width = WIDTH;
    lottieCanvas.height = HEIGHT;

    const container = document.createElement('div');
    container.style.cssText = 'position:absolute;width:0;height:0;overflow:hidden';
    document.body.appendChild(container);

    animInstance = lottie.loadAnimation({
      container: container,
      renderer: 'canvas',
      rendererSettings: {
        canvas: lottieCanvas,
        clearCanvas: true,
        context: '2d',
        preserveAspectRatio: 'xMidYMid slice'
      },
      loop: false,
      autoplay: false,
      animationData: jsonData
    });

    animInstance.addEventListener('DOMLoaded', onAnimLoaded);
  }

  function onAnimLoaded() {
    var elements = animInstance.renderer.elements;
    for (var i = 0; i < elements.length; i++) {
      var el = elements[i];
      if (!el || !el.data) continue;
      var name = el.data.nm || '';
      if (name === '[SlotA]') { slotA = el; el.hide(); }
      else if (name === '[SlotB]') { slotB = el; el.hide(); }
      else if (name === '[MatteA]') { matteA = el; }
      else if (name === '[MatteB]') { matteB = el; }
    }
    hasSlots = !!(slotA || slotB);
    hasMattes = !!(matteA || matteB);

    durationMs = (animInstance.totalFrames / animInstance.frameRate) * 1000;

    console.log('[bridge] Animation loaded. slots:', hasSlots, 'mattes:', hasMattes,
      'frames:', animInstance.totalFrames, 'fps:', animInstance.frameRate,
      'duration:', durationMs, 'ms');

    // Auto-start: render first frame, then begin playback loop
    seekAndRender(0);
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

  function renderLottieWithLayers(visibleLayers, targetY) {
    if (!animInstance || !animInstance.renderer) return;
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
    ctx.drawImage(lottieCanvas, 0, 0, WIDTH, HEIGHT, 0, targetY, WIDTH, HEIGHT);
    for (var j = 0; j < savedVisibility.length; j++) {
      if (savedVisibility[j].hidden) savedVisibility[j].el.hide();
      else savedVisibility[j].el.show();
    }
  }

  function seekAndRender(frame) {
    if (!animInstance || !animInstance.isLoaded) return;
    animInstance.goToAndStop(frame, true);
    ctx.clearRect(0, 0, WIDTH, CANVAS_HEIGHT);

    if (matteA) {
      renderLottieWithLayers(['[MatteA]'], 0);
    } else {
      ctx.fillStyle = 'white';
      ctx.fillRect(0, 0, WIDTH, HEIGHT);
    }

    if (matteB) {
      renderLottieWithLayers(['[MatteB]'], HEIGHT);
    } else {
      ctx.fillStyle = 'black';
      ctx.fillRect(0, HEIGHT, WIDTH, HEIGHT);
    }

    renderLottieWithLayers(['decorative'], HEIGHT * 2);

    if (hasSlots) {
      encodeDataStrip();
    }
  }

  function startPlayback() {
    if (isPlaying) return;
    isPlaying = true;
    startTime = performance.now();
    console.log('[bridge] Starting auto-playback');
    requestAnimationFrame(playbackLoop);
  }

  function playbackLoop(timestamp) {
    if (!isPlaying || !animInstance || !animInstance.isLoaded) return;

    var elapsed = timestamp - startTime;
    var progress = Math.min(elapsed / durationMs, 1.0);
    var frame = progress * (animInstance.totalFrames - 1);

    seekAndRender(frame);

    if (progress < 1.0) {
      requestAnimationFrame(playbackLoop);
    } else {
      // Hold on last frame
      isPlaying = false;
      console.log('[bridge] Playback complete');
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

  // Auto-load from _lottieData (set by <script src> before bridge.js)
  if (window._lottieData) {
    console.log('[bridge] Found _lottieData, loading animation...');
    loadAnimationData(window._lottieData);
  } else if (window._pendingLottieData) {
    console.log('[bridge] Found _pendingLottieData, loading...');
    window.loadLottieData(window._pendingLottieData);
    delete window._pendingLottieData;
  } else {
    console.log('[bridge] No lottie data found, waiting for loadLottieData()');
  }

  console.log('[bridge] Bridge initialized (self-driving mode)');
})();
