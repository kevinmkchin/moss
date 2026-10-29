// term: web pages in tabs (spike). WebView2 (the Edge engine Windows ships) draws each page into a DirectComposition
// visual under the window's swap chain visual; the app leaves the page's rect see-through, so moss's popups go over
// it. The window passes the mouse on (SendMouseInput) while the app says the page has it; a page with the keyboard
// hands moss's own keys back (AcceleratorKeyPressed). WebView2Loader.dll (vendor/webview2, BSD) loads from next to
// moss.exe; without it, or without the runtime, there are no pages.

#include "../vendor/webview2/WebView2.h"

// A COM callback from a lambda: every WebView2 handler is one Invoke (its arguments taken from the interface).
// Starts at no references: WebView2 holds it as long as it needs it.
template<class I, class F, class Sig> struct web_fn;
template<class I, class F, class C, class... A> struct web_fn<I, F, HRESULT (STDMETHODCALLTYPE C::*)(A...)> : I
{
    LONG Refs = 0;
    F Fn;
    web_fn(F In) : Fn(In) {}
    virtual ~web_fn() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Out) override
    {
        if(Id == __uuidof(I) || Id == __uuidof(IUnknown)) { *Out = (I*)this; AddRef(); return S_OK; }
        *Out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&Refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG R = InterlockedDecrement(&Refs); if(!R) { delete this; } return (ULONG)R; }
    HRESULT STDMETHODCALLTYPE Invoke(A... X) override { return Fn(X...); }
};
template<class I, class F> static I* WebFn(F Fn) { return new web_fn<I, F, decltype(&I::Invoke)>(Fn); }

static std::string WebTake(LPWSTR S)
{
    std::string Out = S ? WideToUtf8(S) : std::string();
    if(S) { CoTaskMemFree(S); }
    return Out;
}

struct web_page
{
    u64 Id;
    ICoreWebView2CompositionController* Comp;
    ICoreWebView2Controller* Controller;
    ICoreWebView2* View;
    IDCompositionVisual* Visual;
    RECT Bounds;
    bool32 Shown, Mouse, Closed, Failed, Focused;
    std::wstring Pending;          // navigate there once the page is made
    std::string Url, Title;
    bool32 CanBack, CanForward, Loading, CloseRequested;
    bool32 Visible;                // WebView2 shows it (hidden only once a frame covering it is on screen)
    bool32 CloseLater;             // no longer listed: closed once a frame covering it is on screen
    HCURSOR Cursor;
    u32 Popup;                     // made for this popup (web_popup): its opener gets this page as the new window
    u32 Notices;                   // notifications so far, the last one's title and body
    std::string NoticeTitle, NoticeBody;
};

// A page's request for a new window, held (deferred) until the app makes a web tab for it.
struct web_popup
{
    u32 Id;
    u64 Opener;
    std::string Url;
    ICoreWebView2NewWindowRequestedEventArgs* Args;
    ICoreWebView2Deferral* Deferral;
};

