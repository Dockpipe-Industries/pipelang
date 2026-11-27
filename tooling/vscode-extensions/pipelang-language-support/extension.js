// @ts-check
const vscode = require("vscode");
const childProcess = require("child_process");
const path = require("path");

const PIPELANG_COMPLETION_KEYWORDS = [
  "public",
  "private",
  "Interface",
  "Class",
  "Struct",
  "Record",
  "Enum",
  "List",
  "Result",
  "Optional",
  "ArithmeticError",
  "IComparable",
  "string",
  "int",
  "bool",
  "float",
  "new",
  "return",
  "if",
  "else",
  "some",
  "none",
  "has_value",
  "value_or",
  "empty_list",
  "list",
  "count",
  "append",
  "find_by",
  "filter_by",
  "filter",
  "contains_casefolded",
  "filter_contains_casefolded",
  "filter_joined_contains_casefolded",
  "sort_by_ordinal",
  "ascending",
  "descending",
  "propagate",
  "match",
  "ok",
  "err",
  "_",
  "trim",
  "true",
  "false"
];

function pipeLangDiagnosticSeverity(severity) {
  switch (severity) {
    case "warning": return vscode.DiagnosticSeverity.Warning;
    case "error":
    default: return vscode.DiagnosticSeverity.Error;
  }
}

function pipeLangPosition(position) {
  const line = Math.max(0, Number(position?.line || 1) - 1);
  const column = Math.max(0, Number(position?.utf16_column || position?.column || 1) - 1);
  return new vscode.Position(line, column);
}

function pipeLangRange(range) {
  return new vscode.Range(pipeLangPosition(range?.start), pipeLangPosition(range?.end));
}

function pipeLangEditorDiagnostics(document, diagnostics) {
  const documentPath = path.normalize(document.fileName);
  return diagnostics
    .filter((item) => path.normalize(String(item?.primary?.file || "")) === documentPath)
    .map((item) => {
      const diagnostic = new vscode.Diagnostic(
        pipeLangRange(item.primary),
        String(item.message || "PipeLang diagnostic"),
        pipeLangDiagnosticSeverity(item.severity)
      );
      diagnostic.code = String(item.code || "");
      diagnostic.source = "PipeLang";
      diagnostic.relatedInformation = (Array.isArray(item.related) ? item.related : []).map((related) => new vscode.DiagnosticRelatedInformation(
        new vscode.Location(vscode.Uri.file(String(related.range.file)), pipeLangRange(related.range)),
        String(related.message || "related location")
      ));
      return diagnostic;
    });
}


function runPipeLangCheck(document, binary, options, execute = childProcess.execFile) {
  return new Promise((resolve) => {
    if (typeof binary !== "string" || !binary || binary.includes("\0")) {
      resolve(null);
      return;
    }
    const timeoutMs = Number.isSafeInteger(options.timeoutMs) && options.timeoutMs >= 100
      ? options.timeoutMs : 10000;
    const maxOutputBytes = Number.isSafeInteger(options.maxOutputBytes) && options.maxOutputBytes >= 1024
      ? options.maxOutputBytes : 4194304;
    const child = execute(binary,
      ["check", "--in", document.fileName, "--format", "json", "--stdin"],
      { cwd: path.dirname(document.fileName), windowsHide: true, shell: false,
        timeout: timeoutMs, maxBuffer: maxOutputBytes, signal: options.signal },
      (error, stdout) => {
        if (error && (error.killed || error.name === "AbortError" || error.code === "ERR_CHILD_PROCESS_STDIO_MAXBUFFER")) {
          resolve(null);
          return;
        }
        try {
          const payload = JSON.parse(stdout);
          resolve(payload && payload.schema === 1 && Array.isArray(payload.diagnostics) ? payload.diagnostics : null);
        } catch {
          resolve(null);
        }
      });
    child.stdin.on("error", () => {});
    child.stdin.end(document.getText(), "utf8");
  });
}

function activate(context) {
  const diagnostics = vscode.languages.createDiagnosticCollection("pipelang");
  const timers = new Map();
  const requests = new Map();
  const keyFor = (document) => document.uri.toString();
  const cancel = (key) => {
    clearTimeout(timers.get(key));
    timers.delete(key);
    const request = requests.get(key);
    if (request) request.abort();
    requests.delete(key);
  };
  const refresh = async (document) => {
    const key = keyFor(document);
    timers.delete(key);
    const configuration = vscode.workspace.getConfiguration("pipelang", document.uri);
    const binary = configuration.get("executablePath") || (process.platform === "win32" ? "pipelang.exe" : "pipelang");
    const request = new AbortController();
    requests.set(key, request);
    const version = document.version;
    const result = await runPipeLangCheck(document, binary, {
      timeoutMs: configuration.get("diagnostics.timeoutMs", 10000),
      maxOutputBytes: configuration.get("diagnostics.maxOutputBytes", 4194304),
      signal: request.signal
    });
    if (requests.get(key) !== request) return;
    requests.delete(key);
    if (request.signal.aborted || document.version !== version) return;
    if (result) diagnostics.set(document.uri, pipeLangEditorDiagnostics(document, result));
    else diagnostics.delete(document.uri);
  };
  const schedule = (document) => {
    if (!document || document.languageId !== "pipelang" || document.uri.scheme !== "file") return;
    const key = keyFor(document);
    cancel(key);
    timers.set(key, setTimeout(() => { void refresh(document); }, 150));
  };
  context.subscriptions.push(diagnostics,
    vscode.workspace.onDidOpenTextDocument(schedule),
    vscode.workspace.onDidChangeTextDocument((event) => schedule(event.document)),
    vscode.workspace.onDidSaveTextDocument(schedule),
    vscode.workspace.onDidCloseTextDocument((document) => { cancel(keyFor(document)); diagnostics.delete(document.uri); }),
    { dispose: () => { for (const key of new Set([...timers.keys(), ...requests.keys()])) cancel(key); } },
    vscode.languages.registerCompletionItemProvider({ language: "pipelang", scheme: "file" }, {
      provideCompletionItems() {
        return PIPELANG_COMPLETION_KEYWORDS.map((keyword) => new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword));
      }
    })
  );
  for (const document of vscode.workspace.textDocuments) schedule(document);
}

function deactivate() {}

module.exports = { activate, deactivate, __test: {
  PIPELANG_COMPLETION_KEYWORDS, pipeLangPosition, pipeLangRange, pipeLangEditorDiagnostics, runPipeLangCheck
} };
