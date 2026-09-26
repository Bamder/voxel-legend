#include "data_editor.hpp"
#include "../world/data_pack.hpp"
#include "../world/wear.hpp"
#include "../plugin/plugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dataed {
namespace {

enum : int {
    IDC_CAT = 100,
    IDC_LIST,
    IDC_SAVE,
    IDC_RELOAD,
    IDC_STATUS,
    IDC_ID,
    IDC_DISPLAY,
    IDC_KIND,
    IDC_STACK,
    IDC_HARD,
    IDC_DUR,
    IDC_EFF,
    IDC_CHARGE,
    IDC_COOLDOWN,
    IDC_WEIGHT,
    IDC_RESIST,
    IDC_HARVEST,
    IDC_TAG0 = 120,
    IDC_HNEED0 = 130,
    IDC_DROP1_ITEM = 140,
    IDC_DROP1_COUNT,
    IDC_DROP2_ITEM,
    IDC_DROP2_COUNT,
    IDC_PREVIEW,
    IDC_CRACK_FOLDS = 150,
    IDC_CRACK_R,
    IDC_CRACK_G,
    IDC_CRACK_B,
    IDC_WEAR = 160,
    IDC_E0 = 200
};

HWND g_wnd = nullptr;
HWND g_cat = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
HWND g_preview = nullptr;
HWND g_hover = nullptr;
HFONT g_font = nullptr;
HBRUSH g_hoverBrush = nullptr;
std::wstring g_hoverText;
uintptr_t g_hoverKey = 0;
bool g_loading = false;
bool g_dirty = false;
int g_catIdx = 0;
int g_tagN = 0;
const data::TagInfo* g_tags = nullptr;
constexpr UINT kHoverTimer = 1;

struct EntField {
    const wchar_t* label;
    float vitals::Rates::* member;
};
const EntField kEntFields[] = {
    { L"Mine stamina / sec",     &vitals::Rates::mineStaminaPerSec },
    { L"Sprint stamina / sec",   &vitals::Rates::sprintStaminaPerSec },
    { L"Jump stamina burst",     &vitals::Rates::jumpStaminaBurst },
    { L"Swim stamina / sec",     &vitals::Rates::swimStaminaPerSec },
    { L"Stamina regen (fast)",   &vitals::Rates::stamRegenFast },
    { L"Stamina regen (slow)",   &vitals::Rates::stamRegenSlow },
    { L"Empty recover delay",    &vitals::Rates::emptyRecoverDelay },
    { L"Run cardio / sec",       &vitals::Rates::runCardioPerSec },
    { L"Jump-chain window",      &vitals::Rates::jumpChainWindow },
    { L"Jump cardio base",       &vitals::Rates::jumpCardioBase },
    { L"Jump cardio growth",     &vitals::Rates::jumpCardioGrowth },
    { L"Jump cardio cap",        &vitals::Rates::jumpCardioCap },
    { L"Empty speed mul",        &vitals::Rates::emptySpeedMul },
    { L"Empty jump mul",         &vitals::Rates::emptyJumpMul },
};

std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void setFont(HWND h) {
    if (g_font && h) SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    DWORD extra = 0;
    if (wcscmp(cls, L"STATIC") == 0) extra = SS_ENDELLIPSIS | SS_NOPREFIX;
    HWND c = CreateWindowExW(
        (wcscmp(cls, L"EDIT") == 0 || wcscmp(cls, L"COMBOBOX") == 0) ? WS_EX_CLIENTEDGE : 0,
        cls, text, WS_CHILD | WS_VISIBLE | style | extra,
        x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    setFont(c);
    return c;
}

int measureText(HWND h, const wchar_t* s) {
    if (!h || !s || !s[0]) return 0;
    HDC dc = GetDC(h);
    HFONT old = g_font ? (HFONT)SelectObject(dc, g_font) : nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (old) SelectObject(dc, old);
    ReleaseDC(h, dc);
    return sz.cx;
}

LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_hoverBrush ? g_hoverBrush : (HBRUSH)(COLOR_INFOBK + 1));
        FrameRect(dc, &rc, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        if (g_font) SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(28, 28, 28));
        RECT tr = rc;
        InflateRect(&tr, -8, -3);
        DrawTextW(dc, g_hoverText.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

void hideHover() {
    g_hoverKey = 0;
    g_hoverText.clear();
    if (g_hover && IsWindowVisible(g_hover)) ShowWindow(g_hover, SW_HIDE);
}

void showHoverAt(int screenX, int screenY, const std::wstring& text, uintptr_t key) {
    if (text.empty()) { hideHover(); return; }
    if (!g_hover) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HoverProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VLHoverTip";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        if (!g_hoverBrush) g_hoverBrush = CreateSolidBrush(RGB(255, 248, 196));
        g_hover = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
            L"VLHoverTip", L"", WS_POPUP,
            0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    int tw = measureText(g_hover ? g_hover : g_wnd, text.c_str());
    int w = tw + 18;
    int h = 26;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (screenX + w > wa.right) screenX = wa.right - w;
    if (screenY + h > wa.bottom) screenY = screenY - h - 22;
    if (screenX < wa.left) screenX = wa.left;
    if (screenY < wa.top) screenY = wa.top;

    bool same = (g_hoverKey == key && g_hoverText == text && IsWindowVisible(g_hover));
    g_hoverKey = key;
    if (g_hoverText != text) {
        g_hoverText = text;
        InvalidateRect(g_hover, nullptr, TRUE);
    }
    if (same) return;
    SetWindowPos(g_hover, HWND_TOPMOST, screenX, screenY, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ShowWindow(g_hover, SW_SHOWNA);
}

bool listItemAt(HWND list, POINT parentClient, int& idx, std::wstring& text, RECT& itemScreen) {
    POINT p = parentClient;
    MapWindowPoints(g_wnd, list, &p, 1);
    LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(p.x, p.y));
    if (HIWORD(hit)) return false;
    idx = (int)LOWORD(hit);
    int len = (int)SendMessageW(list, LB_GETTEXTLEN, idx, 0);
    if (len <= 0) return false;
    text.assign((size_t)len + 1, 0);
    SendMessageW(list, LB_GETTEXT, idx, (LPARAM)text.data());
    text.resize(wcslen(text.c_str()));
    RECT ir{};
    if (SendMessageW(list, LB_GETITEMRECT, idx, (LPARAM)&ir) == LB_ERR) return false;
    MapWindowPoints(list, nullptr, (POINT*)&ir, 2);
    itemScreen = ir;
    return true;
}

bool textOverflows(HWND h, const wchar_t* text, int pad) {
    if (!h || !text || !text[0]) return false;
    RECT rc;
    GetClientRect(h, &rc);
    int avail = rc.right - rc.left - pad;
    return avail > 0 && measureText(h, text) > avail;
}

void pollHover() {
    if (!g_wnd || !IsWindow(g_wnd)) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(g_wnd, &wr);
    if (!PtInRect(&wr, pt)) { hideHover(); return; }

    auto hoverList = [&](HWND list) -> bool {
        if (!list || !IsWindowVisible(list)) return false;
        RECT lr;
        GetWindowRect(list, &lr);
        if (!PtInRect(&lr, pt)) return false;
        POINT parent = pt;
        ScreenToClient(g_wnd, &parent);
        int idx = -1;
        std::wstring text;
        RECT item{};
        if (!listItemAt(list, parent, idx, text, item)) { hideHover(); return true; }
        if (!textOverflows(list, text.c_str(), 12)) { hideHover(); return true; }
        showHoverAt(item.left, item.bottom + 4, text, ((uintptr_t)list << 8) ^ (uintptr_t)(idx + 1));
        return true;
    };
    if (hoverList(g_list) || hoverList(g_cat)) return;

    for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c)) continue;
        RECT rc;
        GetWindowRect(c, &rc);
        if (!PtInRect(&rc, pt)) continue;
        wchar_t cls[32]{};
        GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, L"Static") != 0 && _wcsicmp(cls, L"Button") != 0) continue;
        int n = GetWindowTextLengthW(c);
        if (n <= 0) continue;
        std::wstring text((size_t)n + 1, 0);
        GetWindowTextW(c, text.data(), n + 1);
        text.resize(wcslen(text.c_str()));
        if (!textOverflows(c, text.c_str(), 4)) { hideHover(); return; }
        showHoverAt(rc.left, rc.bottom + 4, text, (uintptr_t)c);
        return;
    }
    hideHover();
}

