#pragma once
#include <list>

namespace WebmMfSourceLib
{

//class WebmMfSource;

class WebmMfStream : public IMFMediaStream
{
    WebmMfStream(const WebmMfStream&);
    WebmMfStream& operator=(const WebmMfStream&);

protected:

    WebmMfStream(
        WebmMfSource*,
        IMFStreamDescriptor*,
        const mkvparser::Track*);

public:

    virtual ~WebmMfStream();

    //IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(const IID&, void**);
    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();

    //IMFMediaEventGenerator

    HRESULT STDMETHODCALLTYPE GetEvent(
        DWORD dwFlags,
        IMFMediaEvent**);

    HRESULT STDMETHODCALLTYPE BeginGetEvent(
        IMFAsyncCallback*,
        IUnknown*);

    HRESULT STDMETHODCALLTYPE EndGetEvent(
        IMFAsyncResult*,
        IMFMediaEvent**);

    HRESULT STDMETHODCALLTYPE QueueEvent(
        MediaEventType met,
        REFGUID guidExtendedType,
        HRESULT hrStatus,
        const PROPVARIANT*);

    //IMFMediaStream

    HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource**);
    HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor**);
    HRESULT STDMETHODCALLTYPE RequestSample(IUnknown*);

    //Local methods and properties

    HRESULT GetCurrMediaTime(LONGLONG&) const;  //not presentation time!

    HRESULT Select();
    HRESULT Deselect();
    bool IsSelected() const;

    HRESULT Restart();

    HRESULT Stop();
    HRESULT Pause();
    HRESULT Shutdown();

    virtual void SetRate(BOOL, float) = 0;

    WebmMfSource* const m_pSource;
    IMFStreamDescriptor* const m_pDesc;
    const mkvparser::Track* const m_pTrack;

protected:

    HRESULT OnStart(const PROPVARIANT& time);
    HRESULT OnSeek(const PROPVARIANT& time);
    virtual void OnStop() = 0;

    virtual const mkvparser::BlockEntry* GetCurrBlock() const = 0;
    virtual HRESULT PopulateSample(IMFSample*) = 0;

private:

    IMFMediaEventQueue* m_pEvents;

    typedef std::list<IMFSample*> samples_t;
    samples_t m_samples;

    typedef std::list<IUnknown*> tokens_t;
    tokens_t m_tokens;

    bool m_bSelected;
    bool m_bEOS;  //indicates whether we have posted EOS event

    void PurgeTokens();
    void PurgeSamples();
    void DeliverSamples();

    HANDLE m_hThread;
    HANDLE m_hQuit;
    HANDLE m_hRequestSample;

    static unsigned __stdcall ThreadProc(void*);
    unsigned Main();

    HRESULT StartThread();
    void StopThread();

    void OnRequestSample();


};

}  //end namespace WebmMfSourceLib
