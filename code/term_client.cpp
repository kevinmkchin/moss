// term: the window's side of the pipe. Applies the server's tab list and cell frames (term_ipc_proto.h),
// hands them to the app as TermTab / TermGrid, and sends the app's requests back.

struct client_grid
{
    u32 TabId;
    s32 Cols;
    s32 Rows;
    std::vector<cell> Cells;
    msg_cells Head;
    u64 Version;
};

struct TermClient
{
    ipc_conn Conn;
    bool32 Connected;
    std::string Status;
    HWND Window;

    // --- the tab list as last sent (strings stay put until the next list) ---
    std::vector<msg_tab> Tabs;
    std::vector<std::string> Strings; // per tab: key, name, cwd, notice, agent, owner
    std::vector<TermTab> TabViews;
    bool32 TabsChanged;
    bool32 TabsReceived; // a list since the last connect

    std::vector<client_grid*> Grids;
    std::vector<TermGrid> GridViews;
    u64 NextVersion;

    std::string Clipboard;  // OSC 52 from a program, not taken yet
    u32 ViewTab;            // `view <file>` in that tab, not taken yet (0: none)
    std::string ViewPath;
    bool32 Bell;
    bool32 Close;           // the server said goodbye for good (BYE_CLOSE)
    std::string Name;       // the window's name (empty = the main window), told the server on attach
    std::string Sessions;   // the last MSG_SESSIONS (SessionViews point into it)
    std::vector<TermSession> SessionViews;
    bool32 WantSessions;    // what the server was told
    u32 FindState[3];       // the last MSG_FIND_STATE: tab, count, index
    std::vector<u16> FindMarks; // and where its matches are
    std::string Link;       // a link's URI the server sent (MSG_LINK), to open
    std::string History;    // the last MSG_HISTORY, kept whole: HistoryViews point into it
    std::vector<TermHistoryEntry> HistoryViews;
    u32 HistoryTab, HistoryVersion;
    u32 SentFocus;
    u64 BytesIn;
    u64 BytesOut;
    u64 MessagesIn;
};

static client_grid* ClientGrid(TermClient* C, u32 TabId)
{
    for(client_grid* G : C->Grids) { if(G->TabId == TabId) { return G; } }
    return 0;
}

// The connection is gone (server quit or crashed): its tabs and grids go with it; the window keeps its
// panes and finds the tabs again by lasting id once a server is back.
static void ClientLost(TermClient* C, const char* Why)
{
    C->Connected = false;
    if(C->Status.empty() || Why[0] != 'l') { C->Status = Why; }
    C->Tabs.clear();
    C->Strings.clear();
    for(client_grid* G : C->Grids) { delete G; }
    C->Grids.clear();
    C->TabsChanged = true;
    C->TabsReceived = false;
    C->SentFocus = 0;
    C->WantSessions = false; // a new server is told again
    IpcClose(C->Conn);
    Logf("client", "%s", C->Status.c_str());
}

static void ClientSend(TermClient* C, u32 Type, const std::string& Payload)
{
    if(!C->Connected) { return; }
    if(!IpcSendString(C->Conn, Type, Payload)) { ClientLost(C, "lost the connection to the moss server"); return; }
    C->BytesOut += Payload.size() + 8;
}

extern "C" b32 TermClient_ShouldClose(TermClient* C) { return C->Close; }

extern "C" TermClient* TermClient_Create(String WindowName)
{
    TermClient* C = new TermClient();
    C->Name.assign((const char*)WindowName.data, (size_t)WindowName.count);
    return C;
}

extern "C" void TermClient_SetOwner(TermClient* C, u32 TabId, String Owner)
{
    std::string P((const char*)&TabId, 4);
    P.append((const char*)Owner.data, (size_t)Owner.count);
    ClientSend(C, MSG_SET_OWNER, P);
}

