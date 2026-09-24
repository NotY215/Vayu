// vscode-vayu/extension.js — dependency-free Vayu language client.
//
// Talks to `vls.exe` over stdio using the Language Server Protocol.
// Provides diagnostics, hover, goto-definition, completion, and commands
// that shell out to `vfmt.exe` and `vlint.exe`.
//
// No npm dependencies.  `require('vscode')` is provided by the editor.

const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');

let server = null;                  // { proc, pending: Map<id, {resolve,reject}> }
let nextId = 1;
let diagColl = null;
let outChannel = null;
let buffer = '';
let contentLength = -1;

function log(msg) {
    if (outChannel) outChannel.appendLine(msg);
}

function findBinary(configKey, baseName) {
    const ws = vscode.workspace.workspaceFolders?.[0]?.uri?.fsPath;
    const cfg = vscode.workspace.getConfiguration('vayu').get(configKey) || '';
    const suffix = process.platform === 'win32' ? '.exe' : '';
    if (cfg && cfg.length > 0) {
        if (path.isAbsolute(cfg)) return cfg;
        if (ws) return path.join(ws, cfg);
        return cfg;
    }
    if (ws) {
        const presets = ['x64-debug', 'x64-release', 'Debug', 'Release'];
        for (const p of presets) {
            const cand = path.join(ws, 'build', p, 'bin', baseName + suffix);
            if (fs.existsSync(cand)) return cand;
            const alt = path.join(ws, 'build', p, baseName + suffix);
            if (fs.existsSync(alt)) return alt;
        }
    }
    return baseName + suffix;
}

function startServer(context) {
    stopServer();
    const ws = vscode.workspace.workspaceFolders?.[0]?.uri?.fsPath;
    const vls = findBinary('vlsPath', 'vls');

    if (!fs.existsSync(vls)) {
        vscode.window.showWarningMessage(
            `Vayu: vls not found at ${vls}. Set vayu.vlsPath in settings.`);
        return;
    }

    log(`Starting vls: ${vls} (cwd=${ws || process.cwd()})`);
    server = {
        proc: cp.spawn(vls, [], {
            cwd: ws,
            stdio: ['pipe', 'pipe', 'pipe'],
            windowsHide: true
        }),
        pending: new Map()
    };

    server.proc.stdout.setEncoding('utf8');
    server.proc.stdout.on('data', d => { buffer += d; pumpIncoming(); });

    server.proc.stderr.setEncoding('utf8');
    server.proc.stderr.on('data', d => log('[vls stderr] ' + d.trim()));

    server.proc.on('exit', (code, sig) => {
        log(`vls exited: code=${code} sig=${sig}`);
        server = null;
    });

    server.proc.on('error', err => {
        log('vls spawn error: ' + err.message);
    });

    // initialize
    sendRequest('initialize', {
        processId: process.pid,
        rootUri: ws ? vscode.Uri.file(ws).toString() : null,
        capabilities: {
            textDocument: {
                synchronization: { dynamicRegistration: false },
                hover:              { contentFormat: ['markdown', 'plaintext'] },
                definition:         {},
                completion:         { completionItem: { snippetSupport: false } }
            }
        }
    }).catch(err => log('initialize failed: ' + err.message));

    sendNotification('initialized', {});
}

function stopServer() {
    if (server && server.proc && !server.proc.killed) {
        try { server.proc.kill(); } catch (_) {}
    }
    server = null;
    buffer = '';
    contentLength = -1;
}

function writeMessage(obj) {
    if (!server || !server.proc) return;
    const body = JSON.stringify(obj);
    const bytes = Buffer.byteLength(body, 'utf8');
    server.proc.stdin.write(`Content-Length: ${bytes}\r\n\r\n${body}`);
}

function sendRequest(method, params) {
    return new Promise((resolve, reject) => {
        if (!server) { reject(new Error('vls not running')); return; }
        const id = nextId++;
        server.pending.set(id, { resolve, reject });
        writeMessage({ jsonrpc: '2.0', id, method, params });
    });
}

function sendNotification(method, params) {
    writeMessage({ jsonrpc: '2.0', method, params });
}