typedef HRESULT (STDAPICALLTYPE *web_create_fn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

struct Web
{
    HWND Window;
    Gpu* G;
    UINT Wake;                      // posted to the window when a page's state changed
    web_create_fn Create;           // WebView2Loader.dll's
    std::wstring Folder;            // the pages' data: logins, cookies
    bool32 EnvStarted;              // with the first page (WebView2's browser processes come up then)
    ICoreWebView2Environment3* Env;
    bool32 EnvFailed;
    std::vector<web_page*> Pages;   // closed pages stay allocated: a late callback may still look at them
    web_page* MouseIn;              // the page the mouse is over, or pressed in
    bool32 Pressed;
    std::vector<web_popup> Popups;  // waiting for their tabs
    u32 NextPopup;
};

static void WebWake(Web* W) { PostMessageW(W->Window, W->Wake, 0, 0); }

// A shown page at its place, at once. Hiding waits for Web_Presented: the frame on screen until then has a
// see-through hole where the page is.
static void WebPlace(Web* W, web_page* P)
{
    if(!P->Controller || !P->Shown) { return; }
    P->Controller->put_Bounds(P->Bounds);
    P->Visual->SetOffsetX((float)P->Bounds.left);
    P->Visual->SetOffsetY((float)P->Bounds.top);
    P->Controller->put_IsVisible(TRUE);
    P->Visible = 1;
}

// --- a page: its composition controller, then its events ---
static void WebStartPage(Web* W, web_page* P)
{
    HRESULT Hr = W->Env->CreateCoreWebView2CompositionController(W->Window, WebFn<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
        [W, P](HRESULT Hr, ICoreWebView2CompositionController* C) -> HRESULT
        {
            if(FAILED(Hr) || !C) { Logf("web", "page %llu: no controller (0x%08X)", P->Id, (u32)Hr); P->Failed = 1; return S_OK; }
            ICoreWebView2Controller* Controller = 0;
            C->QueryInterface(IID_PPV_ARGS(&Controller));
            if(P->Closed) { Controller->Close(); Controller->Release(); return S_OK; }
            C->AddRef();
            P->Comp = C;
            P->Controller = Controller;
            Controller->get_CoreWebView2(&P->View);
            W->G->Comp->CreateVisual(&P->Visual);
            W->G->Root->AddVisual(P->Visual, FALSE, W->G->CompVisual);   // under moss's frame
            C->put_RootVisualTarget(P->Visual);
            EventRegistrationToken Token = {};

            ICoreWebView2* V = P->View;
            // --- a popup's page: it is the window its opener asked for (before it navigates anywhere) ---
            bool Opened = false;
            for(size_t I = 0; I < W->Popups.size(); ++I)
            {
                web_popup& Q = W->Popups[I];
                if(Q.Id != P->Popup) { continue; }
                Q.Args->put_NewWindow(V);
                Q.Args->put_Handled(TRUE);
                Q.Deferral->Complete();
                Q.Deferral->Release();
                Q.Args->Release();
                W->Popups.erase(W->Popups.begin() + (s64)I);
                Opened = true;
                break;
            }
            V->add_SourceChanged(WebFn<ICoreWebView2SourceChangedEventHandler>([W, P](ICoreWebView2* V, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                LPWSTR S = 0; V->get_Source(&S); P->Url = WebTake(S); WebWake(W); return S_OK; }), &Token);
            V->add_DocumentTitleChanged(WebFn<ICoreWebView2DocumentTitleChangedEventHandler>([W, P](ICoreWebView2* V, IUnknown*) -> HRESULT {
                LPWSTR S = 0; V->get_DocumentTitle(&S); P->Title = WebTake(S); WebWake(W); return S_OK; }), &Token);
            V->add_HistoryChanged(WebFn<ICoreWebView2HistoryChangedEventHandler>([W, P](ICoreWebView2* V, IUnknown*) -> HRESULT {
                BOOL B = 0; V->get_CanGoBack(&B); P->CanBack = B; V->get_CanGoForward(&B); P->CanForward = B; WebWake(W); return S_OK; }), &Token);
            V->add_NavigationStarting(WebFn<ICoreWebView2NavigationStartingEventHandler>([W, P](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                P->Loading = 1; WebWake(W); return S_OK; }), &Token);
            V->add_NavigationCompleted(WebFn<ICoreWebView2NavigationCompletedEventHandler>([W, P](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* A) -> HRESULT {
                BOOL Ok = 0; A->get_IsSuccess(&Ok);
                COREWEBVIEW2_WEB_ERROR_STATUS Status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN; A->get_WebErrorStatus(&Status);
                Logf("web", "page %llu: loaded %s%s", P->Id, P->Url.c_str(), Ok ? "" : " (failed)");
                P->Loading = 0; WebWake(W); return S_OK; }), &Token);
            V->add_ProcessFailed(WebFn<ICoreWebView2ProcessFailedEventHandler>([P](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* A) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND Kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED; A->get_ProcessFailedKind(&Kind);
                Logf("web", "page %llu: a browser process failed (kind %d)", P->Id, (int)Kind); return S_OK; }), &Token);
            V->add_PermissionRequested(WebFn<ICoreWebView2PermissionRequestedEventHandler>([P](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* A) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND Kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION; A->get_PermissionKind(&Kind);
                LPWSTR S = 0; A->get_Uri(&S);
                // notifications are moss's to show (settings `notifications = off`: none): allowed; the rest WebView2 asks
                if(Kind == COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS) { A->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW); }
                Logf("web", "page %llu: %s asks for permission %d", P->Id, WebTake(S).c_str(), (int)Kind); return S_OK; }), &Token);
            ICoreWebView2_24* V24 = 0;
            V->QueryInterface(IID_PPV_ARGS(&V24));
            if(V24)
            {
                // notifications: moss shows them (the app decides: the tray, a dot on the tab), not WebView2
                V24->add_NotificationReceived(WebFn<ICoreWebView2NotificationReceivedEventHandler>([W, P](ICoreWebView2*, ICoreWebView2NotificationReceivedEventArgs* A) -> HRESULT {
                    ICoreWebView2Notification* N = 0; A->get_Notification(&N);
                    LPWSTR T = 0, B = 0, O = 0;
                    if(N) { N->get_Title(&T); N->get_Body(&B); N->ReportShown(); N->Release(); }
                    A->get_SenderOrigin(&O);
                    A->put_Handled(TRUE);
                    P->NoticeTitle = WebTake(T);
                    P->NoticeBody = WebTake(B);
                    P->Notices++;
                    Logf("web", "page %llu: a notification from %s", P->Id, WebTake(O).c_str());
                    WebWake(W);
                    return S_OK; }), &Token);
                V24->Release();
            }
            V->add_WebMessageReceived(WebFn<ICoreWebView2WebMessageReceivedEventHandler>([P](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* A) -> HRESULT {
                LPWSTR S = 0; A->TryGetWebMessageAsString(&S);
                Logf("web", "page %llu says: %s", P->Id, WebTake(S).c_str()); return S_OK; }), &Token);
            // --- new windows (window.open, target=_blank): held until the app makes a web tab for each ---
            V->add_NewWindowRequested(WebFn<ICoreWebView2NewWindowRequestedEventHandler>([W, P](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* A) -> HRESULT {
                web_popup Q = {};
                Q.Id = ++W->NextPopup;
                Q.Opener = P->Id;
                LPWSTR S = 0; A->get_Uri(&S); Q.Url = WebTake(S);
                A->AddRef();
                Q.Args = A;
                A->GetDeferral(&Q.Deferral);
                W->Popups.push_back(Q);
                Logf("web", "page %llu: new window %u (%s)", P->Id, Q.Id, Q.Url.c_str());
                WebWake(W);
                return S_OK; }), &Token);
            V->add_WindowCloseRequested(WebFn<ICoreWebView2WindowCloseRequestedEventHandler>([W, P](ICoreWebView2*, IUnknown*) -> HRESULT {
                P->CloseRequested = 1; WebWake(W); return S_OK; }), &Token);

            // --- moss's keys while the page has the keyboard: Ctrl+Shift+anything, Alt+arrows, Ctrl+Tab, Ctrl+, F11,
            //     Ctrl+L (the address) go to the window instead ---
            Controller->add_AcceleratorKeyPressed(WebFn<ICoreWebView2AcceleratorKeyPressedEventHandler>([W](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* A) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND Kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN; A->get_KeyEventKind(&Kind);
                UINT Vk = 0; A->get_VirtualKey(&Vk);
                INT LParam = 0; A->get_KeyEventLParam(&LParam);
                bool Ctrl = GetKeyState(VK_CONTROL) < 0, Shift = GetKeyState(VK_SHIFT) < 0, Alt = GetKeyState(VK_MENU) < 0;
                bool Modifier = Vk == VK_CONTROL || Vk == VK_SHIFT || Vk == VK_MENU || Vk == VK_LWIN || Vk == VK_RWIN;
                bool Arrow = Vk == VK_LEFT || Vk == VK_RIGHT || Vk == VK_UP || Vk == VK_DOWN;
                bool Mine = !Modifier && ((Ctrl && Shift) || (Alt && !Ctrl && Arrow) || (Ctrl && Vk == VK_TAB) ||
                                          (Ctrl && !Alt && (Vk == VK_OEM_COMMA || Vk == 'L')) || Vk == VK_F11);
                if(!Mine) { return S_OK; }
                A->put_Handled(TRUE);
                UINT Msg = Kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ? WM_KEYDOWN : Kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP ? WM_KEYUP :
                           Kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN ? WM_SYSKEYDOWN : WM_SYSKEYUP;
                PostMessageW(W->Window, Msg, Vk, (LPARAM)LParam);
                return S_OK; }), &Token);
            Controller->add_GotFocus(WebFn<ICoreWebView2FocusChangedEventHandler>([W, P](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                P->Focused = 1; WebWake(W); return S_OK; }), &Token);
            Controller->add_LostFocus(WebFn<ICoreWebView2FocusChangedEventHandler>([W, P](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                P->Focused = 0; WebWake(W); return S_OK; }), &Token);
            C->add_CursorChanged(WebFn<ICoreWebView2CursorChangedEventHandler>([W, P](ICoreWebView2CompositionController* C, IUnknown*) -> HRESULT {
                C->get_Cursor(&P->Cursor);
                if(W->MouseIn == P && P->Cursor) { SetCursor(P->Cursor); }
                return S_OK; }), &Token);

            Controller->put_IsVisible(FALSE);
            WebPlace(W, P);
            W->G->Comp->Commit();
            if(!P->Pending.empty() && !Opened) { V->Navigate(P->Pending.c_str()); }
            P->Pending.clear();
            Logf("web", "page %llu made%s", P->Id, Opened ? " (a new window)" : "");
            return S_OK;
        }));
    if(FAILED(Hr)) { Logf("web", "CreateCoreWebView2CompositionController failed (0x%08X)", (u32)Hr); P->Failed = 1; }
}

