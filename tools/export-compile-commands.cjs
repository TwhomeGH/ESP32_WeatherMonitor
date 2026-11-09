// 將 Arduino 建置副本的編譯項目對應回專案原始檔，供 VS Code 使用。
const fs = require('node:fs');
const path = require('node:path');

function normalizeArguments(args, directory) {
  const unquote = value => value.replace(/^"(.*)"$/, '$1');
  function expand(tokens, seen = new Set()) {
    return tokens.flatMap(token => {
      if (!token.startsWith('@')) return [token];
      const file = path.resolve(directory, unquote(token.slice(1)));
      if (seen.has(file)) throw Error('Recursive response file: ' + file);
      const nested = new Set(seen); nested.add(file);
      return expand(fs.readFileSync(file, 'utf8').match(/(?:[^\s"]+|"[^"]*")+/g) || [], nested);
    });
  }
  const expanded = expand(args), result = [];
  let prefix = directory;
  for (let i = 0; i < expanded.length; i++) {
    const arg = expanded[i];
    if (arg === '-iprefix') prefix = path.resolve(directory, unquote(expanded[++i]));
    else if (arg === '-iwithprefixbefore' || arg === '-iwithprefix') {
      result.push('-I' + path.resolve(prefix, unquote(expanded[++i])));
    } else result.push(arg);
  }
  return result;
}

/**
 * 匯出原始檔對應及 VS Code 備援設定；覆寫專案中的索引與設定檔。
 * @param {string} source - Arduino 編譯索引的 JSON 路徑。
 * @param {string} root - 專案的絕對路徑。
 * @returns {string} 匯出後的編譯索引路徑。
 * @throws {Error} 來源、參數檔或設定檔無法讀寫／解析時拋出錯誤。
 */
function exportCommands(source, root) {
  const entries = JSON.parse(fs.readFileSync(source, 'utf8'));
  const samePath = (a, b) => path.resolve(a).toLowerCase() === path.resolve(b).toLowerCase();
  for (const entry of entries) {
    // IntelliSense 索引直接列出標頭路徑，不依賴 GCC response file／prefix 選項。
    if (entry.arguments) entry.arguments = normalizeArguments(entry.arguments, entry.directory);
    const original = entry.file;
    const match = original.replace(/\\/g, '/').match(/\/sketch\/(.+)$/);
    if (!match) continue;
    const relative = match[1].replace(/\.ino\.cpp$/, '.ino');
    const local = path.resolve(root, relative);
    if (!local.startsWith(root + path.sep) || !fs.existsSync(local)) continue;
    entry.file = local;
    if (entry.arguments) {
      entry.arguments = entry.arguments.map(arg => samePath(arg, original) ? local : arg);
      // 原始檔與其他本地標頭優先於建置資料夾副本。
      entry.arguments.splice(1, 0, '-I' + root);
    }
  }
  const output = path.join(root, 'build', 'compile_commands.json');
  fs.mkdirSync(path.dirname(output), {recursive:true});
  fs.writeFileSync(output, JSON.stringify(entries, null, 2) + '\n');
  // 標頭檔不一定能關聯至某個編譯單元，提供相同 SDK 的備援設定。
  const sketch = entries.find(entry => entry.file === path.join(root, 'ntfy_test.cpp'));
  if (sketch?.arguments) {
    const tokens = sketch.arguments.flatMap(arg => {
      if (!arg.startsWith('@')) return [arg];
      const file = arg.slice(1);
      if (!fs.existsSync(file)) return [];
      return fs.readFileSync(file, 'utf8').match(/(?:[^\s"]+|"[^"]*")+/g) || [];
    });
    const includes = [root], defines = [];
    let prefix = '';
    for (let i = 1; i < tokens.length; i++) {
      const arg = tokens[i];
      if (arg === '-iprefix') prefix = tokens[++i];
      else if (arg === '-iwithprefixbefore') includes.push(path.join(prefix, tokens[++i]));
      else if (arg === '-I') includes.push(tokens[++i]);
      else if (arg.startsWith('-I')) includes.push(arg.slice(2));
      else if (arg === '-D') defines.push(tokens[++i]);
      else if (arg.startsWith('-D')) defines.push(arg.slice(2));
    }
    const configPath = path.join(root, '.vscode', 'c_cpp_properties.json');
    const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
    const target = config.configurations.find(c => c.name === 'ESP32');
    if (target) {
      // SDK 共用旗標可能包含未隨此版本安裝的元件目錄。
      // GCC 可忽略不存在的 -I，但 VS Code 備援設定會警告，因此只列出實際目錄。
      target.includePath = [...new Set(includes.map(p => path.resolve(p)))]
        .filter(directory => fs.existsSync(directory) && fs.statSync(directory).isDirectory())
        .map(directory => directory === root ? '${workspaceFolder}' : directory.replace(/\\/g, '/'));
      target.defines = [...new Set(defines)];
      target.compilerPath = sketch.arguments[0].replace(/\\/g, '/');
      target.intelliSenseMode = 'windows-gcc-x86';
      fs.writeFileSync(configPath, JSON.stringify(config, null, 2) + '\n');
    }
  }
  return output;
}
module.exports = {exportCommands, normalizeArguments};
if (require.main === module) {
  const root = path.resolve(__dirname, '..');
  console.log(exportCommands(process.argv[2] || path.join(root, 'build', 'compile_commands.json'), root));
}