function pumpIncoming() {
    while (true) {
        if (contentLength < 0) {
            const idx = buffer.indexOf('\r\n\r\n');
            if (idx < 0) return;
            const header = buffer.slice(0, idx);
            buffer = buffer.slice(idx + 4);
            contentLength = 0;
            for (const line of header.split('\r\n')) {
                const m = line.match(/^Content-Length:\s*(\d+)\s*$/i);
                if (m) contentLength = parseInt(m[1], 10);
            }
            if (contentLength < 0) { buffer = ''; return; }
        }
        if (Buffer.byteLength(buffer, 'utf8') < contentLength) return;
        const bodyBytes = Buffer.from(buffer, 'utf8');
        const head = bodyBytes.slice(0, contentLength).toString('utf8');
        buffer = bodyBytes.slice(contentLength).toString('utf8');
        contentLength = -1;
        let msg;
        try { msg = JSON.parse(head); } catch (e) { log('bad json: ' + e.message); continue; }
        handleMessage(msg);
    }
}

function handleMessage(msg) {
    if (msg.id !== undefined && server && server.pending.has(msg.id)) {
        const p = server.pending.get(msg.id);
        server.pending.delete(msg.id);
        if (msg.error) p.reject(new Error(msg.error.message || 'lsp error'));
        else           p.resolve(msg.result);
        return;
    }
    if (msg.method === 'textDocument/publishDiagnostics') {
        applyDiagnostics(msg.params);
        return;
    }
    if (msg.method === 'window/showMessage') {
        vscode.window.showInformationMessage(msg.params.message);
        return;
    }
}

function applyDiagnostics(params) {
    if (!diagColl) return;
    const uri = vscode.Uri.parse(params.uri);
    const list = (params.diagnostics || []).map(d => {
        const r = d.range;
        const range = new vscode.Range(
            r.start.line, r.start.character,
            r.end.line,   r.end.character);
        const sev = d.severity === 1 ? vscode.DiagnosticSeverity.Error
                  : d.severity === 2 ? vscode.DiagnosticSeverity.Warning
                  : vscode.DiagnosticSeverity.Information;
        const diag = new vscode.Diagnostic(range, d.message || '', sev);
        diag.source = d.source || 'vayu';
        return diag;
    });
    diagColl.set(uri, list);
}

function activate(context) {
    outChannel = vscode.window.createOutputChannel('Vayu Language Server');
    diagColl = vscode.languages.createDiagnosticCollection('vayu');
    context.subscriptions.push(diagColl, outChannel);

    startServer(context);

    // Sync open documents.
    vscode.workspace.textDocuments.forEach(doc => {
        if (doc.languageId === 'vayu') openDoc(doc);
    });

    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === 'vayu') openDoc(doc);
        }),
        vscode.workspace.onDidChangeTextDocument(ev => {
            if (ev.document.languageId === 'vayu') changeDoc(ev.document);
        }),
        vscode.workspace.onDidCloseTextDocument(doc => {
            if (doc.languageId === 'vayu') closeDoc(doc);
        })
    );

    // Hover.
    context.subscriptions.push(
        vscode.languages.registerHoverProvider('vayu', {
            provideHover(doc, pos) {
                return sendRequest('textDocument/hover', {
                    textDocument: { uri: doc.uri.toString() },
                    position: { line: pos.line, character: pos.character }
                }).then(res => {
                    if (!res || !res.contents) return null;
                    const md = new vscode.MarkdownString(
                        res.contents.value || res.contents);
                    return new vscode.Hover(md);
                }).catch(() => null);
            }
        })
    );

    // Goto definition.
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider('vayu', {
            provideDefinition(doc, pos) {
                return sendRequest('textDocument/definition', {
                    textDocument: { uri: doc.uri.toString() },
                    position: { line: pos.line, character: pos.character }
                }).then(res => {
                    if (!res) return null;
                    const arr = Array.isArray(res) ? res : [res];
                    return arr.map(loc => new vscode.Location(
                        vscode.Uri.parse(loc.uri),
                        new vscode.Range(
                            loc.range.start.line, loc.range.start.character,
                            loc.range.end.line,   loc.range.end.character)));
                }).catch(() => null);
            }
        })
    );

    // Completion.
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider('vayu', {
            provideCompletionItems(doc, pos) {
                return sendRequest('textDocument/completion', {
                    textDocument: { uri: doc.uri.toString() },
                    position: { line: pos.line, character: pos.character }
                }).then(res => {
                    if (!res || !res.items) return [];
                    return res.items.map(it => {
                        const ci = new vscode.CompletionItem(
                            it.label, vscode.CompletionItemKind.Function);
                        if (it.detail) ci.detail = it.detail;
                        return ci;
                    });
                }).catch(() => []);
            }
        }, '.')
    );

    // Format command.
    context.subscriptions.push(
        vscode.commands.registerCommand('vayu.formatFile', () => {
            const ed = vscode.window.activeTextEditor;
            if (!ed || ed.document.languageId !== 'vayu') {
                vscode.window.showWarningMessage('Open a .vyu file first.');
                return;
            }
            runFormatter(ed.document);
        })
    );

    // Lint command.
    context.subscriptions.push(
        vscode.commands.registerCommand('vayu.lintFile', () => {
            const ed = vscode.window.activeTextEditor;
            if (!ed || ed.document.languageId !== 'vayu') {
                vscode.window.showWarningMessage('Open a .vyu file first.');
                return;
            }
            runLinter(ed.document);
        })
    );

    // Restart server.
    context.subscriptions.push(
        vscode.commands.registerCommand('vayu.restartServer', () => {
            if (diagColl) diagColl.clear();
            startServer(context);
            vscode.workspace.textDocuments.forEach(doc => {
                if (doc.languageId === 'vayu') openDoc(doc);
            });
        })
    );
}