std::string getText(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    if (!h) return {};
    int n = GetWindowTextLengthW(h);
    std::wstring w((size_t)n + 1, 0);
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(wcslen(w.c_str()));
    return toUtf8(w);
}

void setText(int id, const std::string& s) {
    HWND h = GetDlgItem(g_wnd, id);
    if (h) SetWindowTextW(h, toWide(s).c_str());
}

float getFloat(int id) { return (float)std::atof(getText(id).c_str()); }

int getInt(int id) {
    int n = std::atoi(getText(id).c_str());
    return n;
}

void setFloat(int id, float v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4g", v);
    setText(id, buf);
}

bool checked(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setCheck(int id, bool on) {
    HWND h = GetDlgItem(g_wnd, id);
    if (h) SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void showObj(bool on) {
    const int ids[] = {
        IDC_KIND, IDC_STACK, IDC_HARD, IDC_DUR, IDC_EFF, IDC_CHARGE, IDC_COOLDOWN, IDC_WEIGHT, IDC_WEAR, IDC_RESIST,
        IDC_CRACK_FOLDS, IDC_CRACK_R, IDC_CRACK_G, IDC_CRACK_B,
        IDC_HARVEST, IDC_DROP1_ITEM, IDC_DROP1_COUNT, IDC_DROP2_ITEM, IDC_DROP2_COUNT, IDC_PREVIEW
    };
    for (int id : ids) {
        HWND h = GetDlgItem(g_wnd, id);
        if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    }
    for (int i = 0; i < g_tagN; i++) {
        HWND a = GetDlgItem(g_wnd, IDC_TAG0 + i);
        HWND b = GetDlgItem(g_wnd, IDC_HNEED0 + i);
        if (a) ShowWindow(a, on ? SW_SHOW : SW_HIDE);
        if (b) ShowWindow(b, on ? SW_SHOW : SW_HIDE);
    }
    for (int id = 305; id < 330; id++) {
        HWND h = GetDlgItem(g_wnd, id);
        if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    }
}

void showEnt(bool on) {
    for (int i = 0; i < (int)(sizeof(kEntFields) / sizeof(kEntFields[0])); i++) {
        HWND a = GetDlgItem(g_wnd, 400 + i);
        HWND b = GetDlgItem(g_wnd, IDC_E0 + i);
        if (a) ShowWindow(a, on ? SW_SHOW : SW_HIDE);
        if (b) ShowWindow(b, on ? SW_SHOW : SW_HIDE);
    }
}

bool matchCat(const data::ObjectDef& o, int cat) {
    if (cat == 0) return o.kind == loot::Kind::Block;
    if (cat == 1) return o.kind == loot::Kind::Tool;
    if (cat == 2) return o.kind == loot::Kind::Item;
    return false;
}

std::vector<std::string> listIds() {
    std::vector<std::string> ids;
    if (g_catIdx == 3) {
        for (const data::EntityDef& e : data::entities()) ids.push_back(e.id);
        return ids;
    }
    for (const data::ObjectDef& o : data::objects()) {
        if (matchCat(o, g_catIdx)) ids.push_back(o.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string selectedId() {
    int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (i < 0) return {};
    auto ids = listIds();
    if (i >= (int)ids.size()) return {};
    return ids[i];
}

void setStatus(const wchar_t* s) {
    if (g_status) SetWindowTextW(g_status, s);
}

void updateTitle() {
    std::wstring t = L"VOXEL LEGEND — Data Pack";
    if (g_dirty) t += L" *";
    if (g_wnd) SetWindowTextW(g_wnd, t.c_str());
}

void fillDropCombo(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    if (!h) return;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    std::vector<std::string> ids;
    for (const data::ObjectDef& o : data::objects()) {
        if (o.id != data::kHandId && o.id != data::kSodId) ids.push_back(o.id);
    }
    std::sort(ids.begin(), ids.end());
    for (const std::string& idn : ids)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)toWide(idn).c_str());
}

void setComboId(int id, const std::string& value) {
    HWND h = GetDlgItem(g_wnd, id);
    if (!h) return;
    if (value.empty()) {
        SendMessageW(h, CB_SETCURSEL, 0, 0);
        return;
    }
    int n = (int)SendMessageW(h, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        int len = (int)SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
        std::wstring w((size_t)len + 1, 0);
        SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)w.data());
        w.resize(wcslen(w.c_str()));
        if (toUtf8(w) == value) {
            SendMessageW(h, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SetWindowTextW(h, toWide(value).c_str());
}

std::string comboId(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    if (!h) return {};
    int i = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (i <= 0) {
        if (i == 0) return {};
        return getText(id);
    }
    int len = (int)SendMessageW(h, CB_GETLBTEXTLEN, i, 0);
    std::wstring w((size_t)len + 1, 0);
    SendMessageW(h, CB_GETLBTEXT, i, (LPARAM)w.data());
    w.resize(wcslen(w.c_str()));
    return toUtf8(w);
}

void updatePreview() {
    if (!g_preview || g_catIdx == 3) return;
    data::ObjectDef* hand = data::findObject(data::kHandId);
    float hh = hand ? hand->hardness : 2.5f;
    float he = hand ? hand->efficiency : 1.0f;
    float bh = getFloat(IDC_HARD);
    float bd = getFloat(IDC_DUR);
    loot::Kind kind = data::parseKind(getText(IDC_KIND));
    if (g_catIdx == 1) kind = loot::Kind::Tool;

    wchar_t buf[256];
    auto timing = [](float t) { return loot::clampMineTiming(t); };
    float resist = getFloat(IDC_RESIST);
    if (resist < 0.0f) resist = 0.0f;
    if (resist > 1.0f) resist = 1.0f;
    if (kind == loot::Kind::Tool) {
        float ch = timing(getFloat(IDC_CHARGE));
        float cd = timing(getFloat(IDC_COOLDOWN));
        swprintf(buf, 256, L"Dmg = eff * max(Hgap,0) * resist.  Wrong-tool resist is on the block.  Windup %.2f s, recovery %.2f s",
                 ch, cd);
    } else {
        float dmg = he * (hh > bh ? (hh - bh) : 0.0f); // hand resist = 1
        float charge = timing(hand ? hand->chargeSec : 0.5f);
        float cool = timing(hand ? hand->cooldownSec : 0.5f);
        float cycle = charge + cool;
        if (dmg <= 1e-6f)
            swprintf(buf, 256, L"Bare hand cannot break this (hardness gap <= 0).  Durability %.1f  Wrong-tool resist %.2f",
                     bd, resist);
        else {
            int n = (int)std::ceil(bd / dmg - 1e-4f);
            swprintf(buf, 256, L"Hand dmg %.2f / attempt (~%d, ~%.1f s).  Wrong-tool resist %.2f  (windup+recovery %.2f s)",
                     dmg, n, (float)n * cycle, resist, cycle);
        }
    }
    SetWindowTextW(g_preview, buf);
}

void loadObject(data::ObjectDef& o) {
    g_loading = true;
    setText(IDC_ID, o.id);
    setText(IDC_DISPLAY, o.display);
    HWND kind = GetDlgItem(g_wnd, IDC_KIND);
    if (kind) {
        int idx = 0;
        if (o.kind == loot::Kind::Tool) idx = 1;
        else if (o.kind == loot::Kind::Item) idx = 2;
        SendMessageW(kind, CB_SETCURSEL, idx, 0);
    }
    setText(IDC_STACK, std::to_string((int)o.maxStack));
    setFloat(IDC_HARD, o.hardness);
    setFloat(IDC_DUR, o.durability);
    setFloat(IDC_EFF, o.efficiency);
    setFloat(IDC_CHARGE, o.chargeSec);
    setFloat(IDC_COOLDOWN, o.cooldownSec);
    setFloat(IDC_WEIGHT, o.weight);
    if (HWND wear = GetDlgItem(g_wnd, IDC_WEAR)) {
        int sel = 0;
        if (o.wear >= 0 && o.wear < wear::Count) sel = o.wear + 1;
        SendMessageW(wear, CB_SETCURSEL, sel, 0);
    }
    setFloat(IDC_RESIST, o.wrongResist);
    setText(IDC_CRACK_FOLDS, std::to_string(loot::clampCrackFolds(o.crackFolds)));
    setText(IDC_CRACK_R, std::to_string((int)o.crackR));
    setText(IDC_CRACK_G, std::to_string((int)o.crackG));
    setText(IDC_CRACK_B, std::to_string((int)o.crackB));
    for (int i = 0; i < g_tagN; i++) {
        setCheck(IDC_TAG0 + i, (o.tags & g_tags[i].bit) == g_tags[i].bit);
        setCheck(IDC_HNEED0 + i, o.harvest && (o.harvestNeed & g_tags[i].bit) == g_tags[i].bit);
    }
    setCheck(IDC_HARVEST, o.harvest);
    fillDropCombo(IDC_DROP1_ITEM);
    fillDropCombo(IDC_DROP2_ITEM);
    setComboId(IDC_DROP1_ITEM, o.nDrops > 0 ? o.drops[0].itemId : "");
    setText(IDC_DROP1_COUNT, o.nDrops > 0 ? std::to_string((int)o.drops[0].count) : "1");
    setComboId(IDC_DROP2_ITEM, o.nDrops > 1 ? o.drops[1].itemId : "");
    setText(IDC_DROP2_COUNT, o.nDrops > 1 ? std::to_string((int)o.drops[1].count) : "1");
    g_loading = false;
    updatePreview();
}

void loadEntity(data::EntityDef& e) {
    g_loading = true;
    setText(IDC_ID, e.id);
    setText(IDC_DISPLAY, e.display);
    for (int i = 0; i < (int)(sizeof(kEntFields) / sizeof(kEntFields[0])); i++)
        setFloat(IDC_E0 + i, e.rates.*(kEntFields[i].member));
    g_loading = false;
}

void flushObject(data::ObjectDef& o) {
    o.display = getText(IDC_DISPLAY);
    HWND kind = GetDlgItem(g_wnd, IDC_KIND);
    int ki = kind ? (int)SendMessageW(kind, CB_GETCURSEL, 0, 0) : 0;
    if (ki == 1) o.kind = loot::Kind::Tool;
    else if (ki == 2) o.kind = loot::Kind::Item;
    else o.kind = loot::Kind::Block;
    int stack = getInt(IDC_STACK);
    if (stack < 1) stack = 1;
    if (stack > 255) stack = 255;
    o.maxStack = (uint8_t)stack;
    o.hardness = getFloat(IDC_HARD);
    o.durability = getFloat(IDC_DUR);
    o.efficiency = getFloat(IDC_EFF);
    o.chargeSec = getFloat(IDC_CHARGE);
    o.cooldownSec = getFloat(IDC_COOLDOWN);
    o.weight = getFloat(IDC_WEIGHT);
    o.wear = -1;
    if (HWND wear = GetDlgItem(g_wnd, IDC_WEAR)) {
        int sel = (int)SendMessageW(wear, CB_GETCURSEL, 0, 0);
        if (sel > 0 && sel <= wear::Count) o.wear = sel - 1;
    }
    o.wrongResist = getFloat(IDC_RESIST);
    o.crackFolds = loot::clampCrackFolds(getInt(IDC_CRACK_FOLDS));
    auto cl8 = [](int v) -> uint8_t {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
    };
    o.crackR = cl8(getInt(IDC_CRACK_R));
    o.crackG = cl8(getInt(IDC_CRACK_G));
    o.crackB = cl8(getInt(IDC_CRACK_B));
    o.tags = 0;
    for (int i = 0; i < g_tagN; i++)
        if (checked(IDC_TAG0 + i)) o.tags |= g_tags[i].bit;
    o.harvest = checked(IDC_HARVEST);
    o.harvestNeed = 0;
    for (int i = 0; i < g_tagN; i++)
        if (checked(IDC_HNEED0 + i)) o.harvestNeed |= g_tags[i].bit;
    o.nDrops = 0;
    auto push = [&](int itemId, int countId) {
        std::string item = comboId(itemId);
        if (item.empty()) return;
        int c = getInt(countId);
        if (c < 1) c = 1;
        if (c > 255) c = 255;
        if (o.nDrops >= 4) return;
        o.drops[o.nDrops].itemId = item;
        o.drops[o.nDrops].count = (uint8_t)c;
        o.nDrops++;
    };
    push(IDC_DROP1_ITEM, IDC_DROP1_COUNT);
    push(IDC_DROP2_ITEM, IDC_DROP2_COUNT);
    if (o.nDrops > 0) o.harvest = true;
}

void flushEntity(data::EntityDef& e) {
    e.display = getText(IDC_DISPLAY);
    for (int i = 0; i < (int)(sizeof(kEntFields) / sizeof(kEntFields[0])); i++)
        e.rates.*(kEntFields[i].member) = getFloat(IDC_E0 + i);
}

void flushCurrent() {
    std::string id = selectedId();
    if (id.empty()) return;
    if (g_catIdx == 3) {
        if (data::EntityDef* e = data::findEntity(id)) flushEntity(*e);
    } else {
        if (data::ObjectDef* o = data::findObject(id)) flushObject(*o);
    }
}

void loadCurrent() {
    std::string id = selectedId();
    showObj(g_catIdx != 3);
    showEnt(g_catIdx == 3);
    if (id.empty()) {
        setText(IDC_ID, "");
        setText(IDC_DISPLAY, "");
        return;
    }
    if (g_catIdx == 3) {
        if (data::EntityDef* e = data::findEntity(id)) loadEntity(*e);
    } else {
        if (data::ObjectDef* o = data::findObject(id)) loadObject(*o);
    }
}

void refillList() {
    flushCurrent();
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    auto ids = listIds();
    for (const std::string& id : ids) {
        std::string label = id;
        if (g_catIdx == 3) {
            if (data::EntityDef* e = data::findEntity(id)) {
                if (!e->display.empty()) label += "  —  " + e->display;
            }
        } else if (data::ObjectDef* o = data::findObject(id)) {
            if (!o->display.empty()) label += "  —  " + o->display;
        }
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)toWide(label).c_str());
    }
    if (!ids.empty()) SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    loadCurrent();
}

void doSave() {
    flushCurrent();
    if (data::saveAll()) {
        g_dirty = false;
        updateTitle();
        setStatus(L"Wrote assets/data/ and applied runtime tables");
    } else {
        setStatus(L"Save failed: check that assets/data is writable");
    }
}

void doReload() {
    data::reload();
    g_dirty = false;
    updateTitle();
    refillList();
    setStatus(L"Reloaded data packs from disk");
}

void layoutControls() {
    g_tags = data::tagList(&g_tagN);
    mk(L"BUTTON", L"Save All", WS_TABSTOP | BS_PUSHBUTTON, 16, 12, 100, 28, IDC_SAVE);
    mk(L"BUTTON", L"Reload", WS_TABSTOP | BS_PUSHBUTTON, 124, 12, 100, 28, IDC_RELOAD);
    g_status = mk(L"STATIC", L"Save All writes assets/data/<pack>/", 0, 240, 16, 700, 22, IDC_STATUS);

    mk(L"STATIC", L"Category", 0, 16, 52, 160, 18, 301);
    g_cat = mk(L"LISTBOX", L"",
               WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | WS_BORDER |
               LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
               16, 72, 160, 200, IDC_CAT);
    SendMessageW(g_cat, LB_ADDSTRING, 0, (LPARAM)L"Blocks");
    SendMessageW(g_cat, LB_ADDSTRING, 0, (LPARAM)L"Tools");
    SendMessageW(g_cat, LB_ADDSTRING, 0, (LPARAM)L"Items");
    SendMessageW(g_cat, LB_ADDSTRING, 0, (LPARAM)L"Entities");
    SendMessageW(g_cat, LB_SETCURSEL, 0, 0);

    mk(L"STATIC", L"Entries", 0, 192, 52, 220, 18, 302);
    g_list = mk(L"LISTBOX", L"",
                WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | WS_BORDER |
                LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                192, 72, 240, 560, IDC_LIST);

    const int lx = 456, lw = 206, ex = 670, ew = 240, row = 28;
    int y = 72;
    auto lab = [&](const wchar_t* t, int id) {
        mk(L"STATIC", t, 0, lx, y + 4, lw, 20, id);
    };
    lab(L"Id", 303); mk(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, ex, y, ew, 24, IDC_ID); y += row;
    lab(L"Display name", 304); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_DISPLAY); y += row;
    lab(L"Kind", 305);
    HWND kind = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ex, y, ew, 120, IDC_KIND);
    SendMessageW(kind, CB_ADDSTRING, 0, (LPARAM)L"block");
    SendMessageW(kind, CB_ADDSTRING, 0, (LPARAM)L"tool");
    SendMessageW(kind, CB_ADDSTRING, 0, (LPARAM)L"item");
    y += row;
    lab(L"Max stack", 306); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_STACK); y += row;
    lab(L"Hardness", 307); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_HARD); y += row;
    lab(L"Durability", 308); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_DUR); y += row;
    lab(L"Efficiency", 309); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_EFF); y += row;
    lab(L"Windup (sec)", 315); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_CHARGE); y += row;
    lab(L"Recovery (sec)", 316); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_COOLDOWN); y += row;
    lab(L"Weight", 310); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_WEIGHT); y += row;
    lab(L"Wear slot", 320);
    HWND wearBox = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ex, y, ew, 140, IDC_WEAR);
    SendMessageW(wearBox, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    SendMessageW(wearBox, CB_ADDSTRING, 0, (LPARAM)L"hat");
    SendMessageW(wearBox, CB_ADDSTRING, 0, (LPARAM)L"upper");
    SendMessageW(wearBox, CB_ADDSTRING, 0, (LPARAM)L"lower");
    SendMessageW(wearBox, CB_ADDSTRING, 0, (LPARAM)L"shoes");
    SendMessageW(wearBox, CB_SETCURSEL, 0, 0);
    y += row;
    lab(L"Wrong-tool resist", 317); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_RESIST); y += row;
    lab(L"Crack folds", 318); mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, ew, 24, IDC_CRACK_FOLDS); y += row;
    lab(L"Crack color RGB", 319);
    mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, y, 72, 24, IDC_CRACK_R);
    mk(L"EDIT", L"", ES_AUTOHSCROLL, ex + 84, y, 72, 24, IDC_CRACK_G);
    mk(L"EDIT", L"", ES_AUTOHSCROLL, ex + 168, y, 72, 24, IDC_CRACK_B);
    y += row;

    lab(L"Tool tags", 311);
    int tx = ex;
    for (int i = 0; i < g_tagN; i++) {
        mk(L"BUTTON", g_tags[i].label, BS_AUTOCHECKBOX | WS_TABSTOP, tx, y, 84, 22, IDC_TAG0 + i);
        tx += 88;
        if (i == 2) { y += row; tx = ex; }
    }
    y += row;
    mk(L"BUTTON", L"Harvest break (correct tool drops items)", BS_AUTOCHECKBOX | WS_TABSTOP,
       lx, y, 400, 22, IDC_HARVEST);
    y += row;
    lab(L"Harvest tags", 312);
    tx = ex;
    for (int i = 0; i < g_tagN; i++) {
        mk(L"BUTTON", g_tags[i].label, BS_AUTOCHECKBOX | WS_TABSTOP, tx, y, 84, 22, IDC_HNEED0 + i);
        tx += 88;
        if (i == 2) { y += row; tx = ex; }
    }
    y += row;
    lab(L"Drop 1", 313);
    mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, ex, y, 150, 160, IDC_DROP1_ITEM);
    mk(L"EDIT", L"1", ES_AUTOHSCROLL, ex + 156, y, 64, 24, IDC_DROP1_COUNT);
    y += row;
    lab(L"Drop 2", 314);
    mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, ex, y, 150, 160, IDC_DROP2_ITEM);
    mk(L"EDIT", L"1", ES_AUTOHSCROLL, ex + 156, y, 64, 24, IDC_DROP2_COUNT);
    y += row + 6;
    g_preview = mk(L"STATIC", L"", 0, lx, y, 460, 40, IDC_PREVIEW);

    // Shared Id / Display name occupy the first two rows; rates start below them.
    int ey = 72 + row * 2;
    for (int i = 0; i < (int)(sizeof(kEntFields) / sizeof(kEntFields[0])); i++) {
        mk(L"STATIC", kEntFields[i].label, 0, lx, ey + 4, lw, 20, 400 + i);
        mk(L"EDIT", L"", ES_AUTOHSCROLL, ex, ey, ew, 24, IDC_E0 + i);
        ey += row;
    }
    showEnt(false);
}

