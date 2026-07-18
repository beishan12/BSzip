#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <direct.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <process.h>
#pragma warning(disable:4996)
using namespace std;
#undef UNICODE
#undef _UNICODE
#define CP_GBK 936
#define IDC_BTN_COMPRESS    101
#define IDC_BTN_DECOMPRESS  102
#define IDC_EDIT_STATUS     103
#define IDC_STATIC_TITLE    104
#define MAX_READ_SIZE       1024 * 1024
#define IDC_BTN_SELECTFILES 105
#define IDC_BTN_SELECTDIRS  106
#define WM_USER_APPEND      (WM_USER + 100)
#define IDC_FILE_LIST         201
#define IDC_BTN_SELECTALL     202
#define IDC_BTN_UNSELECTALL   203
#define IDC_BTN_EXTRACT_SELECTED 204
const string XOR_KEY = "BS";
const int LZ_WINDOW_SIZE = 4096;
const int LZ_MAX_LEN = 255;
const int MATCH_THRESHOLD = 5;
#define IDC_ARROW_A MAKEINTRESOURCEA(32512)
#define IDI_APPLICATION_A MAKEINTRESOURCEA(32512)
HWND hWndStatus;
HWND hMainWnd;
const char* CLASS_NAME = "BSZip";
string g_CommandLineZipFile;
bool g_bModalActive = false;
// 存储文件的【压缩包内相对路径】+加密后数据，替换原来只存文件名
struct ZipEntry
{
    string relativePath;
    string packedData;
};
vector<ZipEntry> g_previewEntries;
vector<char> g_checkState;
HWND g_previewWnd = nullptr;
// 线程入参结构体
struct CompressParam
{
    HWND hParent;
    string baseDir;
    vector<string> fullFilePaths;
};
wstring GBKToUnicode(const string& gbkStr);
string UnicodeToGBK(const wstring& unicodeStr);
void PostStatus(HWND hWnd, const string& text);
void AppendStatus(const string& text);
string XorCrypt(const string& data);
string LZ77Compress(const string& input);
bool LZ77DecompressStream(const string& packData, FILE* fpOut);
bool UnpackDataToFile(const string& packedData, const string& outPath);
string PackData(const string& fileRaw);
string UnpackData(const string& pack);
string ReadFileBin(const string& gbkPath);
bool WriteFileBin(const string& gbkPath, const string& data);
void RecurseScanFiles(const string& rootDir, vector<string>& outFiles);
string GetRelativePath(const string& fullPath, const string& baseFolder);
bool CreateFolderDeep(string gbkFolder);
vector<string> SelectMultipleFiles(HWND hParent);
vector<string> SelectMultiFolder(HWND hParent);
string SelectFolder(HWND hParent);
string SelectSaveBszip(HWND hParent);
string SelectOpenBszip(HWND hParent);
bool DoCompressFileListCore(HWND hParent, const string& baseDir, const vector<string>& allFiles);
static inline int ReadInt(const char* p);
bool ParseBszip(const string& zipPath, vector<ZipEntry>& entries);
bool ExtractSelectedCore(HWND hParent, const string& outDir);
void ShowZipPreview(HWND hParent);
bool LaunchZipPreviewExe(const string& zipFilePath);
bool LaunchZipPreviewExe(const string& zipFilePath)
{
    char cmdLine[2048] = {0};
    sprintf(cmdLine, "\"bszip_bokzone.exe\" \"%s\"", zipFilePath.c_str());
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(STARTUPINFOA);
    BOOL ret = CreateProcessA(NULL, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (ret)
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    PostStatus(hMainWnd, "启动bszip_bokzone.exe失败");
    return false;
}
// 向主线程发送日志消息，子线程专用
void PostStatus(HWND hWnd, const string& text)
{
    char* pBuf = new char[text.size() + 1];
    strcpy(pBuf, text.c_str());
    PostMessageA(hWnd, WM_USER_APPEND, (WPARAM)pBuf, 0);
}
// 主线程真正执行文本追加
void AppendStatus(const string& text)
{
    int len = GetWindowTextLengthA(hWndStatus);
    SendMessageA(hWndStatus, EM_SETSEL, len, len);
    SendMessageA(hWndStatus, EM_REPLACESEL, 0, (LPARAM)text.c_str());
    SendMessageA(hWndStatus, EM_REPLACESEL, 0, (LPARAM)"\r\n");
}
wstring GBKToUnicode(const string& gbkStr)
{
    int len = MultiByteToWideChar(CP_GBK, 0, gbkStr.c_str(), -1, NULL, 0);
    wchar_t* wstr = new wchar_t[len];
    MultiByteToWideChar(CP_GBK, 0, gbkStr.c_str(), -1, wstr, len);
    wstring res = wstr;
    delete[] wstr;
    return res;
}
string UnicodeToGBK(const wstring& unicodeStr)
{
    int len = WideCharToMultiByte(CP_GBK, 0, unicodeStr.c_str(), -1, NULL, 0, NULL, NULL);
    char* str = new char[len];
    WideCharToMultiByte(CP_GBK, 0, unicodeStr.c_str(), -1, str, len, NULL, NULL);
    string res = str;
    delete[] str;
    return res;
}
string XorCrypt(const string& data)
{
    string out = data;
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] ^= XOR_KEY[i % XOR_KEY.size()];
    }
    return out;
}
string LZ77Compress(const string& input)
{
    string out;
    size_t pos = 0;
    size_t n = input.size();
    while (pos < n)
    {
        unsigned char flagByte = 0;
        string rawBuf;
        int tokenCnt = 0;
        while (tokenCnt < 8 && pos < n)
        {
            int bestOff = 0;
            int bestLen = 0;
            size_t windowStart = (pos > LZ_WINDOW_SIZE) ? pos - LZ_WINDOW_SIZE : 0;
            for (size_t off = windowStart; off < pos; off++)
            {
                int matchLen = 0;
                while (matchLen < LZ_MAX_LEN && (pos + matchLen) < n && input[off + matchLen] == input[pos + matchLen])
                    matchLen++;
                if (matchLen > bestLen)
                {
                    bestLen = matchLen;
                    bestOff = static_cast<int>(pos - off);
                }
            }
            if (bestLen >= MATCH_THRESHOLD)
            {
                flagByte |= (1 << tokenCnt);
                out += static_cast<char>(bestOff & 0xff);
                out += static_cast<char>((bestOff >> 8) & 0xff);
                out += static_cast<char>(bestLen);
                pos += bestLen;
            }
            else
            {
                rawBuf += input[pos++];
            }
            tokenCnt++;
        }
        out += flagByte;
        out.append(rawBuf);
    }
    return out;
}
// 流式LZ77解压，直接写入文件，解决大文件内存崩溃
bool LZ77DecompressStream(const string& packData, FILE* fpOut)
{
    string outBuf;
    const size_t BUF_FLUSH = 1024 * 1024;
    outBuf.reserve(LZ_WINDOW_SIZE);
    size_t pos = 0;
    size_t n = packData.size();
    while (pos < n)
    {
        unsigned char flagByte = static_cast<unsigned char>(packData[pos++]);
        if (pos > n) break;
        for (int i = 0; i < 8 && pos < n; i++)
        {
            if (flagByte & (1 << i))
            {
                if (pos + 3 > n) return false;
                int off = static_cast<unsigned char>(packData[pos]) | (static_cast<unsigned char>(packData[pos + 1]) << 8);
                int len = static_cast<unsigned char>(packData[pos + 2]);
                pos += 3;
                size_t start = outBuf.size() - off;
                for (int k = 0; k < len; k++)
                {
                    outBuf += outBuf[start + k];
                    if (outBuf.size() >= BUF_FLUSH)
                    {
                        fwrite(outBuf.data(), 1, outBuf.size(), fpOut);
                        outBuf.clear();
                    }
                }
            }
            else
            {
                outBuf += packData[pos++];
                if (outBuf.size() >= BUF_FLUSH)
                {
                    fwrite(outBuf.data(), 1, outBuf.size(), fpOut);
                    outBuf.clear();
                }
            }
        }
    }
    if (!outBuf.empty())
    {
        fwrite(outBuf.data(), 1, outBuf.size(), fpOut);
    }
    return true;
}
// 流式解密+解压直接输出文件，不产生超大内存字符串
bool UnpackDataToFile(const string& packedData, const string& outPath)
{
    string lzData = XorCrypt(packedData);
    wstring wOut = GBKToUnicode(outPath);
    FILE* fp = _wfopen(wOut.c_str(), L"wb");
    if (!fp) return false;
    bool ret = LZ77DecompressStream(lzData, fp);
    fclose(fp);
    return ret;
}
// 旧接口保留兼容（小文件使用）
string LZ77Decompress(const string& pack)
{
    string out;
    out.reserve(pack.size() * 3);
    size_t pos = 0;
    size_t n = pack.size();
    while (pos < n)
    {
        unsigned char flagByte = static_cast<unsigned char>(pack[pos++]);
        for (int i = 0; i < 8 && pos < n; i++)
        {
            if (flagByte & (1 << i))
            {
                if (pos + 3 > n) break;
                int off = static_cast<unsigned char>(pack[pos]) | (static_cast<unsigned char>(pack[pos + 1]) << 8);
                int len = static_cast<unsigned char>(pack[pos + 2]);
                pos += 3;
                size_t start = out.size() - off;
                for (int k = 0; k < len; k++)
                    out += out[start + k];
            }
            else
            {
                out += pack[pos++];
            }
        }
    }
    return out;
}
string PackData(const string& fileRaw)
{
    string packedLZ77 = LZ77Compress(fileRaw);
    string out = XorCrypt(packedLZ77);
    return out;
}
string UnpackData(const string& packedData)
{
    string lzData = XorCrypt(packedData);
    string raw = LZ77Decompress(lzData);
    return raw;
}
string ReadFileBin(const string& gbkPath)
{
    wstring wPath = GBKToUnicode(gbkPath);
    FILE* fp = _wfopen(wPath.c_str(), L"rb");
    if (!fp) return "";
    string data;
    data.reserve(MAX_READ_SIZE * 2);
    char buf[MAX_READ_SIZE] = {0};
    size_t readLen;
    while ((readLen = fread(buf, 1, MAX_READ_SIZE, fp)) > 0)
    {
        data.append(buf, readLen);
        memset(buf, 0, MAX_READ_SIZE);
    }
    fclose(fp);
    return data;
}
bool WriteFileBin(const string& gbkPath, const string& data)
{
    wstring wPath = GBKToUnicode(gbkPath);
    FILE* fp = _wfopen(wPath.c_str(), L"wb");
    if (!fp) return false;
    size_t pos = 0;
    size_t total = data.size();
    while (pos < total)
    {
        size_t writeSize = min((size_t)MAX_READ_SIZE, total - pos);
        fwrite(data.c_str() + pos, 1, writeSize, fp);
        pos += writeSize;
    }
    fclose(fp);
    return true;
}
// 递归遍历文件夹获取全部文件
void RecurseScanFiles(const string& rootDir, vector<string>& outFiles)
{
    char searchPath[MAX_PATH] = {0};
    sprintf(searchPath, "%s\\*", rootDir.c_str());
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do
    {
        if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0)
            continue;
        string fullPath = rootDir + "\\" + findData.cFileName;
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            RecurseScanFiles(fullPath, outFiles);
        }
        else
        {
            outFiles.push_back(fullPath);
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
}
// 获取文件相对于根目录的相对路径，统一分隔符为 '/'
string GetRelativePath(const string& fullPath, const string& baseFolder)
{
    string rel = fullPath.substr(baseFolder.size() + 1);
    replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}