function openDoc(doc) {
    sendNotification('textDocument/didOpen', {
        textDocument: {
            uri: doc.uri.toString(),
            languageId: doc.languageId,
            version: doc.version,
            text: doc.getText()
        }
    });
}

function changeDoc(doc) {
    sendNotification('textDocument/didChange', {
        textDocument: { uri: doc.uri.toString(), version: doc.version },
        contentChanges: [{ text: doc.getText() }]
    });
}

function closeDoc(doc) {
    sendNotification('textDocument/didClose', {
        textDocument: { uri: doc.uri.toString() }
    });
    if (diagColl) diagColl.delete(doc.uri);
}

function runTool(binKey, defaultRel, args, doc) {
    const ws = vscode.workspace.workspaceFolders?.[0]?.uri?.fsPath;
    const bin = findBinary(binKey, defaultRel);
    if (!fs.existsSync(bin)) {
        vscode.window.showErrorMessage(`Vayu: ${bin} not found`);
        return;
    }
    const filePath = doc.uri.fsPath;
    cp.execFile(bin, args.concat([filePath]),
        { cwd: ws || path.dirname(filePath), maxBuffer: 4 * 1024 * 1024 },
        (err, stdout, stderr) => {
            if (binKey === 'vfmtPath') {
                if (err) {
                    vscode.window.showErrorMessage('vfmt: ' + (stderr || err.message));
                    return;
                }
                const edit = new vscode.WorkspaceEdit();
                const full = new vscode.Range(0, 0,
                    doc.lineCount, 0);
                edit.replace(doc.uri, full, stdout);
                vscode.workspace.applyEdit(edit).then(() => {
                    vscode.window.showInformationMessage('Vayu: formatted.');
                });
            } else {
                const out = (stdout || '') + (stderr || '');
                if (!out.trim()) {
                    vscode.window.showInformationMessage('Vayu: no lint warnings.');
                    return;
                }
                outChannel.clear();
                outChannel.appendLine(out);
                outChannel.show(true);
                vscode.window.showWarningMessage('Vayu: see Output -> Vayu.');
            }
        });
}

function runFormatter(doc) {
    runTool('vfmtPath', 'vfmt', [], doc);
}

function runLinter(doc) {
    runTool('vlintPath', 'vlint', [], doc);
}

function deactivate() {
    stopServer();
    if (diagColl) diagColl.clear();
}

module.exports = { activate, deactivate };