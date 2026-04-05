(function(root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory(require('./bridge-core'));
    return;
  }

  root.BackendPlan = factory(root.BridgeCore);
})(typeof globalThis !== 'undefined' ? globalThis : this, function(BridgeCore) {
  'use strict';

  function normalizeBackend(name) {
    return name === 'thorvg' ? 'thorvg' : 'browser';
  }

  function reservedLayer(name) {
    return name === '[MatteA]' ||
      name === '[MatteB]' ||
      name === '[SlotA]' ||
      name === '[SlotB]';
  }

  function buildLayerPlan(jsonData, backendName) {
    var backend = normalizeBackend(backendName);

    if (backend === 'thorvg') {
      return {
        backend: backend,
        matteA: BridgeCore.filterLayers(jsonData, function(name) {
          return name === '[MatteA]';
        }),
        matteB: BridgeCore.filterLayers(jsonData, function(name) {
          return name === '[MatteB]';
        }),
        overlay: BridgeCore.filterLayers(jsonData, function(name) {
          return !reservedLayer(name);
        }),
        slots: BridgeCore.filterLayers(jsonData, function(name) {
          return name === '[SlotA]' || name === '[SlotB]';
        }),
        usesBrowserSource: false,
        usesCanvasPacking: false
      };
    }

    return {
      backend: backend,
      matteA: BridgeCore.filterLayers(jsonData, function(name) {
        return name === '[MatteA]' || name === '[SlotA]' || name === '[SlotB]';
      }),
      matteB: BridgeCore.filterLayers(jsonData, function(name) {
        return name === '[MatteB]';
      }),
      overlay: BridgeCore.filterLayers(jsonData, function(name) {
        return !reservedLayer(name);
      }),
      slots: BridgeCore.filterLayers(jsonData, function(name) {
        return name === '[SlotA]' || name === '[SlotB]';
      }),
      usesBrowserSource: true,
      usesCanvasPacking: true
    };
  }

  return {
    buildLayerPlan: buildLayerPlan,
    normalizeBackend: normalizeBackend
  };
});
