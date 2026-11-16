# 檢查語系檔是否涵蓋程式裡所有要翻譯的字串，每一行翻譯的佔位符（%s、%d…）和英文原文一樣，而且翻譯不是空的。
# 用法：uv run scripts/lang_check.py [src/lang/zh-TW.txt ...]
# 不給參數時檢查 src/lang 底下每個 txt。輸出缺少與多餘的 key、佔位符對不上的行、翻譯是空的行；
# 有缺、有對不上、有空的就以非 0 結束（多餘的 key 只列出來）。
# 看得到的 key：src 底下 .cpp / .h（測試除外）裡字串常值緊接在 tr( / trs( / trKey( / fmt1( / fmt2( 後面的，
# 與 app.rc 對話框範本裡控制項的文字。key 放在表裡、之後才 tr(變數) 的，表裡要用 trKey(L"...") 標記，不然這裡看不到。
# 加 --template 會把全部 key 寫成 src/lang/_template.txt（值留空），給翻譯者起手用。
# 測試：uv run --with pytest pytest scripts/tests/test_lang_check.py
import io, re, sys, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIT = r'L"((?:[^"\\]|\\.)*)"'
# 第一個引數是翻譯 key 的函式。trKey 只是標記（core 回傳英文 key，由 app 顯示時翻譯）。
KEY_FUNCTIONS = r'trs?|trKey|fmt1|fmt2'
# rc 檔裡第一個參數是顯示文字的敘述：對話框標題與各種控制項（CONTROL 是通用寫法，文字是空的就不算）。
RC_TEXT_STATEMENTS = ('CAPTION|LTEXT|RTEXT|CTEXT|PUSHBUTTON|DEFPUSHBUTTON|PUSHBOX|GROUPBOX|CHECKBOX|AUTOCHECKBOX|'
                      'RADIOBUTTON|AUTORADIOBUTTON|STATE3|AUTO3STATE|CONTROL')


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


def cpp_keys(text):
    """一個 .cpp / .h 的內容裡的翻譯 key。只認字串常值緊接在括號後的寫法。"""
    keys = set()
    # tr(L"..." L"...") / trs(...) / trKey(...)：相鄰字串常值會接起來
    for m in re.finditer(r'\b(?:' + KEY_FUNCTIONS + r')\(\s*((?:' + LIT + r'\s*)+)[),]', text):
        keys.add(c_unescape(''.join(re.findall(LIT, m.group(1)))))
    # buildMenu 的 item(menu, ID, L"...", ...)
    for m in re.finditer(r'\bitem\(\w+,\s*\w+,\s*' + LIT, text):
        keys.add(c_unescape(m.group(1)))
    return keys


def rc_keys(text):
    """app.rc 的對話框範本裡要翻譯的字串：標題與每一種「第一個參數是文字」的控制項（translateDialog 會逐一翻譯）。"""
    keys = set()
    for m in re.finditer(r'^\s*(?:' + RC_TEXT_STATEMENTS + r')\s+"((?:[^"]|"")*)"', text, re.M):
        if m.group(1):
            keys.add(m.group(1).replace('""', '"'))
    return keys


def source_keys():
    keys = set()
    for f in glob.glob(ROOT + '/src/**/*.cpp', recursive=True) + glob.glob(ROOT + '/src/**/*.h', recursive=True):
        if '/tests/' in f.replace('\\', '/'):
            continue
        keys |= cpp_keys(io.open(f, encoding='utf-8').read())
    keys |= rc_keys(io.open(ROOT + '/src/app/app.rc', encoding='utf-8').read())
    return keys