extern "C" b32 TermClient_Connect(TermClient* C, void* Hwnd, String Cwd, Arena* A, String* Error)
{
    wchar_t Exe[MAX_PATH] = {};
    GetModuleFileNameW(0, Exe, MAX_PATH);
    std::string Err;
    HANDLE Pipe = ConnectToServer(Exe, Err);
    if(!Pipe)
    {
        C->Status = Err;
        *Error = Str_PushCopy(A, String{(u8*)Err.data(), (s64)Err.size()});
        Logf("client", "connect failed: %s", Err.c_str());
        return 0;
    }
    IpcClose(C->Conn);
    IpcInit(C->Conn, Pipe);
    IpcPump(C->Conn);
    C->Window = (HWND)Hwnd;
    C->Connected = true;
    C->Status.clear();
    msg_attach M = {};
    M.Version = TERM_PROTOCOL_VERSION;
    M.Window = (u64)(uintptr_t)Hwnd;
    std::string Payload((const char*)&M, sizeof(M));
    Payload.append((const char*)Cwd.data, (size_t)Cwd.count);
    Payload.push_back(0);
    Payload += C->Name;
    ClientSend(C, MSG_ATTACH, Payload);
    Logf("client", "attached to the server");
    return 1;
}

extern "C" void* TermClient_ReadEvent(TermClient* C)
{
    return C->Connected ? C->Conn.ReadEvent : 0;
}

