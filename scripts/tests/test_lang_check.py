# scripts/lang_check.py 的測試。用法：uv run --with pytest pytest scripts/tests/test_lang_check.py
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import lang_check as lc


def test_c_unescape():
    assert lc.c_unescape(r'a\nb\t\\ \"q\"') == 'a\nb\t\\ "q"'
    assert lc.c_unescape(r'keep \x') == 'keep \\x'
    assert lc.c_unescape('') == ''


def test_cpp_keys_finds_each_marker_function():
    text = r'''
        auto a = tr(L"Open…");
        auto b = trs(L"Save");
        return trKey(L"PNG image");
        x = fmt1(L"Cannot open: %s", name);
        y = fmt2(L"In use: %s (%s)", a, b);
        item(menu, IDM_OPEN, L"&Open…\tCtrl+O", true);
    '''
    assert lc.cpp_keys(text) == {'Open…', 'Save', 'PNG image', 'Cannot open: %s', 'In use: %s (%s)', '&Open…\tCtrl+O'}


def test_cpp_keys_joins_adjacent_literals_and_unescapes():
    text = 'tr(L"line one\\n"\n   L"line \\"two\\"")'
    assert lc.cpp_keys(text) == {'line one\nline "two"'}


def test_cpp_keys_ignores_indirect_and_other_calls():
    text = r'''
        tr(key);                // 不是字串常值
        mytr(L"not a key");     // 名字不同
        strKey(L"nope");
        other(L"x");
    '''
    assert lc.cpp_keys(text) == set()


def test_rc_keys():
    rc = '''
IDD_X DIALOGEX 0, 0, 10, 10
CAPTION "Settings"
BEGIN
    LTEXT           "Name:",IDC_STATIC,7,7,40,8
    LTEXT           "",IDC_EMPTY,7,7,40,8
    PUSHBUTTON      "Say ""hi""",IDC_B,1,1,1,1
    EDITTEXT        IDC_EDIT,1,1,1,1
    CONTROL         "ignored",IDC_C,"Button",0,1,1,1,1
END
'''
    assert lc.rc_keys(rc) == {'Settings', 'Name:', 'Say "hi"'}


def test_parse_lang_line():
    assert lc.parse_lang_line('Open…=開啟…') == ('Open…', '開啟…')
    assert lc.parse_lang_line('  Save = 儲存  ') == ('Save', '儲存')
    assert lc.parse_lang_line(r'A\=B=甲=乙') == ('A=B', '甲=乙')          # key 的等號要跳脫，值的不用
    assert lc.parse_lang_line(r'Line one\nLine two=第一行\n第二行') == ('Line one\nLine two', '第一行\n第二行')
    assert lc.parse_lang_line(r'back\\slash=反\\斜') == ('back\\slash', '反\\斜')
    assert lc.parse_lang_line('Empty=') == ('Empty', '')
    for junk in ['', '   ', '# comment', '; comment', 'no equals sign', '=no key']:
        assert lc.parse_lang_line(junk) is None


def test_lang_entries_skips_header_keys(tmp_path):
    p = tmp_path / 'xx.txt'
    p.write_bytes('﻿_code=xx\n_name=Test\n# c\nSave=S\nOpen=O\n'.encode('utf-8'))
    assert lc.lang_entries(str(p)) == {'Save': 'S', 'Open': 'O'}


def test_format_specs():
    assert lc.format_specs('plain text') == []
    assert lc.format_specs('%s: %d%%') == ['s', 'd']
    assert lc.format_specs('%.0f × %.0f pixels/inch') == ['f', 'f']
    assert lc.format_specs('%llu bytes / %lu / %08X / %-5.2ls / %I64d') == ['llu', 'lu', 'X', 'ls', 'I64d']
    assert lc.format_specs('%*d %.*s') == ['*', 'd', '*', 's']
    assert lc.format_specs('100%% sure') == []
    assert lc.format_specs('50% off') == ['o']          # 對 printf 來說是合法的
    for bad in ['100%', '%n', '%s %n', '% 以上', '%5']:
        assert lc.format_specs(bad) is None


def test_placeholder_mismatches():
    entries = {
        'Cannot open: %s': '無法開啟：%s',      # 一樣
        'Saved %d of %d': '存了 %d 個，共 %d 個',
        'Bad: %s': '壞：%d',                    # 型別不同
        'More: %s': '多：%s %s',                # 多一個
        'Less: %s': '少',                       # 少一個
        'Swap: %s %d': '換：%d %s',             # 順序不同
        'N: %s': '%n%s',                        # %n
        'Plain': '普通',
        'Plain2': '意外的 %s',                  # key 沒有，翻譯多了
        'Percent': '100%',                      # 翻譯不是合法的格式
        'Half 50%': '一半 50%',                 # key 本身不是合法的格式：不檢查
        'Rate %d%%': '比率 %d%%',
    }
    assert lc.placeholder_mismatches(entries) == ['Bad: %s', 'Less: %s', 'More: %s', 'N: %s', 'Percent', 'Plain2', 'Swap: %s %d']


def test_shipped_language_files_cover_every_key_and_match_placeholders():
    src = lc.source_keys()
    assert len(src) > 300
    for name in ['zh-TW.txt', 'ja-JP.txt']:
        entries = lc.lang_entries(os.path.join(lc.ROOT, 'src', 'lang', name))
        assert sorted(src - set(entries)) == [], name
        assert lc.placeholder_mismatches(entries) == [], name
