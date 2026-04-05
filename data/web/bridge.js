// Bridge.js - Self-driving Lottie transition renderer
// Channel-packing: MatteA→R, MatteB→G, Overlay luminance→B, Overlay alpha→A
// All rendered into a single 1920x1080 canvas.

(function() {
  var BridgeCore = window.BridgeCore;
  var BackendPlan = window.BackendPlan;
  var cfg = window._obsConfig || {};
  var params = {};
  try { params = new URLSearchParams(window.location.search); } catch(e) {}
  const WIDTH = cfg.width || parseInt(params.get && params.get('width')) || 1920;
  const HEIGHT = cfg.height || parseInt(params.get && params.get('height')) || 1080;

  const canvas = document.getElementById('lottie-canvas');
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const ctx = canvas.getContext('2d');

  // Stage 1: bridge.js started — fill red so shader sees matteA=255 (scene A)
  ctx.fillStyle = 'rgb(255,0,0)';
  ctx.fillRect(0, 0, WIDTH, HEIGHT);

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

    var plan = BackendPlan.buildLayerPlan(jsonData, 'browser');
    var matteAData = plan.matteA;
    var matteBData = plan.matteB;
    var overlayData = plan.overlay;

    console.error('[bridge] Creating 3 instances: matteA=' + matteAData.layers.length +
      ' matteB=' + matteBData.layers.length + ' overlay=' + overlayData.layers.length + ' layers');

    createInstance('matteA', matteAData);
    createInstance('matteB', matteBData);
    createInstance('overlay', overlayData);
  }

  function onAllLoaded() {
    console.error('[bridge] All 3 instances loaded. frames=' + totalFrames +
      ' fps=' + frameRate + ' duration=' + durationMs + 'ms slots=' + hasSlots);

    try {
      seekAndRender(0);
    } catch(e) {
      console.error('[bridge] seekAndRender error: ' + e);
      return;
    }
    startPlayback();
  }

  function encodeDataStrip(imageData) {
    BridgeCore.encodeDataStrip(
      imageData,
      imageData.width,
      HEIGHT,
      BridgeCore.extractTransform(slotA),
      BridgeCore.extractTransform(slotB)
    );
  }

  var _logCount = 0;
  function seekAndRender(frame) {
    // Clamp frame to valid range — negative frames clear lottie canvases
    if (frame < 0) frame = 0;
    if (frame > totalFrames - 1) frame = totalFrames - 1;

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

    BridgeCore.packFrameData(dA, dB, dO, out);

    // Encode slot transforms into bottom row if slots exist
    if (hasSlots) {
      encodeDataStrip(output);
    }

    ctx.putImageData(output, 0, 0);

    _logCount++;
    if (_logCount <= 30) {
      // Sample at 25% from top-left edge — more useful than center for circle wipes
      var sx = Math.floor(WIDTH * 0.25), sy = Math.floor(HEIGHT * 0.25);
      var idx = (sy * WIDTH + sx) * 4;
      // Also sample center
      var cx = Math.floor(WIDTH/2), cy = Math.floor(HEIGHT/2);
      var cidx = (cy * WIDTH + cx) * 4;
      console.error('[bridge] frame=' + frame.toFixed(1) +
        ' edge(25%): R=' + out[idx] + ' G=' + out[idx+1] + ' B=' + out[idx+2] +
        ' center: R=' + out[cidx] + ' G=' + out[cidx+1] + ' B=' + out[cidx+2]);
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
    if (window._lottieData) {
      loadAnimationData(window._lottieData);
    }
  }

  if (typeof lottie !== 'undefined') {
    tryStart();
  } else {
    window._onLottieReady = tryStart;
  }
})();