extern "C" b32 TermClient_Poll(TermClient* C)
{
    if(!C->Connected) { return 0; }
    TIMED_BLOCK(ClientPoll);
    bool32 Changed = false;
    bool32 Alive = IpcPump(C->Conn);
    u32 Type = 0;
    std::string Payload;
    while(IpcNextMessage(C->Conn, Type, Payload))
    {
        C->MessagesIn += 1;
        C->BytesIn += Payload.size() + 8;
        if(Type == MSG_TABS && Payload.size() >= sizeof(msg_tabs))
        {
            // --- the tab list ---
            msg_tabs H = {};
            memcpy(&H, Payload.data(), sizeof(H));
            size_t At = sizeof(H);
            C->Tabs.clear();
            C->Strings.clear();
            for(u32 I = 0; I < H.Count && At + sizeof(msg_tab) <= Payload.size(); ++I)
            {
                msg_tab M = {};
                memcpy(&M, Payload.data() + At, sizeof(M));
                At += sizeof(M);
                u32 Lens[8] = {M.KeyLen, M.NameLen, M.CwdLen, M.NoticeLen, M.AgentLen, M.OwnerLen, M.AccountLen, M.AccountWasLen};
                for(u32 Len : Lens)
                {
                    size_t From = At < Payload.size() ? At : Payload.size();
                    size_t Take = Len < Payload.size() - From ? Len : Payload.size() - From;
                    C->Strings.push_back(Payload.substr(From, Take));
                    At += Len;
                }
                C->Tabs.push_back(M);
            }
            // grids of tabs that are gone
            for(size_t K = 0; K < C->Grids.size();)
            {
                bool32 Kept = false;
                for(const msg_tab& M : C->Tabs) { Kept = Kept || M.Id == C->Grids[K]->TabId; }
                if(Kept) { ++K; continue; }
                delete C->Grids[K];
                C->Grids.erase(C->Grids.begin() + (s64)K);
            }
            C->TabsChanged = true;
            C->TabsReceived = true;
            Changed = true;
        }
        else if(Type == MSG_CELLS && Payload.size() >= sizeof(msg_cells))
        {
            // --- a frame of one tab: its changed rows ---
            Term_LatencyMark("client_cells");
            msg_cells H = {};
            memcpy(&H, Payload.data(), sizeof(H));
            client_grid* G = ClientGrid(C, H.TabId);
            if(!G) { G = new client_grid(); G->TabId = H.TabId; C->Grids.push_back(G); }
            if((H.Flags & CELLS_FULL) || H.Cols != G->Cols || H.Rows != G->Rows)
            {
                G->Cols = Max(H.Cols, 0);
                G->Rows = Max(H.Rows, 0);
                G->Cells.assign((size_t)G->Cols * G->Rows, cell{});
            }
            size_t At = sizeof(H);
            size_t RowBytes = (size_t)G->Cols * sizeof(cell);
            for(u32 I = 0; I < H.RowCount && At + sizeof(s32) + RowBytes <= Payload.size(); ++I)
            {
                s32 Y = 0;
                memcpy(&Y, Payload.data() + At, sizeof(Y));
                At += sizeof(Y);
                if(Y >= 0 && Y < G->Rows) { memcpy(G->Cells.data() + (size_t)Y * G->Cols, Payload.data() + At, RowBytes); }
                At += RowBytes;
            }
            G->Head = H;
            G->Version = ++C->NextVersion;
            Changed = true;
        }
        else if(Type == MSG_OUTPUT)
        {
            // --- what programs pass to the terminal itself: the bell, OSC 52 "set the clipboard" (how
            // Claude Code copies), "ESC ] 52 ; <targets> ; <base64> ESC \" ---
            if(Payload.find('\x07') != std::string::npos) { C->Bell = true; Changed = true; }
            for(size_t At = Payload.find("\x1b]52;"); At != std::string::npos; At = Payload.find("\x1b]52;", At + 1))
            {
                size_t Semi = Payload.find(';', At + 5);
                size_t End = Payload.find("\x1b\\", At);
                if(Semi == std::string::npos || End == std::string::npos || Semi > End) { continue; }
                std::string Text;
                u32 Bits = 0;
                s32 BitCount = 0;
                for(size_t I = Semi + 1; I < End; ++I)
                {
                    char Ch = Payload[I];
                    s32 V = (Ch >= 'A' && Ch <= 'Z') ? Ch - 'A' : (Ch >= 'a' && Ch <= 'z') ? Ch - 'a' + 26 :
                            (Ch >= '0' && Ch <= '9') ? Ch - '0' + 52 : (Ch == '+') ? 62 : (Ch == '/') ? 63 : -1;
                    if(V < 0) { continue; } // '=' padding; a "?" query has no data and sets nothing
                    Bits = (Bits << 6) | (u32)V;
                    BitCount += 6;
                    if(BitCount >= 8) { BitCount -= 8; Text += (char)((Bits >> BitCount) & 0xFF); }
                }
                if(!Text.empty()) { C->Clipboard = Text; Changed = true; }
            }
            // `view <file>` in a tab: "ESC ] 7777 ; view ; <tab id> ; <path> ESC \" (the server adds the tab)
            for(size_t At = Payload.find("\x1b]7777;view;"); At != std::string::npos; At = Payload.find("\x1b]7777;view;", At + 1))
            {
                size_t Semi = Payload.find(';', At + 12);
                size_t End = Payload.find("\x1b\\", At);
                if(Semi == std::string::npos || End == std::string::npos || Semi > End) { continue; }
                C->ViewTab = (u32)strtoul(Payload.substr(At + 12, Semi - At - 12).c_str(), 0, 10);
                C->ViewPath = Payload.substr(Semi + 1, End - Semi - 1);
                Changed = true;
            }
        }
        else if(Type == MSG_LINK && Payload.size() > 4)
        {
            C->Link = Payload.substr(4);
            Changed = true;
        }
        else if(Type == MSG_HISTORY && Payload.size() >= 8)
        {
            C->History = Payload;
            C->HistoryViews.clear();
            u32 Count = 0;
            memcpy(&C->HistoryTab, C->History.data(), 4);
            memcpy(&Count, C->History.data() + 4, 4);
            size_t At = 8;
            for(u32 I = 0; I < Count && At + 9 <= C->History.size(); ++I)
            {
                u8 FromUser = (u8)C->History[At];
                u32 TimeLen = 0, TextLen = 0;
                memcpy(&TimeLen, C->History.data() + At + 1, 4);
                memcpy(&TextLen, C->History.data() + At + 5, 4);
                At += 9;
                if(At + (size_t)TimeLen + TextLen > C->History.size()) { break; }
                TermHistoryEntry V = {};
                V.from_user = FromUser;
                V.time = String{(u8*)C->History.data() + At, (s64)TimeLen};
                V.text = String{(u8*)C->History.data() + At + TimeLen, (s64)TextLen};
                At += (size_t)TimeLen + TextLen;
                C->HistoryViews.push_back(V);
            }
            C->HistoryVersion++;
            Changed = true;
        }
        else if(Type == MSG_FIND_STATE && Payload.size() >= sizeof(C->FindState))
        {
            memcpy(C->FindState, Payload.data(), sizeof(C->FindState));
            C->FindMarks.resize((Payload.size() - sizeof(C->FindState)) / 2);
            if(!C->FindMarks.empty()) { memcpy(C->FindMarks.data(), Payload.data() + sizeof(C->FindState), C->FindMarks.size() * 2); }
            Changed = true;
        }
        else if(Type == MSG_SESSIONS && Payload.size() >= 4)
        {
            // --- Claude Code's sessions: views onto the payload kept whole ---
            C->Sessions = Payload;
            C->SessionViews.clear();
            u32 Count = 0;
            memcpy(&Count, C->Sessions.data(), 4);
            size_t At = 4;
            for(u32 I = 0; I < Count && At + sizeof(msg_session) <= C->Sessions.size(); ++I)
            {
                msg_session M = {};
                memcpy(&M, C->Sessions.data() + At, sizeof(M));
                At += sizeof(M);
                if(At + (size_t)M.IdLen + M.CwdLen + M.ProjectLen + M.TitleLen > C->Sessions.size()) { break; }
                u8* P = (u8*)C->Sessions.data() + At;
                TermSession V = {};
                V.id = String{P, (s64)M.IdLen}; P += M.IdLen;
                V.cwd = String{P, (s64)M.CwdLen}; P += M.CwdLen;
                V.project = String{P, (s64)M.ProjectLen}; P += M.ProjectLen;
                V.title = String{P, (s64)M.TitleLen};
                V.modified = M.Modified;
                At += (size_t)M.IdLen + M.CwdLen + M.ProjectLen + M.TitleLen;
                C->SessionViews.push_back(V);
            }
            Changed = true;
        }
        else if(Type == MSG_BYE)
        {
            // the reason, then 0 and BYE_*: quit from the tray (or taken over) closes the window
            size_t Zero = Payload.find('\0');
            C->Status = Payload.substr(0, Zero);
            C->Close = Zero != std::string::npos && Zero + 1 < Payload.size() && (u8)Payload[Zero + 1] == BYE_CLOSE;
            ClientLost(C, C->Status.c_str());
            return 1;
        }
    }
    if(!Alive && C->Connected)
    {
        ClientLost(C, "lost the connection to the moss server");
        Changed = true;
    }
    return Changed;
}