// 递归创建多级目录
bool CreateFolderDeep(string gbkFolder)
{
    if (_access(gbkFolder.c_str(), 0) == 0) return true;
    size_t pos = 0;
    while ((pos = gbkFolder.find('\\', pos + 1)) != string::npos)
    {
        string sub = gbkFolder.substr(0, pos);
        _mkdir(sub.c_str());
    }
    return _mkdir(gbkFolder.c_str()) == 0;
}
// 选择文件，修复单选文件失效bug
vector<string> SelectMultipleFiles(HWND hParent)
{
    vector<string> res;
    OPENFILENAMEA ofn = {0};
    char fileBuf[4096] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = hParent;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrFilter = "所有文件 (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "选择要压缩的文件（可多选）";
    g_bModalActive = true;
    bool ok = GetOpenFileNameA(&ofn);
    g_bModalActive = false;
    if (!ok) return res;
    // 判断单选还是多选
    if (fileBuf[strlen(fileBuf) + 1] == '\0')
    {
        res.emplace_back(fileBuf);
    }
    else
    {
        string folder = string(fileBuf);
        char* ptr = fileBuf + folder.size() + 1;
        while (*ptr)
        {
            string fullpath = folder + "\\" + ptr;
            res.push_back(fullpath);
            ptr += strlen(ptr) + 1;
        }
    }
    return res;
}
vector<string> SelectMultiFolder(HWND hParent)
{
    vector<string> dirList;
    g_bModalActive = true;
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hParent;
    bi.lpszTitle = "选择一个文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    g_bModalActive = false;
    if (!pidl)
    {
        PostStatus(hMainWnd, "取消选择文件夹");
        return dirList;
    }
    char buf[MAX_PATH] = {0};
    SHGetPathFromIDListA(pidl, buf);
    CoTaskMemFree(pidl);
    dirList.emplace_back(buf);
    PostStatus(hMainWnd, "选中目录:" + string(buf));
    return dirList;
}
string SelectFolder(HWND hParent)
{
    g_bModalActive = true;
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hParent;
    bi.lpszTitle = "选择解压输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    g_bModalActive = false;
    if (!pidl) return "";
    char buf[MAX_PATH] = {0};
    SHGetPathFromIDListA(pidl, buf);
    CoTaskMemFree(pidl);
    return string(buf);
}
string SelectSaveBszip(HWND hParent)
{
    OPENFILENAMEA ofn = {0};
    char buffer[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = hParent;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "BS压缩文件 (*.bszip)\0*.bszip\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = "bszip";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    ofn.lpstrTitle = "保存压缩包";
    g_bModalActive = true;
    bool ok = GetSaveFileNameA(&ofn);
    g_bModalActive = false;
    if (ok)
        return string(buffer);
    return "";
}
string SelectOpenBszip(HWND hParent)
{
    OPENFILENAMEA ofn = {0};
    char buffer[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = hParent;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "BS压缩文件 (*.bszip)\0*.bszip\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "选择bszip压缩包";
    g_bModalActive = true;
    bool ok = GetOpenFileNameA(&ofn);
    g_bModalActive = false;
    if (ok)
        return string(buffer);
    return "";
}
// 压缩核心逻辑（子线程运行）
bool DoCompressFileListCore(HWND hParent, const string& baseDir, const vector<string>& allFiles)
{
    if (allFiles.empty())
    {
        PostStatus(hParent, "没有选中文件");
        return false;
    }
    string savePath = SelectSaveBszip(hParent);
    if (savePath.empty())
    {
        PostStatus(hParent, "取消保存");
        return false;
    }
    PostStatus(hParent, "一共待压缩文件数：" + to_string(allFiles.size()));
    wstring wSave = GBKToUnicode(savePath);
    FILE* fpOut = _wfopen(wSave.c_str(), L"wb");
    if (!fpOut)
    {
        PostStatus(hParent, "创建压缩包失败");
        return false;
    }
    // 文件头：BSZIP:文件数::
    string head = "BSZIP:" + to_string(allFiles.size()) + "::";
    fwrite(head.data(), 1, head.size(), fpOut);
    for (size_t idx = 0; idx < allFiles.size(); idx++)
    {
        string filePath = allFiles[idx];
        string relPath = GetRelativePath(filePath, baseDir);
        PostStatus(hParent, "正在处理:" + relPath);
        string raw = ReadFileBin(filePath);
        string packedData = PackData(raw);
        int pathLen = (int)relPath.size();
        int dataLen = (int)packedData.size();
        // 写入：路径长度、路径字符串、加密数据长度、加密数据
        fwrite(&pathLen, sizeof(int), 1, fpOut);
        fwrite(relPath.data(), 1, pathLen, fpOut);
        fwrite(&dataLen, sizeof(int), 1, fpOut);
        fwrite(packedData.data(), 1, dataLen, fpOut);
        PostStatus(hParent, relPath + " 原始:" + to_string(raw.size()) + "字节,打包后:" + to_string(packedData.size()) + "字节");
    }
    fclose(fpOut);
    PostStatus(hParent, "压缩完成，输出文件：" + savePath);
    return true;
}
static inline int ReadInt(const char* p)
{
    int res;
    memcpy(&res, p, sizeof(int));
    return res;
}
// 读取bszip压缩包
bool ParseBszip(const string& zipPath, vector<ZipEntry>& entries)
{
    entries.clear();
    string zipData = ReadFileBin(zipPath);
    if (zipData.empty())
    {
        PostStatus(hMainWnd, "压缩包读取失败");
        return false;
    }
    size_t pos = zipData.find("BSZIP:");
    if (pos == string::npos)
    {
        PostStatus(hMainWnd, "不是合法BSZIP文件");
        return false;
    }
    pos += 6;
    size_t sep = zipData.find("::", pos);
    if (sep == string::npos)
    {
        PostStatus(hMainWnd, "文件头损坏");
        return false;
    }
    int fileCnt = atoi(zipData.substr(pos, sep - pos).c_str());
    pos = sep + 2;
    PostStatus(hMainWnd, "压缩包内文件总数：" + to_string(fileCnt));
    for (int i = 0; i < fileCnt; i++)
    {
        if (pos + sizeof(int) * 2 > zipData.size()) break;
        int pathLen = ReadInt(zipData.c_str() + pos);
        pos += sizeof(int);
        if (pos + pathLen > zipData.size()) break;
        string relPath = zipData.substr(pos, pathLen);
        pos += pathLen;
        int dataLen = ReadInt(zipData.c_str() + pos);
        pos += sizeof(int);
        if (pos + dataLen > zipData.size()) break;
        string packStr = zipData.substr(pos, dataLen);
        pos += dataLen;
        ZipEntry ent;
        ent.relativePath = relPath;
        ent.packedData = packStr;
        entries.push_back(ent);
        PostStatus(hMainWnd, "包内文件：" + relPath);
    }
    return true;
}
// 解压核心逻辑（子线程运行，流式解压，解决大文件崩溃）
bool ExtractSelectedCore(HWND hParent, const string& outDir)
{
    CreateFolderDeep(outDir);
    bool allOk = true;
    for (int i = 0; i < (int)g_previewEntries.size(); i++)
    {
        if (g_checkState[i] == 0) continue;
        string innerPath = g_previewEntries[i].relativePath;
        replace(innerPath.begin(), innerPath.end(), '/', '\\');
        string fullFilePath = outDir + "\\" + innerPath;
        // 创建文件所在文件夹
        size_t lastSlash = fullFilePath.find_last_of("\\");
        string fileDir = fullFilePath.substr(0, lastSlash);
        CreateFolderDeep(fileDir);
        // 流式解压写入磁盘，不占用大块内存
        bool ok = UnpackDataToFile(g_previewEntries[i].packedData, fullFilePath);
        if (ok)
            PostStatus(hParent, "解压成功:" + g_previewEntries[i].relativePath);
        else
        {
            PostStatus(hParent, "解压失败:" + g_previewEntries[i].relativePath);
            allOk = false;
        }
    }
    PostStatus(hParent, "选中文件解压完毕");
    return allOk;
}
// 压缩线程入口
unsigned int __stdcall CompressThreadProc(LPVOID pParam)
{
    CompressParam* p = (CompressParam*)pParam;
    DoCompressFileListCore(p->hParent, p->baseDir, p->fullFilePaths);
    delete p;
    return 0;
}
// 解压线程入口
unsigned int __stdcall ExtractThreadProc(LPVOID pParam)
{
    string* pOutDir = (string*)pParam;
    ExtractSelectedCore(hMainWnd, *pOutDir);
    delete pOutDir;
    return 0;
}
// 预览窗口过程函数
LRESULT CALLBACK PreviewWndProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        HWND hList = CreateWindowExA(0, "SysListView32", "", WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_VSCROLL,
            10, 10, 420, 300, hDlg, (HMENU)IDC_FILE_LIST, GetModuleHandleA(NULL), NULL);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LV_COLUMN lvCol = {0};
        lvCol.mask = LVCF_TEXT | LVCF_WIDTH;
        lvCol.cx = 400;
        lvCol.pszText = (LPSTR)"文件名";
        ListView_InsertColumn(hList, 0, &lvCol);
        g_checkState.assign(g_previewEntries.size(), 0);
        for (int i = 0; i < (int)g_previewEntries.size(); i++)
        {
            LVITEM lvItem = {0};
            lvItem.mask = LVIF_TEXT;
            lvItem.iItem = i;
            // UI只显示文件名，内部保存完整相对路径
            string fullPath = g_previewEntries[i].relativePath;
            size_t pos = fullPath.find_last_of("/\\");
            string showName = (pos == string::npos) ? fullPath : fullPath.substr(pos + 1);
            lvItem.pszText = (LPSTR)showName.c_str();
            ListView_InsertItem(hList, &lvItem);
            ListView_SetCheckState(hList, i, FALSE);
        }
        CreateWindowExA(0, "BUTTON", "全选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 320, 100, 30, hDlg, (HMENU)IDC_BTN_SELECTALL, GetModuleHandleA(NULL), nullptr);
        CreateWindowExA(0, "BUTTON", "取消全选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 120, 320, 100, 30, hDlg, (HMENU)IDC_BTN_UNSELECTALL, GetModuleHandleA(NULL), nullptr);
        CreateWindowExA(0, "BUTTON", "解压选中项", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 230, 320, 120, 30, hDlg, (HMENU)IDC_BTN_EXTRACT_SELECTED, GetModuleHandleA(NULL), nullptr);
        CreateWindowExA(0, "BUTTON", "关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 360, 320, 70, 30, hDlg, (HMENU)IDCANCEL, GetModuleHandleA(NULL), nullptr);
        break;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        HWND hList = GetDlgItem(hDlg, IDC_FILE_LIST);
        if (id == IDC_BTN_SELECTALL)
        {
            for (int i = 0; i < (int)g_previewEntries.size(); i++)
            {
                g_checkState[i] = 1;
                ListView_SetCheckState(hList, i, TRUE);
            }
        }
        else if (id == IDC_BTN_UNSELECTALL)
        {
            for (int i = 0; i < (int)g_previewEntries.size(); i++)
            {
                g_checkState[i] = 0;
                ListView_SetCheckState(hList, i, FALSE);
            }
        }
        else if (id == IDC_BTN_EXTRACT_SELECTED)
        {
            for (int i = 0; i < (int)g_previewEntries.size(); i++)
                g_checkState[i] = ListView_GetCheckState(hList, i) ? 1 : 0;
            string outDir = SelectFolder(hDlg);
            if (!outDir.empty())
            {
                string* pDir = new string(outDir);
                _beginthreadex(NULL, 0, ExtractThreadProc, pDir, 0, nullptr);
            }
        }
        else if (id == IDCANCEL)
        {
            DestroyWindow(hDlg);
            g_previewWnd = nullptr;
        }
        break;
    }
    case WM_NOTIFY:
    {
        NM_LISTVIEW* pNMLV = (NM_LISTVIEW*)lParam;
        if (pNMLV->hdr.idFrom == IDC_FILE_LIST && pNMLV->hdr.code == LVN_ITEMCHANGED)
        {
            int idx = pNMLV->iItem;
            HWND hList = GetDlgItem(hDlg, IDC_FILE_LIST);
            g_checkState[idx] = ListView_GetCheckState(hList, idx) ? 1 : 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hDlg);
        g_previewWnd = nullptr;
        break;
    }
    return DefWindowProcA(hDlg, uMsg, wParam, lParam);
}
void ShowZipPreview(HWND hParent)
{
    if (g_previewWnd != nullptr) return;
    WNDCLASSA previewClass = {0};
    previewClass.lpfnWndProc = PreviewWndProc;
    previewClass.hInstance = GetModuleHandleA(NULL);
    previewClass.lpszClassName = "PreviewClass";
    previewClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&previewClass);
    g_previewWnd = CreateWindowExA(0, previewClass.lpszClassName, "预览压缩包", WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 450, 400, hParent, NULL, GetModuleHandleA(NULL), NULL);
}
LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        hMainWnd = hWnd;
        SetConsoleOutputCP(CP_GBK);
        SetConsoleCP(CP_GBK);
        HWND hTitle = CreateWindowExA(0, "STATIC", "BSZip", WS_VISIBLE | WS_CHILD | SS_CENTER, 10, 10, 500, 40, hWnd, (HMENU)IDC_STATIC_TITLE, GetModuleHandleA(NULL), NULL);
        HFONT hTitleFont = CreateFontA(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "微软雅黑");
        SendMessageA(hTitle, WM_SETFONT, (WPARAM)hTitleFont, TRUE);
        HWND hBtnFile = CreateWindowExA(0, "BUTTON", "选择文件压缩", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 20, 70, 140, 60, hWnd, (HMENU)IDC_BTN_SELECTFILES, GetModuleHandleA(NULL), NULL);
        HWND hBtnDir = CreateWindowExA(0, "BUTTON", "选择文件夹压缩", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 170, 70, 140, 60, hWnd, (HMENU)IDC_BTN_SELECTDIRS, GetModuleHandleA(NULL), NULL);
        HWND hBtnDecomp = CreateWindowExA(0, "BUTTON", "解压缩包", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 320, 70, 140, 60, hWnd, (HMENU)IDC_BTN_DECOMPRESS, GetModuleHandleA(NULL), NULL);
        HFONT hBtnFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "微软雅黑");
        SendMessageA(hBtnFile, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
        SendMessageA(hBtnDir, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
        SendMessageA(hBtnDecomp, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
        hWndStatus = CreateWindowExA(0, "EDIT", "", WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 150, 500, 300, hWnd, (HMENU)IDC_EDIT_STATUS, GetModuleHandleA(NULL), NULL);
        HFONT hEditFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "微软雅黑");
        SendMessageA(hWndStatus, WM_SETFONT, (WPARAM)hEditFont, TRUE);
        if (!g_CommandLineZipFile.empty())
        {
            PostStatus(hWnd, "加载压缩包：" + g_CommandLineZipFile);
            size_t dotBSzip = g_CommandLineZipFile.find(".bszip");
            if (dotBSzip != string::npos)
            {
                ParseBszip(g_CommandLineZipFile, g_previewEntries);
                ShowZipPreview(hWnd);
            }
        }
        break;
    }
    case WM_USER_APPEND:
    {
        char* p = (char*)wParam;
        AppendStatus(string(p));
        delete[] p;
        break;
    }
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDC_BTN_SELECTFILES:
        {
            vector<string> files = SelectMultipleFiles(hWnd);
            if (!files.empty())
            {
                // 文件模式：取目录部分作为基准目录
                string baseDir;
                size_t pos = files[0].find_last_of("\\");
                baseDir = files[0].substr(0, pos);
                CompressParam* param = new CompressParam;
                param->hParent = hWnd;
                param->baseDir = baseDir;
                param->fullFilePaths = files;
                _beginthreadex(NULL, 0, CompressThreadProc, param, 0, nullptr);
            }
            break;
        }
        case IDC_BTN_SELECTDIRS:
        {
            vector<string> dirs = SelectMultiFolder(hWnd);
            vector<string> allFiles;
            if (!dirs.empty())
            {
                string baseDir = dirs[0];
                RecurseScanFiles(baseDir, allFiles);
                if (!allFiles.empty())
                {
                    CompressParam* param = new CompressParam;
                    param->hParent = hWnd;
                    param->baseDir = baseDir;
                    param->fullFilePaths = allFiles;
                    _beginthreadex(NULL, 0, CompressThreadProc, param, 0, nullptr);
                }
            }
            break;
        }
        case IDC_BTN_DECOMPRESS:
        {
            string zipPath = SelectOpenBszip(hWnd);
            if (!zipPath.empty())
            {
                size_t dotBSzip = zipPath.find(".bszip");
                if (dotBSzip != string::npos)
                {
                    ParseBszip(zipPath, g_previewEntries);
                    ShowZipPreview(hWnd);
                }
            }
            break;
        }
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcA(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    if (lpCmdLine && strlen(lpCmdLine) > 0)
    {
        string arg(lpCmdLine);
        while (arg.size() && (arg[0] == ' ' || arg[0] == '"')) arg.erase(arg.begin());
        while (arg.size() && (arg.back() == ' ' || arg.back() == '"')) arg.pop_back();
        size_t dotZip = arg.find(".zip");
        size_t dotBSzip = arg.find(".bszip");
        if (dotZip != string::npos)
        {
            LaunchZipPreviewExe(arg);
            return 0;
        }
        else if (dotBSzip != string::npos)
        {
            g_CommandLineZipFile = arg;
        }
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW_A);
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION_A);
    if (!RegisterClassA(&wc)) return 0;
    HWND hWnd = CreateWindowExA(0, CLASS_NAME, "BSZip", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 540, 520, NULL, NULL, hInstance, NULL);
    if (!hWnd) return 0;
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