// Loads WebView2's loader; the engine itself starts with the first page.
extern "C" Web* Web_Create(void* Hwnd, Gpu* G, u32 WakeMessage, const wchar_t* DataFolder)
{
    if(!G || !G->Root) { return 0; }
    HMODULE Loader = LoadLibraryW((ExeDir() + L"WebView2Loader.dll").c_str());
    web_create_fn Create = Loader ? (web_create_fn)GetProcAddress(Loader, "CreateCoreWebView2EnvironmentWithOptions") : 0;
    if(!Create) { Logf("web", "no WebView2Loader.dll next to moss.exe"); return 0; }
    Web* W = new Web();
    W->Window = (HWND)Hwnd;
    W->G = G;
    W->Wake = WakeMessage;
    W->Create = Create;
    W->Folder = DataFolder;
    return W;
}

// WebView2's environment (its browser processes come up in the background); pages wait for it.
static void WebStartEnv(Web* W)
{
    W->EnvStarted = 1;
    u64 T0 = Time_NowMicros();
    HRESULT Hr = W->Create(0, W->Folder.c_str(), 0, WebFn<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [W, T0](HRESULT Hr, ICoreWebView2Environment* Env) -> HRESULT
        {
            if(FAILED(Hr) || !Env) { Logf("web", "no WebView2 environment (0x%08X)", (u32)Hr); W->EnvFailed = 1; return S_OK; }
            Env->QueryInterface(IID_PPV_ARGS(&W->Env));
            if(!W->Env) { Logf("web", "WebView2 is too old (no composition hosting)"); W->EnvFailed = 1; return S_OK; }
            LPWSTR Version = 0;
            Env->get_BrowserVersionString(&Version);
            Logf("web", "WebView2 %s ready in %llu ms", WebTake(Version).c_str(), (Time_NowMicros() - T0) / 1000);
            for(web_page* P : W->Pages) { if(!P->Closed) { WebStartPage(W, P); } }
            return S_OK;
        }));
    if(FAILED(Hr)) { Logf("web", "CreateCoreWebView2EnvironmentWithOptions failed (0x%08X)", (u32)Hr); W->EnvFailed = 1; }
}

