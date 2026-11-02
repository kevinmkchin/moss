// term: voice mode (the app's dictation and read-aloud, AppOutput.voice_* and speak_*): Windows' speech recognition
// while the dictation key is held, and speech synthesis for a reply. Each runs on its own thread; the app sees the
// text through Voice_Poll, and the window is woken with the wake message. (Included from term_cxx.cpp.)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechRecognition.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>
#include <memory>
#include <string>

static struct {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE dictation = NULL;             // the dictation thread
    HANDLE release = NULL;               // the key came up: the dictation finishes
    HWND hwnd = NULL;
    u32 wake = 0;
    u8 state = VoiceState_Idle;
    std::string heard, message;          // since the last Voice_Poll
    HANDLE speech = NULL;                // the thread reading aloud
    HANDLE speech_stop = NULL;
} g_Voice;

static DWORD WINAPI VoiceDictate(void *)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::string message;
    try {
        winrt::Windows::Media::SpeechRecognition::SpeechRecognizer recognizer;
        recognizer.CompileConstraintsAsync().get();   // no grammar: the default, dictation
        auto session = recognizer.ContinuousRecognitionSession();
        session.ResultGenerated([](auto const &, auto const &args) {
            if (args.Result().Status() != winrt::Windows::Media::SpeechRecognition::SpeechRecognitionResultStatus::Success) return;
            std::string text = winrt::to_string(args.Result().Text());
            AcquireSRWLockExclusive(&g_Voice.lock);
            if (!g_Voice.heard.empty() && !text.empty()) g_Voice.heard += ' ';
            g_Voice.heard += text;
            ReleaseSRWLockExclusive(&g_Voice.lock);
        });
        session.StartAsync().get();
        WaitForSingleObject(g_Voice.release, INFINITE);
        session.StopAsync().get();
    } catch (winrt::hresult_error const &e) {
        message = "Dictation failed: " + winrt::to_string(e.message());
        Logf("voice", "dictation failed (0x%08X): %s", (unsigned)e.code(), message.c_str());
    }
    AcquireSRWLockExclusive(&g_Voice.lock);
    g_Voice.message = message;
    g_Voice.state = VoiceState_Idle;
    ReleaseSRWLockExclusive(&g_Voice.lock);
    PostMessageW(g_Voice.hwnd, g_Voice.wake, 0, 0);
    return 0;
}

extern "C" void Voice_Start(void *Hwnd, u32 WakeMessage)
{
    if (g_Voice.state != VoiceState_Idle) return;
    if (g_Voice.dictation) {   // the last one is finishing: its thread ends right after it goes idle
        WaitForSingleObject(g_Voice.dictation, INFINITE);
        CloseHandle(g_Voice.dictation);
        g_Voice.dictation = NULL;
    }
    if (!g_Voice.release) g_Voice.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_Voice.release);
    g_Voice.hwnd = (HWND)Hwnd;
    g_Voice.wake = WakeMessage;
    AcquireSRWLockExclusive(&g_Voice.lock);
    g_Voice.heard.clear();
    g_Voice.state = VoiceState_Listening;
    ReleaseSRWLockExclusive(&g_Voice.lock);
    g_Voice.dictation = CreateThread(NULL, 0, VoiceDictate, NULL, 0, NULL);
    if (!g_Voice.dictation) {
        AcquireSRWLockExclusive(&g_Voice.lock);
        g_Voice.message = "Dictation failed: no thread";
        g_Voice.state = VoiceState_Idle;
        ReleaseSRWLockExclusive(&g_Voice.lock);
    }
    Logf("voice", "listening");
}

extern "C" void Voice_Stop(void)
{
    AcquireSRWLockExclusive(&g_Voice.lock);
    if (g_Voice.state == VoiceState_Listening) {
        g_Voice.state = VoiceState_Finishing;
        SetEvent(g_Voice.release);
    }
    ReleaseSRWLockExclusive(&g_Voice.lock);
}

extern "C" u8 Voice_Poll(Arena *arena, String *text, String *message)
{
    AcquireSRWLockExclusive(&g_Voice.lock);
    *text = g_Voice.heard.empty() ? String{} : Str_PushCopy(arena, String{ (u8 *)g_Voice.heard.data(), (s64)g_Voice.heard.size() });
    *message = g_Voice.message.empty() ? String{} : Str_PushCopy(arena, String{ (u8 *)g_Voice.message.data(), (s64)g_Voice.message.size() });
    g_Voice.heard.clear();
    g_Voice.message.clear();
    u8 state = g_Voice.state;
    ReleaseSRWLockExclusive(&g_Voice.lock);
    return state;
}

struct VoiceSpeech { std::string text; HANDLE stop; };

static DWORD WINAPI VoiceReadAloud(void *Arg)
{
    std::unique_ptr<VoiceSpeech> speech((VoiceSpeech *)Arg);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer synth;
        auto stream = synth.SynthesizeTextToStreamAsync(winrt::to_hstring(speech->text)).get();
        winrt::Windows::Media::Playback::MediaPlayer player;
        HANDLE ended = CreateEventW(NULL, TRUE, FALSE, NULL);
        player.MediaEnded([ended](auto const &, auto const &) { SetEvent(ended); });
        player.MediaFailed([ended](auto const &, auto const &) { SetEvent(ended); });
        player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream, stream.ContentType()));
        player.Play();
        HANDLE events[2] = { ended, speech->stop };
        WaitForMultipleObjects(2, events, FALSE, INFINITE);
        player.Close();
        CloseHandle(ended);
    } catch (winrt::hresult_error const &e) {
        Logf("voice", "read aloud failed (0x%08X): %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
    }
    return 0;
}

extern "C" void Voice_SpeakStop(void)
{
    if (!g_Voice.speech) return;
    SetEvent(g_Voice.speech_stop);
    WaitForSingleObject(g_Voice.speech, INFINITE);
    CloseHandle(g_Voice.speech);
    CloseHandle(g_Voice.speech_stop);
    g_Voice.speech = NULL;
    g_Voice.speech_stop = NULL;
}

extern "C" void Voice_Speak(String text)
{
    Voice_SpeakStop();
    if (!text.count) return;
    VoiceSpeech *speech = new VoiceSpeech{ std::string((const char *)text.data, (size_t)text.count), CreateEventW(NULL, TRUE, FALSE, NULL) };
    HANDLE thread = CreateThread(NULL, 0, VoiceReadAloud, speech, 0, NULL);
    if (!thread) {
        CloseHandle(speech->stop);
        delete speech;
        return;
    }
    g_Voice.speech = thread;
    g_Voice.speech_stop = speech->stop;
}