def parse_lang_line(line):
    """語系檔的一行 -> (key, value)；註解、空行、沒有 = 的行回 None。跳脫規則同 core/Lang.cpp。"""
    line = line.strip()
    if not line or line[0] in '#;':
        return None
    unescape = {'n': '\n', 't': '\t', '\\': '\\', '=': '='}
    # 第一個沒跳脫的 =
    i, k = 0, []
    while i < len(line):
        if line[i] == '\\' and i + 1 < len(line):
            k.append(unescape.get(line[i + 1], '\\' + line[i + 1]))
            i += 2
            continue
        if line[i] == '=':
            break
        k.append(line[i])
        i += 1
    else:
        return None
    raw, v, j = line[i + 1:].strip(), [], 0
    while j < len(raw):
        if raw[j] == '\\' and j + 1 < len(raw):
            v.append(unescape.get(raw[j + 1], '\\' + raw[j + 1]))
            j += 2
        else:
            v.append(raw[j])
            j += 1
    key = ''.join(k).strip()
    return (key, ''.join(v)) if key else None


def lang_entries(path):
    """語系檔的 key -> 翻譯（不含 _code、_name）。"""
    entries = {}
    for line in io.open(path, encoding='utf-8-sig'):
        kv = parse_lang_line(line)
        if kv and not kv[0].startswith('_'):
            entries[kv[0]] = kv[1]
    return entries


def format_specs(s):
    """printf 格式字串裡會吃引數的轉換，依序列出（例如 '%s: %d%%' -> ['s', 'd']）；寬度或精度寫 * 的也算一個。
    格式不合法（% 後面不是轉換、結尾單獨的 %、用了 %n）回 None。和 core/Lang.cpp 的 formatSpecs 同一套規則。"""
    specs, i, n = [], 0, len(s)
    while i < n:
        if s[i] != '%':
            i += 1
            continue
        i += 1
        if i >= n:
            return None
        if s[i] == '%':
            i += 1
            continue
        while i < n and s[i] in '-+ #0':
            i += 1
        if i < n and s[i] == '*':
            specs.append('*')
            i += 1
        else:
            while i < n and s[i].isascii() and s[i].isdigit():
                i += 1
        if i < n and s[i] == '.':
            i += 1
            if i < n and s[i] == '*':
                specs.append('*')
                i += 1
            else:
                while i < n and s[i].isascii() and s[i].isdigit():
                    i += 1
        token = ''
        while i < n and s[i] in 'hlLjztwI':
            token += s[i]
            i += 1
            if token[-1] == 'I':
                while i < n and s[i].isascii() and s[i].isdigit():
                    token += s[i]
                    i += 1
        if i >= n or s[i] not in 'diuoxXeEfFgGaAcCsSp':
            return None
        specs.append(token + s[i])
        i += 1
    return specs


def placeholder_mismatches(entries):
    """佔位符和 key 對不上的 key（程式載入語系檔時會丟掉這些行、顯示英文）。key 本身不是合法格式的不檢查。"""
    bad = []
    for key, value in entries.items():
        want = format_specs(key)
        if want is not None and format_specs(value) != want:
            bad.append(key)
    return sorted(bad)


def empty_translations(entries):
    """翻譯是空的 key（寫了 key= 卻沒填）：程式會顯示成空白，不會退回英文。"""
    return sorted(k for k, v in entries.items() if not v.strip())


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
        entries = lang_entries(p)
        have = set(entries)
        missing, extra, mismatched = sorted(src - have), sorted(have - src), placeholder_mismatches(entries)
        empty = empty_translations(entries)
        print(f'{os.path.relpath(p, ROOT)}: {len(have)} keys, missing {len(missing)}, unused {len(extra)}, '
              f'placeholder mismatch {len(mismatched)}, empty {len(empty)}')
        for k in missing:
            print('  MISSING:', repr(k))
        for k in extra:
            print('  UNUSED: ', repr(k))
        for k in mismatched:
            print('  PLACEHOLDER:', repr(k), '->', repr(entries[k]))
        for k in empty:
            print('  EMPTY:  ', repr(k))
        bad = bad or bool(missing) or bool(mismatched) or bool(empty)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
