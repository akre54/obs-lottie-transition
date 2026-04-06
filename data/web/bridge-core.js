(function(root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
    return;
  }

  root.BridgeCore = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';

  function filterLayers(jsonData, keepFn) {
    var copy = JSON.parse(JSON.stringify(jsonData));
    copy.layers = copy.layers.filter(function(layer) {
      return keepFn(layer.nm || '');
    });
    return copy;
  }

  function extractTransform(element) {
    if (!element) {
      return {
        pos_x: 0,
        pos_y: 0,
        scale_x: 1,
        scale_y: 1,
        rotation: 0,
        opacity: 1
      };
    }

    var mat = element.finalTransform.mat.props;
    return {
      pos_x: mat[12],
      pos_y: mat[13],
      scale_x: Math.sqrt(mat[0] * mat[0] + mat[1] * mat[1]),
      scale_y: Math.sqrt(mat[4] * mat[4] + mat[5] * mat[5]),
      rotation: Math.atan2(mat[1], mat[0]) * (180 / Math.PI),
      opacity: (function() {
        try {
          return element.finalTransform.mProp.o.v / 100;
        } catch (e) {
          return 1;
        }
      })()
    };
  }

  function encodeFloat(value, min, max) {
    var normalized = (value - min) / (max - min);
    return Math.round(Math.max(0, Math.min(1, normalized)) * 65535);
  }

  function encodeDataStrip(imageData, width, height, transformA, transformB) {
    var data = imageData.data;
    var ranges = [[-4096, 4096], [-4096, 4096], [0, 10], [0, 10], [-360, 360], [0, 1]];
    var stripRow = (height - 1) * width * 4;

    function encodeSlot(transform, pixelOffset) {
      var values = [
        transform.pos_x,
        transform.pos_y,
        transform.scale_x,
        transform.scale_y,
        transform.rotation,
        transform.opacity
      ];

      for (var i = 0; i < 6; i += 2) {
        var px = pixelOffset + (i / 2);
        var idx = stripRow + px * 4;
        var enc1 = encodeFloat(values[i], ranges[i][0], ranges[i][1]);
        var enc2 = encodeFloat(values[i + 1], ranges[i + 1][0], ranges[i + 1][1]);

        data[idx + 0] = (enc1 >> 8) & 0xFF;
        data[idx + 1] = enc1 & 0xFF;
        data[idx + 2] = (enc2 >> 8) & 0xFF;
        data[idx + 3] = enc2 & 0xFF;
      }
    }

    encodeSlot(transformA, 0);
    encodeSlot(transformB, 3);

    var magicIdx = stripRow + 6 * 4;
    data[magicIdx] = 0xCA;
    data[magicIdx + 1] = 0xFE;
    data[magicIdx + 2] = 0xBA;
    data[magicIdx + 3] = 0xBE;
  }

  function packFrameData(matteAData, matteBData, overlayData, outputData) {
    for (var p = 0; p < outputData.length; p += 4) {
      var matteALuma = Math.round(
        matteAData[p] * 0.299 +
        matteAData[p + 1] * 0.587 +
        matteAData[p + 2] * 0.114
      );

      var matteBLuma = Math.round(
        matteBData[p] * 0.299 +
        matteBData[p + 1] * 0.587 +
        matteBData[p + 2] * 0.114
      );

      outputData[p + 0] = matteALuma;
      outputData[p + 1] = matteBLuma;
      outputData[p + 2] = overlayData[p + 3];
      outputData[p + 3] = 255;
    }
  }

  return {
    encodeDataStrip: encodeDataStrip,
    encodeFloat: encodeFloat,
    extractTransform: extractTransform,
    filterLayers: filterLayers,
    packFrameData: packFrameData
  };
});