// The pages as the app has them this frame: new ones made, placed, shown or hidden, navigated; ones it no longer
// lists closed. The keyboard goes to the page `Focus` (0: the window keeps it).
extern "C" void Web_Update(Web* W, const AppWeb* Webs, s32 Count, u64 Focus)
{
    if(!W) { return; }
    if(Count && !W->EnvStarted) { WebStartEnv(W); }
    bool Changed = false;
    for(s32 I = 0; I < Count; ++I)
    {
        const AppWeb* A = &Webs[I];
        web_page* P = 0;
        for(web_page* Q : W->Pages) { if(Q->Id == A->id && !Q->Closed) { P = Q; } }
        if(!P)
        {
            P = new web_page();
            P->Id = A->id;
            P->Popup = A->popup;
            W->Pages.push_back(P);
            if(W->Env) { WebStartPage(W, P); }
        }
        RECT B = {(LONG)A->rect.x0, (LONG)A->rect.y0, (LONG)A->rect.x1, (LONG)A->rect.y1};
        bool Moved = !EqualRect(&B, &P->Bounds) || P->Shown != A->shown;
        P->Bounds = B;
        P->Shown = A->shown;
        P->Mouse = A->mouse;
        if(Moved && P->Controller) { WebPlace(W, P); Changed = true; }
        if(A->navigate.count)
        {
            std::wstring Url = Utf8ToWide(std::string((const char*)A->navigate.data, (size_t)A->navigate.count));
            Logf("web", "page %llu: go to %.*s", P->Id, STR_ARG(A->navigate));
            if(P->View) { P->View->Navigate(Url.c_str()); }
            else { P->Pending = Url; }
        }
        if(P->View && A->step < 0) { P->View->GoBack(); }
        if(P->View && A->step > 0) { P->View->GoForward(); }
        if(P->View && A->reload) { P->View->Reload(); }
    }
    for(web_page* P : W->Pages)
    {
        bool Listed = false;
        for(s32 I = 0; I < Count; ++I) { Listed = Listed || Webs[I].id == P->Id; }
        if(Listed || P->Closed) { continue; }
        P->Closed = 1;
        P->CloseLater = 1;
        if(W->MouseIn == P) { W->MouseIn = 0; }
        Logf("web", "page %llu closed", P->Id);
    }
    // a popup whose opener is gone gets no window
    for(size_t I = 0; I < W->Popups.size();)
    {
        web_popup& Q = W->Popups[I];
        bool Alive = false;
        for(web_page* P : W->Pages) { Alive = Alive || (P->Id == Q.Opener && !P->Closed); }
        if(Alive) { ++I; continue; }
        Q.Deferral->Complete();
        Q.Deferral->Release();
        Q.Args->Release();
        W->Popups.erase(W->Popups.begin() + (s64)I);
    }
    if(Changed) { W->G->Comp->Commit(); }

    // --- the keyboard: to the page the app says, only while the window is in front, and not while a press of moss's
    //     own is held (a tab picked up: moving the focus would take the mouse from the drag) ---
    bool MossPress = !W->Pressed && (GetKeyState(VK_LBUTTON) < 0 || GetKeyState(VK_RBUTTON) < 0 || GetKeyState(VK_MBUTTON) < 0);
    if(GetForegroundWindow() == W->Window && !MossPress)
    {
        web_page* Want = 0;
        web_page* Has = 0;
        for(web_page* P : W->Pages)
        {
            if(P->Closed || !P->Controller) { continue; }
            if(P->Id == Focus && P->Shown) { Want = P; }
            if(P->Focused) { Has = P; }
        }
        if(Want && Want != Has) { Want->Controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
        if(!Want && Has) { SetFocus(W->Window); }
    }
}

// A frame is on screen: pages it covers now (no longer shown, or closed) go.
extern "C" void Web_Presented(Web* W)
{
    bool Changed = false;
    for(size_t I = 0; W && I < W->Pages.size(); ++I)
    {
        web_page* P = W->Pages[I];
        if(P->CloseLater)
        {
            P->CloseLater = 0;
            if(P->Controller) { P->Controller->Close(); }
            if(P->Visual) { W->G->Root->RemoveVisual(P->Visual); }
            Changed = true;
        }
        else if(P->Visible && !P->Shown && P->Controller)
        {
            P->Controller->put_IsVisible(FALSE);
            P->Visible = 0;
            Changed = true;
        }
    }
    if(Changed) { W->G->Comp->Commit(); }
}

// The pages' state for the app.
extern "C" s32 Web_Fill(Web* W, Arena* A, AppWebState* Out, s32 Max)
{
    s32 N = 0;
    for(size_t I = 0; W && I < W->Pages.size() && N < Max; ++I)
    {
        web_page* P = W->Pages[I];
        if(P->Closed) { continue; }
        AppWebState* S = &Out[N++];
        S->id = P->Id;
        S->url = Str_PushCopy(A, String{(u8*)P->Url.data(), (s64)P->Url.size()});
        S->title = Str_PushCopy(A, String{(u8*)P->Title.data(), (s64)P->Title.size()});
        S->can_back = P->CanBack;
        S->can_forward = P->CanForward;
        S->loading = P->Loading;
        S->failed = P->Failed || W->EnvFailed;
        S->close_requested = P->CloseRequested;
        S->notices = P->Notices;
        S->notice_title = Str_PushCopy(A, String{(u8*)P->NoticeTitle.data(), (s64)P->NoticeTitle.size()});
        S->notice_body = Str_PushCopy(A, String{(u8*)P->NoticeBody.data(), (s64)P->NoticeBody.size()});
    }
    return N;
}

// New windows pages asked for, waiting for their tabs.
extern "C" s32 Web_Popups(Web* W, Arena* A, AppWebPopup* Out, s32 Max)
{
    s32 N = 0;
    for(size_t I = 0; W && I < W->Popups.size() && N < Max; ++I)
    {
        const web_popup& Q = W->Popups[I];
        Out[N].id = Q.Id;
        Out[N].opener = Q.Opener;
        Out[N].url = Str_PushCopy(A, String{(u8*)Q.Url.data(), (s64)Q.Url.size()});
        N++;
    }
    return N;
}

extern "C" b32 Web_HasFocus(Web* W)
{
    for(size_t I = 0; W && I < W->Pages.size(); ++I) { if(W->Pages[I]->Focused && !W->Pages[I]->Closed) { return 1; } }
    return 0;
}

extern "C" void* Web_Cursor(Web* W) { return W && W->MouseIn ? (void*)W->MouseIn->Cursor : 0; }

// A mouse message of the window: to the page under it (one the app says the mouse is over), or the one pressed in
// until the buttons are up. 1: a page took it.
extern "C" b32 Web_Mouse(Web* W, u32 Msg, u64 WParam, s64 LParam)
{
    if(!W) { return 0; }
    POINT Pt = {GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam)};
    if(Msg == WM_MOUSEWHEEL || Msg == WM_MOUSEHWHEEL) { ScreenToClient(W->Window, &Pt); }
    web_page* P = W->Pressed ? W->MouseIn : 0;
    // a press that began on moss's own (a tab dragged over a page, a splitter) stays moss's until it's let go: a page
    // seeing a held button would take the mouse
    bool MossPress = !W->Pressed && (GET_KEYSTATE_WPARAM(WParam) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) &&
                     (Msg == WM_MOUSEMOVE || Msg == WM_LBUTTONUP || Msg == WM_RBUTTONUP || Msg == WM_MBUTTONUP);
    if(!P && Msg != WM_MOUSELEAVE && !MossPress)
    {
        for(web_page* Q : W->Pages) { if(!Q->Closed && Q->Comp && Q->Shown && Q->Mouse && PtInRect(&Q->Bounds, Pt)) { P = Q; } }
    }
    if(W->MouseIn && W->MouseIn != P && W->MouseIn->Comp)
    {
        W->MouseIn->Comp->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, POINT{});
    }
    W->MouseIn = P;
    if(!P) { W->Pressed = 0; return 0; }
    COREWEBVIEW2_MOUSE_EVENT_KIND Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
    UINT32 Data = 0;
    switch(Msg)
    {
    case WM_MOUSEMOVE: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; break;
    case WM_LBUTTONDOWN: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; break;
    case WM_LBUTTONUP: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; break;
    case WM_LBUTTONDBLCLK: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK; break;
    case WM_RBUTTONDOWN: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; break;
    case WM_RBUTTONUP: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; break;
    case WM_RBUTTONDBLCLK: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK; break;
    case WM_MBUTTONDOWN: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN; break;
    case WM_MBUTTONUP: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP; break;
    case WM_MBUTTONDBLCLK: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK; break;
    case WM_XBUTTONDOWN: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN; Data = GET_XBUTTON_WPARAM(WParam); break;
    case WM_XBUTTONUP: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP; Data = GET_XBUTTON_WPARAM(WParam); break;
    case WM_XBUTTONDBLCLK: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK; Data = GET_XBUTTON_WPARAM(WParam); break;
    case WM_MOUSEWHEEL: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL; Data = (UINT32)(s32)GET_WHEEL_DELTA_WPARAM(WParam); break;
    case WM_MOUSEHWHEEL: Kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL; Data = (UINT32)(s32)GET_WHEEL_DELTA_WPARAM(WParam); break;
    default: return 0;
    }
    u32 Buttons = (u32)GET_KEYSTATE_WPARAM(WParam) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2);
    bool Down = Msg == WM_LBUTTONDOWN || Msg == WM_RBUTTONDOWN || Msg == WM_MBUTTONDOWN || Msg == WM_XBUTTONDOWN ||
                Msg == WM_LBUTTONDBLCLK || Msg == WM_RBUTTONDBLCLK || Msg == WM_MBUTTONDBLCLK;
    // a press makes the page take the keyboard, which brings the window to the front: only while it's there already (a
    // click on a window behind activates it first; a test window's posted clicks never steal the foreground)
    if(Down && GetForegroundWindow() != W->Window) { return 0; }
    if(Down) { W->Pressed = 1; }
    else if(!Buttons) { W->Pressed = 0; }
    POINT At = {Pt.x - P->Bounds.left, Pt.y - P->Bounds.top};
    P->Comp->SendMouseInput(Kind, (COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)GET_KEYSTATE_WPARAM(WParam), Data, At);
    return 1;
}

