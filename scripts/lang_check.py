# 檢查語系檔是否涵蓋程式裡所有要翻譯的字串。
# 用法：uv run python scripts/lang_check.py [src/lang/zh-TW.txt ...]
# 不給參數時檢查 src/lang 底下每個 txt。輸出缺少與多餘的 key，有缺就以非 0 結束。
# 加 --template 會把全部 key 寫成 src/lang/_template.txt（值留空），給翻譯者起手用。
import io, re, sys, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT = r'L"((?:[^"\\]|\\.)*)"'


def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 't': '\t', '\\': '\\', '"': '"'}.get(n, '\\' + n))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def source_keys():
    keys = set()
    for f in glob.glob(ROOT + '/src/**/*.cpp', recursive=True):
        if '/tests/' in f.replace('\\', '/'):
            continue
        s = io.open(f, encoding='utf-8').read()
        # tr(L"..." L"...") / trs(...)：相鄰字串常值會接起來
        for m in re.finditer(r'\b(?:trs?|fmt1|fmt2)\(\s*((?:' + LIT + r'\s*)+)[),]', s):
            keys.add(c_unescape(''.join(re.findall(LIT, m.group(1)))))
        # buildMenu 的 item(menu, ID, L"...", ...)
        for m in re.finditer(r'\bitem\(\w+,\s*\w+,\s*' + LIT, s):
            keys.add(c_unescape(m.group(1)))
    rc = io.open(ROOT + '/src/app/app.rc', encoding='utf-8').read()
    for m in re.finditer(r'^\s*(?:CAPTION|LTEXT|RTEXT|AUTOCHECKBOX|PUSHBUTTON|DEFPUSHBUTTON|GROUPBOX|AUTORADIOBUTTON)\s+"((?:[^"]|"")*)"', rc, re.M):
        if m.group(1):
            keys.add(m.group(1).replace('""', '"'))
    return keys


def lang_keys(path):
    keys = set()
    for line in io.open(path, encoding='utf-8-sig'):
        line = line.strip()
        if not line or line[0] in '#;':
            continue
        # 第一個沒跳脫的 =
        i, k = 0, []
        while i < len(line):
            if line[i] == '\\' and i + 1 < len(line):
                k.append({'n': '\n', 't': '\t', '\\': '\\', '=': '='}.get(line[i + 1], '\\' + line[i + 1]))
                i += 2
                continue
            if line[i] == '=':
                break
            k.append(line[i])
            i += 1
        key = ''.join(k).strip()
        if key and not key.startswith('_'):
            keys.add(key)
    return keys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    src = source_keys()
    if '--template' in sys.argv:
        esc = lambda t: t.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('=', '\\=')
        with io.open(ROOT + '/src/lang/_template.txt', 'w', encoding='utf-8', newline='\n') as f:
            f.write('# mpainter language template. Copy to lang\\<code>.txt, fill _code / _name and the translations.\n')
            f.write('# One line per string: English original=translation. Escapes: \\n \\t \\\\ \\= . Keep %d / %s / %llu placeholders.\n')
            f.write('_code=\n_name=\n\n')
            for k in sorted(src):
                f.write(esc(k) + '=\n')
        print('template written:', len(src), 'keys')
    files = args or glob.glob(ROOT + '/src/lang/*.txt')
    bad = False
    for p in files:
        if os.path.basename(p).startswith('_'):
            continue
        have = lang_keys(p)
        missing, extra = sorted(src - have), sorted(have - src)
        print(f'{os.path.relpath(p, ROOT)}: {len(have)} keys, missing {len(missing)}, unused {len(extra)}')
        for k in missing:
            print('  MISSING:', repr(k))
        for k in extra:
            print('  UNUSED: ', repr(k))
        bad = bad or bool(missing)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