void onCommand(WPARAM w, LPARAM) {
    int id = LOWORD(w);
    int code = HIWORD(w);
    if (id == IDC_SAVE && code == BN_CLICKED) { doSave(); return; }
    if (id == IDC_RELOAD && code == BN_CLICKED) { doReload(); return; }
    if (id == IDC_CAT && code == LBN_SELCHANGE) {
        flushCurrent();
        g_catIdx = (int)SendMessageW(g_cat, LB_GETCURSEL, 0, 0);
        if (g_catIdx < 0) g_catIdx = 0;
        refillList();
        return;
    }
    if (id == IDC_LIST && code == LBN_SELCHANGE) {
        // selection already changed; we flushed on previous via... need flush before change.
        // ListBox changes sel first. Track previous? Simpler: flush all matching? 
        // We flush the object that is still in the fields (id box), not the new selection.
        std::string editing = getText(IDC_ID);
        if (!editing.empty()) {
            if (g_catIdx == 3) {
                if (data::EntityDef* e = data::findEntity(editing)) flushEntity(*e);
            } else if (data::ObjectDef* o = data::findObject(editing)) {
                flushObject(*o);
            }
        }
        loadCurrent();
        return;
    }
    if (g_loading) return;
    if (code == EN_CHANGE || code == BN_CLICKED || code == CBN_SELCHANGE || code == CBN_EDITCHANGE) {
        if (id == IDC_SAVE || id == IDC_RELOAD || id == IDC_CAT || id == IDC_LIST) return;
        g_dirty = true;
        updateTitle();
        updatePreview();
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        onCommand(w, l);
        return 0;
    }
    if (m == WM_CLOSE) {
        if (g_dirty) {
            int r = MessageBoxW(h, L"Unsaved changes. Save before closing?", L"Data Pack",
                                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) doSave();
        }
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_MEASUREITEM) {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlType == ODT_LISTBOX) mi->itemHeight = 22;
        return TRUE;
    }
    if (m == WM_DRAWITEM) {
        auto* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType != ODT_LISTBOX || di->itemID == (UINT)-1) return TRUE;
        wchar_t buf[512]{};
        SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)buf);
        bool sel = (di->itemState & ODS_SELECTED) != 0;
        FillRect(di->hDC, &di->rcItem,
                 (HBRUSH)(INT_PTR)((sel ? COLOR_HIGHLIGHT : COLOR_WINDOW) + 1));
        if (g_font) SelectObject(di->hDC, g_font);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        RECT tr = di->rcItem;
        InflateRect(&tr, -6, 0);
        DrawTextW(di->hDC, buf, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
        return TRUE;
    }
    if (m == WM_TIMER && w == kHoverTimer) {
        pollHover();
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, kHoverTimer);
        hideHover();
        if (g_hover) { DestroyWindow(g_hover); g_hover = nullptr; }
        g_wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

void runModal(HWND owner) {
    data::init();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VLDataEditor";
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    g_dirty = false;
    g_catIdx = 0;
    g_wnd = CreateWindowExW(
        0, L"VLDataEditor", L"VOXEL LEGEND — Data Pack",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
        owner, nullptr, inst, nullptr);
    if (!g_wnd) {
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        return;
    }
    layoutControls();
    refillList();
    updateTitle();
    SetTimer(g_wnd, kHoverTimer, 50, nullptr);

    if (owner) EnableWindow(owner, FALSE);
    while (g_wnd && IsWindow(g_wnd)) {
        MSG msg;
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (g_hover) { DestroyWindow(g_hover); g_hover = nullptr; }
    if (g_hoverBrush) { DeleteObject(g_hoverBrush); g_hoverBrush = nullptr; }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    g_cat = g_list = g_status = g_preview = nullptr;
}

} // namespace dataed