extern "C" void TermClient_Fill(TermClient* C, AppFrameInput* In)
{
    In->find_tab = C->FindState[0];
    In->find_count = (s32)C->FindState[1];
    In->find_index = (s32)C->FindState[2];
    In->find_mark_count = (s64)C->FindMarks.size();
    In->find_marks = C->FindMarks.data();
    In->history_tab = C->HistoryTab;
    In->history_version = C->HistoryVersion;
    In->history_count = (s64)C->HistoryViews.size();
    In->history = C->HistoryViews.data();
    // --- tabs: views onto the strings of the last list ---
    C->TabViews.resize(C->Tabs.size());
    for(size_t I = 0; I < C->Tabs.size(); ++I)
    {
        const msg_tab& M = C->Tabs[I];
        TermTab& V = C->TabViews[I];
        V = TermTab{};
        V.id = M.Id;
        V.request = M.Request;
        V.flags = (M.Flags & TAB_AGENT ? TermTab_Agent : 0) | (M.Flags & TAB_BUSY ? TermTab_Busy : 0) |
                  (M.Flags & TAB_NEEDS_INPUT ? TermTab_NeedsInput : 0) | (M.Flags & TAB_EXITED ? TermTab_Exited : 0) |
                  (M.Flags & TAB_RUNNING ? TermTab_Running : 0) | (M.Flags & TAB_ACCOUNT_HELD ? TermTab_AccountHeld : 0);
        V.cpu = M.Cpu;
        std::string* S = &C->Strings[I * 8];
        V.key = String{(u8*)S[0].data(), (s64)S[0].size()};
        V.name = String{(u8*)S[1].data(), (s64)S[1].size()};
        V.cwd = String{(u8*)S[2].data(), (s64)S[2].size()};
        V.notice = String{(u8*)S[3].data(), (s64)S[3].size()};
        V.agent = String{(u8*)S[4].data(), (s64)S[4].size()};
        V.owner = String{(u8*)S[5].data(), (s64)S[5].size()};
        V.account = String{(u8*)S[6].data(), (s64)S[6].size()};
        V.account_was = String{(u8*)S[7].data(), (s64)S[7].size()};
    }
    // --- grids ---
    C->GridViews.resize(C->Grids.size());
    for(size_t I = 0; I < C->Grids.size(); ++I)
    {
        const client_grid* G = C->Grids[I];
        TermGrid& V = C->GridViews[I];
        V = TermGrid{};
        V.tab_id = G->TabId;
        V.cols = G->Cols;
        V.rows = G->Rows;
        V.cells = (const TermCell*)G->Cells.data();
        V.cursor_x = G->Head.CursorX;
        V.cursor_y = G->Head.CursorY;
        V.cursor_shape = G->Head.CursorShape;
        V.flags = G->Head.Flags & ~(u32)CELLS_FULL; // TermGrid_* match CELLS_* bit for bit
        V.mouse_mode = G->Head.MouseMode;
        V.scroll_offset = G->Head.ScrollOffset;
        V.scrollback = G->Head.Scrollback;
        V.version = G->Version;
    }
    In->connected = C->Connected;
    In->status = String{(u8*)C->Status.data(), (s64)C->Status.size()};
    In->tab_count = (s64)C->TabViews.size();
    In->tabs = C->TabViews.data();
    In->session_count = (s64)C->SessionViews.size();
    In->sessions = C->SessionViews.data();
    In->grid_count = (s64)C->GridViews.size();
    In->grids = C->GridViews.data();
    In->tabs_received = C->Connected && C->TabsReceived;
    C->TabsChanged = false;
}

