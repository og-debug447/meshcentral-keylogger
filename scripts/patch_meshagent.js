'use strict';
const fs = require('fs');
const path = require('path');
const sourceRoot = path.join(__dirname, '..');
const agentFiles = [
    ['agent/ILibDuktape_Keylogger.c', 'microscript/ILibDuktape_Keylogger.c'],
    ['agent/ILibDuktape_Keylogger.h', 'microscript/ILibDuktape_Keylogger.h']
];
const root = path.resolve(process.argv[2] || '');
if (!fs.existsSync(path.join(root, 'MeshAgent.sln')) || !fs.existsSync(path.join(root, 'microscript', 'ILibDuktape_ScriptContainer.c'))) throw new Error('Pass a MeshAgent checkout root');
for (const [from, to] of agentFiles) fs.copyFileSync(path.join(sourceRoot, from), path.join(root, to));

function edit(relativePath, transform) {
    const file = path.join(root, relativePath);
    const old = fs.readFileSync(file, 'utf8');
    const next = transform(old);
    if (old !== next) fs.writeFileSync(file, next, 'utf8');
}

edit(path.join('microscript', 'ILibDuktape_ScriptContainer.c'), function (source) {
    if (!source.includes('#include "ILibDuktape_Keylogger.h"')) {
        const marker = '#include "ILibDuktape_WebRTC.h"';
        if (!source.includes(marker)) throw new Error('ScriptContainer include anchor changed');
        source = source.replace(marker, marker + '\r\n#ifdef WIN32\r\n#include "ILibDuktape_Keylogger.h"\r\n#endif');
    }
    if (!source.includes('ILibDuktape_Keylogger_Init(ctx);')) {
        const webRtc = source.indexOf('ILibDuktape_WebRTC_Init(ctx);');
        const guard = source.lastIndexOf('#ifndef NO_WEBRTC', webRtc);
        if (webRtc < 0 || guard < 0) throw new Error('ScriptContainer module-init point changed');
        const newline = source.includes('\r\n') ? '\r\n' : '\n';
        const init = '#ifdef WIN32' + newline + '\t\tILibDuktape_Keylogger_Init(ctx);' + newline + '#endif' + newline;
        source = source.slice(0, guard) + init + source.slice(guard);
    }
    return source;
});

// Add the C module to every Windows build project (meshconsole + meshservice).
const projectDirs = ['meshconsole', 'meshservice'];
for (const dir of projectDirs) {
    for (const file of fs.readdirSync(path.join(root, dir)).filter((x) => x.endsWith('.vcxproj'))) {
        edit(path.join(dir, file), function (source) {
            if (!source.includes('..\\microscript\\ILibDuktape_Keylogger.c')) {
                const marker = '<ClCompile Include="..\\microscript\\ILibDuktape_WebRTC.c" />';
                if (!source.includes(marker)) throw new Error(file + ' compile item changed');
                source = source.replace(marker, marker + '\r\n    <ClCompile Include="..\\microscript\\ILibDuktape_Keylogger.c" />');
            }
            if (!source.includes('..\\microscript\\ILibDuktape_Keylogger.h')) {
                const marker = '<ClInclude Include="..\\microscript\\ILibDuktape_WebRTC.h" />';
                if (!source.includes(marker)) throw new Error(file + ' include item changed');
                source = source.replace(marker, marker + '\r\n    <ClInclude Include="..\\microscript\\ILibDuktape_Keylogger.h" />');
            }
            return source;
        });
    }
}

// Add the "keylog-worker" command branch so the service executable can be
// re-launched inside the interactive user's session (see
// ILibDuktape_Keylogger.c). Without this branch a session 0 agent has no
// way to capture the console user's keystrokes.
edit(path.join('meshservice', 'ServiceMain.c'), function (source) {
    if (source.includes('keylog-worker')) { return source; }
    const marker = 'if (argc > 1 && (strcasecmp(argv[1], "-finstall") == 0';
    const at = source.indexOf(marker);
    if (at < 0) throw new Error('ServiceMain.c -finstall anchor changed');
    const newline = source.includes('\r\n') ? '\r\n' : '\n';
    const branch = [
        '\t/* User-session keylog worker (see ILibDuktape_Keylogger.c). Runs the',
        '\t * keyboard hook inside the interactive user\'s session and sends the',
        '\t * formatted lines back to the agent over a named pipe. */',
        '\tif (argc > 1 && strcasecmp(argv[1], "keylog-worker") == 0)',
        '\t{',
        '\t\tretCode = ILibDuktape_KeyloggerWorker_Run(argc, argv);',
        '\t\targv[argc] = NULL; /* wmain_free releases argc+1 slots */',
        '\t\twmain_free(argv);',
        '\t\treturn retCode;',
        '\t}',
        '',
        ''
    ].join(newline);
    return source.slice(0, at) + branch + source.slice(at);
});

edit('makefile', function (source) {
    if (source.includes('microscript/ILibDuktape_Keylogger.c')) return source;
    const marker = 'SOURCES += microscript/ILibDuktape_ScriptContainer.c';
    const at = source.indexOf(marker);
    if (at < 0) throw new Error('MeshAgent makefile source list changed');
    const end = source.indexOf('\n', at);
    return source.slice(0, end) + ' microscript/ILibDuktape_Keylogger.c' + source.slice(end);
});
