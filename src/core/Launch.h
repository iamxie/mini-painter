#pragma once
#include <string>
#include <vector>

namespace mp {

// mpainter 的命令列。重開自己（換語言、自動更新）時組的字串和 main 拆的規則只寫在這裡。
//   mpainter.exe [路徑]
//   mpainter.exe --after-update <舊行程 pid> [路徑]     自動更新換好檔之後由舊的那個啟動
struct LaunchArgs {
    bool afterUpdate = false;
    unsigned long waitPid = 0; // afterUpdate：先等這個行程結束（它在存設定、刪暫存）
    std::wstring path;         // 要開的圖片、資料夾或壓縮檔；空 = 不開
};

// 拆命令列。argv[0] 是 exe 自己。
LaunchArgs parseLaunchArgs(const std::vector<std::wstring>& argv);

// 把一個引數包成命令列上的一段（CommandLineToArgvW 的規則）：有空白、tab、引號或是空字串時加引號，
// 裡面的引號加反斜線，緊接在引號前的反斜線（含結尾的）加倍。D:\comics\ 這種結尾是反斜線的路徑
// 直接包引號的話，最後的 \" 會被當成跳脫的引號。
std::wstring quoteArg(const std::wstring& arg);

// 給 exe 之後的參數字串（不含 exe 本身）。path 是空的就不帶路徑。
std::wstring openArgs(const std::wstring& path);
std::wstring afterUpdateArgs(unsigned long pid, const std::wstring& path);

} // namespace mp