extern "C" void TermClient_Apply(TermClient* C, const AppOutput* Out)
{
    if(!C->Connected) { return; }
    TIMED_BLOCK(ClientApply);
    if(Out->term_watch_changed)
    {
        std::string P;
        u32 Count = (u32)Out->term_watch_count;
        P.append((const char*)&Count, 4);
        for(s64 I = 0; I < Out->term_watch_count; ++I)
        {
            msg_watch W = {Out->term_watches[I].tab_id, Out->term_watches[I].cols, Out->term_watches[I].rows, (u32)(Out->term_watches[I].shown != 0)};
            P.append((const char*)&W, sizeof(W));
        }
        ClientSend(C, MSG_WATCH, P);
    }
    for(s64 I = 0; I < Out->new_tab_count; ++I)
    {
        const TermNewTab& N = Out->new_tabs[I];
        msg_new_tab M = {N.request, N.kind, N.cols, N.rows};
        std::string P((const char*)&M, sizeof(M));
        P.append((const char*)N.cwd.data, (size_t)N.cwd.count);
        P.push_back(0);
        P.append((const char*)N.session.data, (size_t)N.session.count);
        P.push_back(0);
        P.append((const char*)N.command.data, (size_t)N.command.count);
        ClientSend(C, MSG_NEW_TAB, P);
    }
    for(s64 I = 0; I < Out->input_count; ++I)
    {
        std::string P((const char*)&Out->inputs[I].tab_id, 4);
        P.append((const char*)Out->inputs[I].bytes.data, (size_t)Out->inputs[I].bytes.count);
        Term_LatencyMark("client_send");
        ClientSend(C, MSG_INPUT, P);
    }
    if(C->Connected && (bool32)(Out->want_sessions != 0) != C->WantSessions)
    {
        C->WantSessions = Out->want_sessions != 0;
        u32 On = C->WantSessions;
        ClientSend(C, MSG_WANT_SESSIONS, std::string((const char*)&On, 4));
    }
    for(s64 I = 0; I < Out->scroll_count; ++I)
    {
        std::string P((const char*)&Out->scrolls[I].tab_id, 4);
        P.append((const char*)&Out->scrolls[I].lines, 4);
        ClientSend(C, MSG_SCROLL, P);
    }
    if(Out->link_tab)
    {
        u32 Ask[2] = {Out->link_tab, Out->link_id};
        ClientSend(C, MSG_LINK_GET, std::string((const char*)Ask, sizeof(Ask)));
    }
    if(Out->account_ok)
    {
        ClientSend(C, MSG_ACCOUNT_OK, std::string((const char*)&Out->account_ok, 4));
    }
    if(Out->history_get)
    {
        ClientSend(C, MSG_HISTORY_GET, std::string((const char*)&Out->history_get, 4));
    }
    if(Out->find_send)
    {
        u32 Head[2] = {Out->find_tab, Out->find_flags};
        std::string P((const char*)Head, sizeof(Head));
        P.append((const char*)Out->find_text.data, (size_t)Out->find_text.count);
        ClientSend(C, MSG_FIND, P);
    }
    for(s64 I = 0; I < Out->close_count; ++I)
    {
        ClientSend(C, MSG_CLOSE_TAB, std::string((const char*)&Out->closes[I], 4));
    }
    if(Out->focus_tab != C->SentFocus)
    {
        C->SentFocus = Out->focus_tab;
        ClientSend(C, MSG_FOCUS, std::string((const char*)&Out->focus_tab, 4));
    }
}

extern "C" String TermClient_TakeLink(TermClient* C, Arena* A)
{
    if(C->Link.empty()) { return String{}; }
    String S = Str_PushCopy(A, String{(u8*)C->Link.data(), (s64)C->Link.size()});
    C->Link.clear();
    return S;
}

