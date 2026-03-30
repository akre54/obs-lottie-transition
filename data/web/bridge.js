// Bridge.js - Self-driving Lottie transition renderer
// Channel-packing: MatteA→R, MatteB→G, Overlay luminance→B, Overlay alpha→A
// All rendered into a single 1920x1080 canvas.

(function() {
  var cfg = window._obsConfig || {};
  var params = {};
  try { params = new URLSearchParams(window.location.search); } catch(e) {}
  const WIDTH = cfg.width || parseInt(params.get && params.get('width')) || 1920;
  const HEIGHT = cfg.height || parseInt(params.get && params.get('height')) || 1080;

  const canvas = document.getElementById('lottie-canvas');
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const ctx = canvas.getContext('2d');

  function paintStatus(r, g, b) {
    ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
    ctx.fillRect(0, 0, 20, 20);
  }

  // Stage 1: bridge.js started
  ctx.fillStyle = 'rgb(255,0,0)';
  ctx.fillRect(0, 0, WIDTH, HEIGHT);
  paintStatus(255, 0, 0);

  var instances = { matteA: null, matteB: null, overlay: null };
  var canvases = { matteA: null, matteB: null, overlay: null };
  var slotA = null, slotB = null;
  var hasSlots = false;
  var isPlaying = false;
  var startTime = 0;
  var durationMs = 0;
  var totalFrames = 0;
  var frameRate = 30;
  var loadedCount = 0;

  function filterLayers(jsonData, keepFn) {
    var copy = JSON.parse(JSON.stringify(jsonData));
    copy.layers = copy.layers.filter(function(layer) {
      return keepFn(layer.nm || '');
    });
    return copy;
  }

  function createInstance(name, animData) {
    var container = document.createElement('div');
    container.style.cssText = 'position:absolute;left:-9999px;width:' + WIDTH + 'px;height:' + HEIGHT + 'px;overflow:hidden';
    document.body.appendChild(container);

    var inst = lottie.loadAnimation({
      container: container,
      renderer: 'canvas',
      rendererSettings: { clearCanvas: true, preserveAspectRatio: 'xMidYMid slice' },
      loop: false,
      autoplay: false,
      animationData: animData
    });

    inst.addEventListener('DOMLoaded', function() {
      var cv = inst.renderer.canvasContext.canvas;
      canvases[name] = cv;
      console.error('[bridge] ' + name + ' DOMLoaded, canvas=' + cv.width + 'x' + cv.height + ' layers=' + animData.layers.length);

      if (name === 'matteA') {
        var elements = inst.renderer.elements;
        for (var i = 0; i < elements.length; i++) {
          var el = elements[i];
          if (!el || !el.data) continue;
          var nm = el.data.nm || '';
          if (nm === '[SlotA]') slotA = el;
          else if (nm === '[SlotB]') slotB = el;
        }
        hasSlots = !!(slotA || slotB);
      }

      loadedCount++;
      if (loadedCount >= 3) onAllLoaded();
    });

    inst.addEventListener('error', function() {
      console.error('[bridge] ' + name + ' error!');
      paintStatus(255, 165, 0);
    });

    instances[name] = inst;
  }

  function loadAnimationData(jsonData) {
    for (var key in instances) {
      if (instances[key]) { instances[key].destroy(); instances[key] = null; }
    }
    slotA = slotB = null;
    hasSlots = false;
    loadedCount = 0;
    totalFrames = jsonData.op - jsonData.ip;
    frameRate = jsonData.fr || 30;
    durationMs = (totalFrames / frameRate) * 1000;

    var matteAData = filterLayers(jsonData, function(nm) {
      return nm === '[MatteA]' || nm === '[SlotA]' || nm === '[SlotB]';
    });
    var matteBData = filterLayers(jsonData, function(nm) {
      return nm === '[MatteB]';
    });
    var overlayData = filterLayers(jsonData, function(nm) {
      return nm !== '[MatteA]' && nm !== '[MatteB]' && nm !== '[SlotA]' && nm !== '[SlotB]';
    });

    console.error('[bridge] Creating 3 instances: matteA=' + matteAData.layers.length +
      ' matteB=' + matteBData.layers.length + ' overlay=' + overlayData.layers.length + ' layers');

    paintStatus(255, 255, 0);
    createInstance('matteA', matteAData);
    createInstance('matteB', matteBData);
    createInstance('overlay', overlayData);
  }

  function onAllLoaded() {
    paintStatus(0, 255, 255);
    console.error('[bridge] All 3 instances loaded. frames=' + totalFrames +
      ' fps=' + frameRate + ' duration=' + durationMs + 'ms slots=' + hasSlots);

    try {
      seekAndRender(0);
      paintStatus(255, 0, 255);
    } catch(e) {
      console.error('[bridge] seekAndRender error: ' + e);
      paintStatus(255, 165, 0);
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

  // Encode slot transforms into bottom 2 pixel rows
  function encodeDataStrip(imageData) {
    var transformA = extractTransform(slotA);
    var transformB = extractTransform(slotB);
    var data = imageData.data;
    var w = imageData.width;
    var ranges = [[-4096,4096],[-4096,4096],[0,10],[0,10],[-360,360],[0,1]];
    var stripRow = (HEIGHT - 1) * w * 4; // last row

    function encodeSlot(transform, pixelOffset) {
      var values = [transform.pos_x, transform.pos_y, transform.scale_x, transform.scale_y, transform.rotation, transform.opacity];
      for (var i = 0; i < 6; i += 2) {
        var px = pixelOffset + (i / 2);
        var idx = stripRow + px * 4;
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

    // Magic marker at pixel 6
    var magicIdx = stripRow + 6 * 4;
    data[magicIdx] = 0xCA; data[magicIdx+1] = 0xFE; data[magicIdx+2] = 0xBA; data[magicIdx+3] = 0xBE;
  }

  var _logCount = 0;
  function seekAndRender(frame) {
    // Advance all 3 lottie instances to the target frame
    var names = ['matteA', 'matteB', 'overlay'];
    for (var i = 0; i < 3; i++) {
      var inst = instances[names[i]];
      if (inst && inst.isLoaded) inst.goToAndStop(frame, true);
    }

    // Read pixels from each lottie canvas
    var cvA = canvases.matteA, cvB = canvases.matteB, cvO = canvases.overlay;
    if (!cvA || !cvB || !cvO) return;

    var ctxA = cvA.getContext('2d');
    var ctxB = cvB.getContext('2d');
    var ctxO = cvO.getContext('2d');

    var pixA = ctxA.getImageData(0, 0, WIDTH, HEIGHT);
    var pixB = ctxB.getImageData(0, 0, WIDTH, HEIGHT);
    var pixO = ctxO.getImageData(0, 0, WIDTH, HEIGHT);

    // Channel-pack into output: R=matteA, G=matteB, B=overlay_luminance, A=overlay_alpha
    var output = ctx.createImageData(WIDTH, HEIGHT);
    var out = output.data;
    var dA = pixA.data, dB = pixB.data, dO = pixO.data;
    var len = WIDTH * HEIGHT * 4;

    for (var p = 0; p < len; p += 4) {
      // MatteA: premultiplied luminance (white shape on transparent = alpha IS the matte)
      var mA_a = dA[p+3];
      var mA_lum = mA_a > 0 ? Math.round((dA[p]*0.299 + dA[p+1]*0.587 + dA[p+2]*0.114) * mA_a / 255) : 0;

      var mB_a = dB[p+3];
      var mB_lum = mB_a > 0 ? Math.round((dB[p]*0.299 + dB[p+1]*0.587 + dB[p+2]*0.114) * mB_a / 255) : 0;

      // Overlay alpha
      var ov_a = dO[p+3];

      out[p+0] = mA_lum;   // R = matteA
      out[p+1] = mB_lum;   // G = matteB
      out[p+2] = ov_a;     // B = overlay alpha (blend factor)
      out[p+3] = 255;      // A = always opaque (browser composites away alpha=0 pixels)
    }

    // Encode slot transforms into bottom row if slots exist
    if (hasSlots) {
      encodeDataStrip(output);
    }

    ctx.putImageData(output, 0, 0);

    _logCount++;
    if (_logCount <= 6) {
      var cx = Math.floor(WIDTH/2), cy = Math.floor(HEIGHT/2);
      var idx = (cy * WIDTH + cx) * 4;
      console.error('[bridge] frame=' + frame.toFixed(1) +
        ' center: R(matteA)=' + out[idx] + ' G(matteB)=' + out[idx+1] +
        ' B(ov_alpha)=' + out[idx+2] + ' A=' + out[idx+3]);
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
    if (!isPlaying) return;

    var elapsed = timestamp - startTime;
    var loopElapsed = elapsed % durationMs;
    var progress = loopElapsed / durationMs;
    var frame = progress * (totalFrames - 1);

    seekAndRender(frame);
    paintStatus(0, 255, 0);

    requestAnimationFrame(playbackLoop);
  }

  // Public API
  window.seekFrame = function(frame) { seekAndRender(frame); };
  window.transitionStart = function() { startPlayback(); };
  window.transitionStop = function() {
    isPlaying = false;
    ctx.clearRect(0, 0, WIDTH, HEIGHT);
  };
  window.loadLottieData = function(jsonString) {
    try { loadAnimationData(JSON.parse(jsonString)); } catch(e) { console.error('[bridge] Parse error:', e); }
  };

  function tryStart() {
    if (typeof lottie === 'undefined') {
      paintStatus(255, 255, 0);
      return;
    }
    paintStatus(0, 128, 255);
    if (window._lottieData) {
      loadAnimationData(window._lottieData);
    }
  }

  if (typeof lottie !== 'undefined') {
    tryStart();
  } else {
    paintStatus(255, 255, 0);
    window._onLottieReady = tryStart;
  }
})();
