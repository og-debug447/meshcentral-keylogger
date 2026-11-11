'use strict';

// MeshCentral Keylogger plugin (mckeylog).
// The Keylogs tab (page 61) is added to the device pages by
// scripts/patch_meshcentral.js; this entry point keeps existing
// plugin lifecycle hooks working.
module.exports.mckeylog = function (parent) {
    return {
        parent: parent,
        exports: ['onDeviceRefreshEnd'],
        onDeviceRefreshEnd: function () { }
    };
};