extern "C" String TermClient_TakeView(TermClient* C, Arena* A, u32* Tab)
{
    *Tab = C->ViewTab;
    String Path = C->ViewTab ? Str_PushCopy(A, String{(u8*)C->ViewPath.data(), (s64)C->ViewPath.size()}) : String{};
    C->ViewTab = 0;
    C->ViewPath.clear();
    return Path;
}

extern "C" String TermClient_TakeClipboard(TermClient* C, Arena* A)
{
    if(C->Clipboard.empty()) { return String{}; }
    String S = Str_PushCopy(A, String{(u8*)C->Clipboard.data(), (s64)C->Clipboard.size()});
    C->Clipboard.clear();
    return S;
}

extern "C" b32 TermClient_TakeBell(TermClient* C)
{
    b32 B = C->Bell;
    C->Bell = false;
    return B;
}

extern "C" String TermClient_ScreenText(TermClient* C, u32 TabId, Arena* A, b32 WithAttrs)
{
    client_grid* G = ClientGrid(C, TabId);
    if(!G) { return String{}; }
    std::string Out;
    for(s32 Y = 0; Y < G->Rows; ++Y)
    {
        std::string Line;
        size_t Ink = 0;
        u32 PrevFg = 0, PrevBg = 0, PrevAttr = 0;
        for(s32 X = 0; X < G->Cols; ++X)
        {
            const cell& K = G->Cells[(size_t)Y * G->Cols + X];
            if(K.Flags & CELL_WIDE_CONT) { continue; }
            if(WithAttrs && (K.Fg != PrevFg || K.Bg != PrevBg || K.Attr != PrevAttr))
            {
                char Buf[64];
                snprintf(Buf, sizeof(Buf), "{%x/%x/%x}", K.Fg, K.Bg, K.Attr);
                Line += Buf;
                PrevFg = K.Fg; PrevBg = K.Bg; PrevAttr = K.Attr;
            }
            AppendUtf8(Line, K.Codepoint >= 0x20 ? K.Codepoint : ' ');
            if(K.Codepoint > ' ') { Ink = Line.size(); }
        }
        Line.resize(Ink);
        Out += Line;
        Out += "\n";
    }
    while(Out.size() >= 2 && Out[Out.size() - 1] == '\n' && Out[Out.size() - 2] == '\n') { Out.pop_back(); }
    return Str_PushCopy(A, String{(u8*)Out.data(), (s64)Out.size()});
}

extern "C" void TermClient_Stats(TermClient* C, u64* BytesIn, u64* BytesOut, u64* MessagesIn)
{
    *BytesIn = C->BytesIn;
    *BytesOut = C->BytesOut;
    *MessagesIn = C->MessagesIn;
}

extern "C" b32 TermClient_FindText(TermClient* C, u32 TabId, String Text, s32* Col, s32* Row)
{
    client_grid* G = ClientGrid(C, TabId);
    if(!G || !Text.count) { return 0; }
    std::vector<u32> Want;
    for(s64 I = 0; I < Text.count;) { size_t At = (size_t)I; Want.push_back(DecodeUtf8((const char*)Text.data, (size_t)Text.count, &At)); I = (s64)At; }
    for(s32 Y = 0; Y < G->Rows; ++Y)
    {
        // the row's codepoints with the cell each starts at (wide characters' second halves skipped)
        std::vector<u32> Cps;
        std::vector<s32> Cols;
        for(s32 X = 0; X < G->Cols; ++X)
        {
            const cell& K = G->Cells[(size_t)Y * G->Cols + X];
            if(K.Flags & CELL_WIDE_CONT) { continue; }
            Cps.push_back(K.Codepoint ? K.Codepoint : ' ');
            Cols.push_back(X);
        }
        for(size_t I = 0; I + Want.size() <= Cps.size(); ++I)
        {
            if(memcmp(&Cps[I], Want.data(), Want.size() * sizeof(u32)) != 0) { continue; }
            *Col = Cols[I];
            *Row = Y;
            return 1;
        }
    }
    return 0;
}

extern "C" void TermClient_Debug(TermClient* C, u32 What)
{
    ClientSend(C, MSG_DEBUG, std::string((const char*)&What, 4));
}

extern "C" void TermClient_Destroy(TermClient* C)
{
    IpcClose(C->Conn);
    for(client_grid* G : C->Grids) { delete G; }
    delete C;
}