extern "C" void Web_Destroy(Web* W)
{
    if(!W) { return; }
    for(web_page* P : W->Pages)
    {
        if(P->Controller && (!P->Closed || P->CloseLater)) { P->Controller->Close(); }
        if(P->View) { P->View->Release(); }
        if(P->Controller) { P->Controller->Release(); }
        if(P->Comp) { P->Comp->Release(); }
        if(P->Visual) { P->Visual->Release(); }
        delete P;
    }
    if(W->Env) { W->Env->Release(); }
    delete W;
}

// --- `moss --web-probe URL OUT.png`: a page in a window no one sees (off screen, never activated), then what the
//     engine can do there (Widevine, notifications, the user agent) to the log, and a picture of the page ---
static const wchar_t WebProbeScript[] = LR"JS(
(async () => {
  const out = [];
  out.push('agent ' + navigator.userAgent);
  out.push('notifications ' + (typeof Notification) + ' ' + (window.Notification ? Notification.permission : ''));
  out.push('serviceWorker ' + ('serviceWorker' in navigator) + ', push ' + ('PushManager' in window));
  for (const ks of ['com.widevine.alpha', 'com.microsoft.playready.recommendation', 'org.w3.clearkey']) {
    try {
      await navigator.requestMediaKeySystemAccess(ks, [{ initDataTypes: ['cenc'],
        audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }],
        videoCapabilities: [{ contentType: 'video/mp4; codecs="avc1.42E01E"' }] }]);
      out.push('drm ' + ks + ' yes');
    } catch (e) { out.push('drm ' + ks + ' no: ' + e.name); }
  }
  out.push('mse ' + (window.MediaSource ? MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E"') : false) +
           ', vp9 ' + (window.MediaSource ? MediaSource.isTypeSupported('video/webm; codecs="vp9"') : false));
  for (const line of out) chrome.webview.postMessage(line);
  chrome.webview.postMessage('probe done');
})();
)JS";

