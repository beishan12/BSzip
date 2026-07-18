#define INITGUID
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <direct.h>
#include <cstdlib>
#include <cstring>
#include <process.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma warning(disable:4996)
using namespace std;
#undef UNICODE
#undef _UNICODE
#define CP_GBK 936
#define IDC_FILE_LIST     201
#define IDC_BTN_SELECTALL 202
#define IDC_BTN_UNSELECTALL 203
#define IDC_BTN_EXTRACT_SELECTED 204
#define IDCANCEL 2
#define MAX_READ_SIZE 1024 * 1024
HWND hPreviewMainWnd;
string g_zipFilePath;
struct ZipItem
{
    string displayName;
    string fullInnerPath;
};
vector<ZipItem> g_fileList;
vector<char> g_checkState;
string GetExeDirectory()
{
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    string path(buf);
    size_t pos = path.find_last_of("\\");
    return path.substr(0, pos);
}
wstring GBKToUnicode(const string& gbkStr)
{
    int len = MultiByteToWideChar(CP_GBK,0,gbkStr.c_str(),-1,NULL,0);
    wchar_t* wstr = new wchar_t[len];
    MultiByteToWideChar(CP_GBK,0,gbkStr.c_str(),-1,wstr,len);
    wstring res = wstr;
    delete[] wstr;
    return res;
}
string UnicodeToGBK(const wstring& unicodeStr)
{
    int len = WideCharToMultiByte(CP_GBK,0,unicodeStr.c_str(),-1,NULL,0,NULL,NULL);
    char* str = new char[len];
    WideCharToMultiByte(CP_GBK,0,unicodeStr.c_str(),-1,str,len,NULL,NULL);
    string res = str;
    delete[] str;
    return res;
}
bool CreateFolderDeep(string gbkFolder)
{
    if (_access(gbkFolder.c_str(),0)==0) return true;
    size_t pos = 0;
    while((pos = gbkFolder.find('\\',pos+1)) != string::npos)
    {
        string sub = gbkFolder.substr(0,pos);
        _mkdir(sub.c_str());
    }
    return _mkdir(gbkFolder.c_str()) == 0;
}
HRESULT EnumZipItems(IShellFolder *pFolder, LPCSTR relBase, vector<ZipItem>& outList)
{
    HRESULT hr;
    IEnumIDList *pEnum = nullptr;
    hr = pFolder->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &pEnum);
    if(FAILED(hr)) return hr;

    LPITEMIDLIST pidl;
    ULONG fetched;
    while(pEnum->Next(1, &pidl, &fetched) == S_OK)
    {
        STRRET strRet;
        pFolder->GetDisplayNameOf(pidl, SHGDN_NORMAL, &strRet);
        WCHAR wszName[MAX_PATH] = {0};
        StrRetToBufW(&strRet, pidl, wszName, MAX_PATH);
        string fileName = UnicodeToGBK(wszName);

        SFGAOF attr = SFGAO_FOLDER;
        const ITEMIDLIST* pConstPidl = pidl;
        pFolder->GetAttributesOf(1, &pConstPidl, &attr);

        if(attr & SFGAO_FOLDER)
        {
            IShellFolder *pSubFolder = nullptr;
            hr = pFolder->BindToObject(pidl, NULL, IID_IShellFolder, (void**)&pSubFolder);
            if(SUCCEEDED(hr) && pSubFolder)
            {
                string newRel = string(relBase) + fileName + "/";
                EnumZipItems(pSubFolder, newRel.c_str(), outList);
                pSubFolder->Release();
            }
        }
        else
        {
            ZipItem item;
            item.fullInnerPath = string(relBase) + fileName;
            size_t pos = item.fullInnerPath.find_last_of("/\\");
            item.displayName = (pos == string::npos) ? item.fullInnerPath : item.fullInnerPath.substr(pos+1);
            outList.push_back(item);
        }
        CoTaskMemFree(pidl);
    }
    pEnum->Release();
    return S_OK;
}
bool ParseZipFile(const string& zipPath, vector<ZipItem>& fileList)
{
    fileList.clear();
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) return false;
    WCHAR wszZipPath[MAX_PATH];
    wcscpy(wszZipPath, GBKToUnicode(zipPath).c_str());
    LPITEMIDLIST pidlZip = nullptr;
    SFGAOF outAttrib = 0;
    hr = SHParseDisplayName(wszZipPath, NULL, &pidlZip, 0, &outAttrib);
    if(FAILED(hr) || pidlZip == nullptr)
    {
        MessageBoxA(NULL, "解析Zip失败，文件损坏", "错误", MB_ICONERROR);
        CoUninitialize();
        return false;
    }
    IShellFolder *pDesktop = nullptr;
    SHGetDesktopFolder(&pDesktop);
    IShellFolder *pZipFolder = nullptr;
    hr = pDesktop->BindToObject(pidlZip, NULL, IID_IShellFolder, (void**)&pZipFolder);
    if(FAILED(hr))
    {
        MessageBoxA(NULL, "打开Zip目录失败", "错误", MB_ICONERROR);
        CoTaskMemFree(pidlZip);
        pDesktop->Release();
        CoUninitialize();
        return false;
    }
    EnumZipItems(pZipFolder, "", fileList);
    pZipFolder->Release();
    CoTaskMemFree(pidlZip);
    pDesktop->Release();
    CoUninitialize();
    if(fileList.empty())
    {
        MessageBoxA(NULL, "压缩包内无文件", "提示", MB_OK);
        return false;
    }
    return true;
}
// 删除目录工具
bool RemoveDir(const string& folder)
{
    string cmd = "rmdir /s /q \"" + folder + "\"";
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(STARTUPINFOA);
    BOOL ok = CreateProcessA(NULL, (LPSTR)cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if(ok)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return true;
}
// 获取系统错误文本
string GetWinErrorMsg(DWORD errCode)
{
    char buf[1024] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, errCode, 0, buf, 1024, NULL);
    return string(buf);
}
// 带重试安全复制，解决文件占用无法复制
bool SafeCopyFile(const string& src, const string& dst, int retryTimes = 3, DWORD waitMs = 500)
{
    for (int i = 0; i < retryTimes; i++)
    {
        if (CopyFileA(src.c_str(), dst.c_str(), FALSE))
            return true;
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_FILE_NOT_FOUND)
        {
            Sleep(waitMs);
            continue;
        }
        return false;
    }
    return false;
}
unsigned int __stdcall ExtractThreadProc(LPVOID pParam)
{
    pair<string, string>* pPair = (pair<string, string>*)pParam;
    string zipPath = pPair->first;
    string outDir = pPair->second;
    string exeDir = GetExeDirectory();
    string full7zaPath = exeDir + "\\7za.exe";
    string tempDir = exeDir + "\\_tmp_extract";
    CreateFolderDeep(outDir);
    CreateFolderDeep(tempDir);
    // 校验7za存在
    if (_access(full7zaPath.c_str(), 0) != 0)
    {
        MessageBoxA(NULL, "7za.exe 缺失，请放在程序同目录", "错误", MB_ICONERROR);
        delete pPair;
        return 0;
    }
    // 7za解压命令
    string cmdLine = "\"" + full7zaPath + "\" x -y \"" + zipPath + "\" -o\"" + tempDir + "\"";
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(STARTUPINFOA);
    BOOL ok = CreateProcessA(NULL, (LPSTR)cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok)
    {
        DWORD err = GetLastError();
        MessageBoxA(NULL, ("启动7za失败：" + GetWinErrorMsg(err)).c_str(), "错误", MB_ICONERROR);
        RemoveDir(tempDir);
        delete pPair;
        return 0;
    }
    // 等待7za完全退出释放文件句柄
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode != 0)
    {
        MessageBoxA(NULL, "7za解压压缩包失败", "错误", MB_ICONERROR);
        RemoveDir(tempDir);
        delete pPair;
        return 0;
    }
    bool allOk = true;
    // 遍历复制选中文件
    for (int i = 0; i < (int)g_fileList.size(); i++)
    {
        if (!g_checkState[i]) continue;
        string srcFile = tempDir + "\\" + g_fileList[i].fullInnerPath;
        string dstFile = outDir + "\\" + g_fileList[i].fullInnerPath;
        size_t pos = dstFile.find_last_of("\\");
        CreateFolderDeep(dstFile.substr(0, pos));
        // 校验源文件是否存在
        if (_access(srcFile.c_str(), 0) != 0)
        {
            MessageBoxA(NULL, ("临时目录缺失文件：" + srcFile).c_str(), "警告", MB_OK);
            allOk = false;
            continue;
        }
        // 重试复制解决文件占用
        if (!SafeCopyFile(srcFile, dstFile))
        {
            DWORD err = GetLastError();
            string errInfo = "复制失败：" + srcFile + "\n错误信息：" + GetWinErrorMsg(err);
            MessageBoxA(NULL, errInfo.c_str(), "复制错误", MB_ICONERROR);
            allOk = false;
        }
    }
    // 全部复制完成再删除临时目录
    RemoveDir(tempDir);
    delete pPair;
    if (allOk)
        MessageBoxA(hPreviewMainWnd, "选中文件解压完成", "成功", MB_OK);
    else
        MessageBoxA(hPreviewMainWnd, "部分文件复制失败", "警告", MB_ICONERROR);
    return 0;
}
string SelectFolder(HWND hParent)
{
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hParent;
    bi.lpszTitle = "选择解压输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if(!pidl) return "";
    char buf[MAX_PATH] = {0};
    SHGetPathFromIDListA(pidl, buf);
    CoTaskMemFree(pidl);
    return string(buf);
}
LRESULT CALLBACK PreviewProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg)
    {
    case WM_CREATE:
    {
        HWND hList = CreateWindowExA(0, "SysListView32", "", WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_VSCROLL,
            10, 10, 420, 300, hDlg, (HMENU)IDC_FILE_LIST, GetModuleHandleA(NULL), nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LV_COLUMN lv = {0};
        lv.mask = LVCF_TEXT | LVCF_WIDTH;
        lv.cx = 400;
        lv.pszText = (LPSTR)"文件名";
        ListView_InsertColumn(hList,0,&lv);
        g_checkState.assign(g_fileList.size(),0);
        for(int i=0;i<(int)g_fileList.size();i++)
        {
            LVITEM item = {0};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = (LPSTR)g_fileList[i].displayName.c_str();
            ListView_InsertItem(hList, &item);
            ListView_SetCheckState(hList,i,FALSE);
        }
        CreateWindowExA(0,"BUTTON","全选",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,10,320,100,30,hDlg,(HMENU)IDC_BTN_SELECTALL,GetModuleHandleA(NULL),nullptr);
        CreateWindowExA(0,"BUTTON","取消全选",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,120,320,100,30,hDlg,(HMENU)IDC_BTN_UNSELECTALL,GetModuleHandleA(NULL),nullptr);
        CreateWindowExA(0,"BUTTON","解压选中项",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,230,320,120,30,hDlg,(HMENU)IDC_BTN_EXTRACT_SELECTED,GetModuleHandleA(NULL),nullptr);
        CreateWindowExA(0,"BUTTON","关闭",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,360,320,70,30,hDlg,(HMENU)IDCANCEL,GetModuleHandleA(NULL),nullptr);
        break;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        HWND hList = GetDlgItem(hDlg, IDC_FILE_LIST);
        if(id == IDC_BTN_SELECTALL)
        {
            for(int i=0;i<(int)g_fileList.size();i++)
            {
                g_checkState[i] = 1;
                ListView_SetCheckState(hList,i,TRUE);
            }
        }
        else if(id == IDC_BTN_UNSELECTALL)
        {
            for(int i=0;i<(int)g_fileList.size();i++)
            {
                g_checkState[i] = 0;
                ListView_SetCheckState(hList,i,FALSE);
            }
        }
        else if(id == IDC_BTN_EXTRACT_SELECTED)
        {
            for(int i=0;i<(int)g_fileList.size();i++)
                g_checkState[i] = ListView_GetCheckState(hList,i) ? 1 :0;
            string outDir = SelectFolder(hDlg);
            if(!outDir.empty())
            {
                auto* pPair = new pair<string, string>(g_zipFilePath, outDir);
                _beginthreadex(nullptr,0,ExtractThreadProc,pPair,0,nullptr);
            }
        }
        else if(id == IDCANCEL)
        {
            DestroyWindow(hDlg);
            PostQuitMessage(0);
        }
        break;
    }
    case WM_NOTIFY:
    {
        NM_LISTVIEW* pNMLV = (NM_LISTVIEW*)lParam;
        if(pNMLV->hdr.idFrom == IDC_FILE_LIST && pNMLV->hdr.code == LVN_ITEMCHANGED)
        {
            int idx = pNMLV->iItem;
            HWND hList = GetDlgItem(hDlg, IDC_FILE_LIST);
            g_checkState[idx] = ListView_GetCheckState(hList, idx) ? 1 : 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hDlg);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(hDlg, uMsg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    if(lpCmdLine == nullptr || strlen(lpCmdLine) <= 0)
    {
        MessageBoxA(NULL, "请拖拽zip文件到本程序", "参数错误", MB_ICONERROR);
        return -1;
    }
    string arg(lpCmdLine);
    while(!arg.empty() && (arg[0] == ' ' || arg[0] == '"')) arg.erase(arg.begin());
    while(!arg.empty() && (arg.back() == ' ' || arg.back() == '"')) arg.pop_back();
    g_zipFilePath = arg;
    if(!ParseZipFile(g_zipFilePath, g_fileList)) return -1;
    g_checkState.assign(g_fileList.size(),0);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ZipPreviewClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassA(&wc);
    hPreviewMainWnd = CreateWindowExA(0, wc.lpszClassName, "Zip预览工具", WS_POPUPWINDOW | WS_CAPTION,
        CW_USEDEFAULT, CW_USEDEFAULT, 450, 400, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hPreviewMainWnd, SW_SHOW);
    UpdateWindow(hPreviewMainWnd);
    MSG msg;
    while(GetMessageA(&msg, nullptr,0,0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