extern "C" int Web_ProbeMain(const wchar_t* Url, const wchar_t* Png)
{
    WNDCLASSW Wc = {};
    Wc.lpfnWndProc = DefWindowProcW;
    Wc.hInstance = GetModuleHandleW(0);
    Wc.lpszClassName = L"moss-web-probe";
    RegisterClassW(&Wc);
    HWND Window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"moss-web-probe", L"moss web probe", WS_POPUP,
                                  -4000, -4000, 1280, 800, 0, 0, Wc.hInstance, 0);
    ShowWindow(Window, SW_SHOWNOACTIVATE);
    Gpu* G = Gpu_Create(Window, 1280, 800);
    if(!G || !G->Root) { Logf("web", "probe: no GPU"); return 1; }
    wchar_t Temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, Temp);
    std::wstring Folder = std::wstring(Temp) + L"moss-web-probe";
    // an off-screen window counts as covered: pages there would stop drawing
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--disable-features=CalculateNativeWinOcclusion");
    Web* W = Web_Create(Window, G, WM_NULL, Folder.c_str());
    if(!W) { return 1; }
    AppWeb Page = {};
    Page.id = 1;
    Page.rect = Rect{0, 0, 1280, 800};
    Page.shown = 1;
    std::string Utf8 = WideToUtf8(Url);
    Page.navigate = String{(u8*)Utf8.data(), (s64)Utf8.size()};
    Web_Update(W, &Page, 1, 0);
    Page.navigate = String{};
    u64 Start = Time_NowMicros(), LoadedAt = 0;
    int Stage = 0;   // 0 loading, 1 settling, 2 script sent, 3 picture asked, 4 done
    bool Done = false;
    while(!Done && Time_NowMicros() - Start < 60000000)
    {
        MSG Msg = {};
        while(PeekMessageW(&Msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&Msg); DispatchMessageW(&Msg); }
        web_page* P = W->Pages.empty() ? 0 : W->Pages[0];
        if(W->EnvFailed || (P && P->Failed)) { break; }
        if(Stage == 0 && P && P->View && !P->Loading && !P->Url.empty()) { Stage = 1; LoadedAt = Time_NowMicros(); }
        if(Stage == 1 && Time_NowMicros() - LoadedAt > 3000000)
        {
            Stage = 2;
            EventRegistrationToken Token = {};
            P->View->ExecuteScript(WebProbeScript, WebFn<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }));
            P->View->add_WebMessageReceived(WebFn<ICoreWebView2WebMessageReceivedEventHandler>([&Stage, P, Png, &Done](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* A) -> HRESULT {
                LPWSTR S = 0; A->TryGetWebMessageAsString(&S);
                std::string Line = WebTake(S);
                if(Line != "probe done" || Stage != 2) { return S_OK; }
                Stage = 3;
                IStream* Stream = 0;
                SHCreateStreamOnFileEx(Png, STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, 0, &Stream);
                if(!Stream) { Done = true; return S_OK; }
                P->View->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, Stream, WebFn<ICoreWebView2CapturePreviewCompletedHandler>(
                    [Stream, &Done](HRESULT Hr) -> HRESULT { Logf("web", "probe: picture 0x%08X", (u32)Hr); Stream->Release(); Done = true; return S_OK; }));
                return S_OK; }), &Token);
        }
        MsgWaitForMultipleObjects(0, 0, FALSE, 50, QS_ALLINPUT);
    }
    Logf("web", "probe: %s in %llu ms", Done ? "done" : "gave up", (Time_NowMicros() - Start) / 1000);
    Web_Destroy(W);
    DestroyWindow(Window);
    return Done ? 0 : 1;
}
