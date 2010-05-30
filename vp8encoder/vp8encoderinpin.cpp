// Copyright (c) 2010 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license and patent
// grant that can be found in the LICENSE file in the root of the source
// tree. All contributing project authors may be found in the AUTHORS
// file in the root of the source tree.

#pragma warning(disable:4505)  //unreferenced local function has been removed
#include "vp8encoderfilter.hpp"
#include "vp8encoderoutpin.hpp"
#include "mediatypeutil.hpp"
#include "webmtypes.hpp"
#include "vp8cx.h"
#include <vfwmsgs.h>
#include <uuids.h>
#include <cassert>
#include <amvideo.h>
#ifdef _DEBUG
#include "odbgstream.hpp"
#include <iomanip>
using std::endl;
using std::hex;
using std::dec;
#endif

namespace VP8EncoderLib
{
    
Inpin::Inpin(Filter* p) :
    Pin(p, PINDIR_INPUT, L"input"),
    m_bEndOfStream(false),
    m_bFlush(false),
    m_bDiscontinuity(true),
    m_buf(0),
    m_buflen(0)
{
    AM_MEDIA_TYPE mt;
    
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_YV12;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = 0;
    mt.formattype = GUID_NULL;
    mt.pUnk = 0;
    mt.cbFormat = 0;
    mt.pbFormat = 0;
    
    m_preferred_mtv.Add(mt);

    mt.subtype = WebmTypes::MEDIASUBTYPE_I420;
    m_preferred_mtv.Add(mt);
    
    mt.subtype = MEDIASUBTYPE_YUY2;
    m_preferred_mtv.Add(mt);

    mt.subtype = MEDIASUBTYPE_YUYV;
    m_preferred_mtv.Add(mt);
}


Inpin::~Inpin()
{
    PurgePending();
    
    delete[] m_buf;
}


HRESULT Inpin::QueryInterface(const IID& iid, void** ppv)
{
    if (ppv == 0)
        return E_POINTER;
        
    IUnknown*& pUnk = reinterpret_cast<IUnknown*&>(*ppv);
    
    if (iid == __uuidof(IUnknown))
        pUnk = static_cast<IPin*>(this);
        
    else if (iid == __uuidof(IPin))
        pUnk = static_cast<IPin*>(this);
        
    else if (iid == __uuidof(IMemInputPin))
        pUnk = static_cast<IMemInputPin*>(this);
        
    else
    {
        pUnk = 0;
        return E_NOINTERFACE;
    }
    
    pUnk->AddRef();
    return S_OK;
}
    

ULONG Inpin::AddRef()
{
    return m_pFilter->AddRef();
}


ULONG Inpin::Release()
{
    return m_pFilter->Release();
}


HRESULT Inpin::Connect(IPin*, const AM_MEDIA_TYPE*)
{
    return E_UNEXPECTED;  //for output pins only
}


HRESULT Inpin::QueryInternalConnections( 
    IPin** pa,
    ULONG* pn)
{
    if (pn == 0)
        return E_POINTER;
        
    Filter::Lock lock;
    
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;
        
    const ULONG m = 2;  //number of output pins
        
    ULONG& n = *pn;
    
    if (n == 0)
    {
        if (pa == 0)  //query for required number
        {
            n = m;
            return S_OK;
        }
        
        return S_FALSE;  //means "insufficient number of array elements"
    }
    
    if (n < m)
    {
        n = 0;
        return S_FALSE;  //means "insufficient number of array elements"
    }
        
    if (pa == 0)
    {
        n = 0;
        return E_POINTER;
    }
    
    pa[0] = &m_pFilter->m_outpin_video;    
    pa[0]->AddRef();
    
    pa[1] = &m_pFilter->m_outpin_preview;
    pa[1]->AddRef();

    n = m;    
    return S_OK;        
}


HRESULT Inpin::ReceiveConnection( 
    IPin* pin,
    const AM_MEDIA_TYPE* pmt)
{
    if (pin == 0)
        return E_INVALIDARG;
        
    if (pmt == 0)
        return E_INVALIDARG;
        
    Filter::Lock lock;
        
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;
        
    if (m_pFilter->m_state != State_Stopped)
        return VFW_E_NOT_STOPPED;

    if (bool(m_pPinConnection))
        return VFW_E_ALREADY_CONNECTED;
        
    m_connection_mtv.Clear();
    
    hr = QueryAccept(pmt);
        
    if (hr != S_OK)
        return VFW_E_TYPE_NOT_ACCEPTED;
        
    const AM_MEDIA_TYPE& mt = *pmt;
            
    hr = m_connection_mtv.Add(mt);
    
    if (FAILED(hr))
        return hr;
        
    m_pPinConnection = pin;
    
    m_pFilter->m_outpin_video.OnInpinConnect(mt);
    m_pFilter->m_outpin_preview.OnInpinConnect(mt);

    return S_OK;
}


HRESULT Inpin::EndOfStream()
{
    Filter::Lock lock;
        
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;

    if (!bool(m_pPinConnection))
        return VFW_E_NOT_CONNECTED;

    if (m_pFilter->m_state == State_Stopped)
        return VFW_E_NOT_RUNNING;

    if (m_bFlush)
        return S_FALSE;  //TODO: correct return value here?

    if (m_bEndOfStream)
        return S_FALSE;

    m_bEndOfStream = true;
   
    const vpx_codec_err_t err = vpx_codec_encode(&m_ctx, 0, 0, 0, 0, 0);
    err;
    assert(err == VPX_CODEC_OK);  //TODO
    
    vpx_codec_iter_t iter = 0;

    for (;;)
    {    
        const vpx_codec_cx_pkt_t* const pkt = vpx_codec_get_cx_data(&m_ctx, &iter);
        
        if (pkt == 0)
            break;
            
        assert(pkt->kind == VPX_CODEC_CX_FRAME_PKT);
        
        AppendFrame(pkt);
    }
        
    OutpinVideo& outpin = m_pFilter->m_outpin_video;

    while (!m_pending.empty())
    {
        if (!bool(outpin.m_pAllocator))
            break;

        lock.Release();
        
        GraphUtil::IMediaSamplePtr pOutSample;

        const HRESULT hrGetBuffer = outpin.m_pAllocator->GetBuffer(&pOutSample, 0, 0, 0);
        
        hr = lock.Seize(m_pFilter);

        if (FAILED(hr))
            return hr;
            
        if (FAILED(hrGetBuffer))
            break;
            
        assert(bool(pOutSample));
        
        PopulateSample(pOutSample);  //consume pending frame
        
        if (!bool(outpin.m_pInputPin))
            break;
        
        lock.Release();
    
        const HRESULT hrReceive = outpin.m_pInputPin->Receive(pOutSample);
        
        hr = lock.Seize(m_pFilter);

        if (FAILED(hr))
            return hr;

        if (hrReceive != S_OK)
            break;
    }
        
    //We hold the lock.

    if (IPin* pPin = m_pFilter->m_outpin_preview.m_pPinConnection)
    {
        lock.Release();

        hr = pPin->EndOfStream();
        
        hr = lock.Seize(m_pFilter);
        assert(SUCCEEDED(hr));  //TODO
    }   

    //We hold the lock.

    if (IPin* pPin = outpin.m_pPinConnection)
    {
        lock.Release();
        hr = pPin->EndOfStream();
    }
   
    return S_OK;
}
    

HRESULT Inpin::BeginFlush()
{
    Filter::Lock lock;
        
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;
        
    if (!bool(m_pPinConnection))
        return VFW_E_NOT_CONNECTED;

    //TODO: need this?
    //if (m_bFlush)
    //    return S_FALSE;
        
    m_bFlush = true;
    
    //We hold the lock
    
    if (IPin* pPin = m_pFilter->m_outpin_preview.m_pPinConnection)
    {
        lock.Release();

        hr = pPin->BeginFlush();
        
        hr = lock.Seize(m_pFilter);
        assert(SUCCEEDED(hr));  //TODO
    }   

    //We hold the lock

    if (IPin* pPin = m_pFilter->m_outpin_video.m_pPinConnection)
    {
        lock.Release();
        hr = pPin->BeginFlush();
    }
    
    return S_OK;
}
    

HRESULT Inpin::EndFlush()
{
    Filter::Lock lock;
        
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;

    if (!bool(m_pPinConnection))
        return VFW_E_NOT_CONNECTED;

    m_bFlush = false;
    
    //We hold the lock
    
    if (IPin* pPin = m_pFilter->m_outpin_preview.m_pPinConnection)
    {
        lock.Release();

        hr = pPin->EndFlush();
        
        hr = lock.Seize(m_pFilter);
        assert(SUCCEEDED(hr));  //TODO
    }   

    //We hold the lock

    if (IPin* pPin = m_pFilter->m_outpin_video.m_pPinConnection)
    {
        lock.Release();
        hr = pPin->EndFlush();
    }
    
    return S_OK;
}


HRESULT Inpin::NewSegment( 
    REFERENCE_TIME st,
    REFERENCE_TIME sp,
    double r)
{
    Filter::Lock lock;
        
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;

    if (!bool(m_pPinConnection))
        return VFW_E_NOT_CONNECTED;

    //We hold the lock
    
    if (IPin* pPin = m_pFilter->m_outpin_preview.m_pPinConnection)
    {
        lock.Release();

        hr = pPin->NewSegment(st, sp, r);
        
        hr = lock.Seize(m_pFilter);
        assert(SUCCEEDED(hr));  //TODO
    }   

    //We hold the lock

    if (IPin* pPin = m_pFilter->m_outpin_video.m_pPinConnection)
    {
        lock.Release();
        hr = pPin->NewSegment(st, sp, r);
    }
    
    return S_OK;
}


HRESULT Inpin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    if (pmt == 0)
        return E_INVALIDARG;
        
    const AM_MEDIA_TYPE& mt = *pmt;
    
    if (mt.majortype != MEDIATYPE_Video)
        return S_FALSE;
        
    if (mt.subtype == MEDIASUBTYPE_YV12)
        __noop;

    else if (mt.subtype == WebmTypes::MEDIASUBTYPE_I420)
        __noop;

    else if (mt.subtype == MEDIASUBTYPE_YUY2)
        __noop;

    else if (mt.subtype == MEDIASUBTYPE_YUYV)
        __noop;

    else
        return S_FALSE;
        
    if (mt.formattype != FORMAT_VideoInfo)  //TODO: liberalize
        return S_FALSE;
        
    if (mt.pbFormat == 0)
        return S_FALSE;

    if (mt.cbFormat < sizeof(VIDEOINFOHEADER))
        return S_FALSE;
        
    const VIDEOINFOHEADER& vih = (VIDEOINFOHEADER&)(*mt.pbFormat);
    const BITMAPINFOHEADER& bmih = vih.bmiHeader;
    
    if (bmih.biSize != sizeof(BITMAPINFOHEADER))  //TODO: liberalize
        return S_FALSE;
        
    if (bmih.biWidth <= 0)
        return S_FALSE;
        
    if (bmih.biWidth % 2)  //TODO
        return S_FALSE;
        
    if (bmih.biHeight <= 0)
        return S_FALSE;
        
    if (bmih.biHeight % 2)  //TODO
        return S_FALSE;
        
    if (bmih.biCompression != mt.subtype.Data1)
        return S_FALSE;
        
    return S_OK;
}


HRESULT Inpin::GetAllocator(IMemAllocator** p)
{
    if (p)
        *p = 0;
        
    return VFW_E_NO_ALLOCATOR;
}



HRESULT Inpin::NotifyAllocator( 
    IMemAllocator* pAllocator,
    BOOL)
{
    if (pAllocator == 0)
        return E_INVALIDARG;
        
    ALLOCATOR_PROPERTIES props;
    
    const HRESULT hr = pAllocator->GetProperties(&props);
    hr;
    assert(SUCCEEDED(hr));
    
#ifdef _DEBUG    
    wodbgstream os;
    os << "vp8enc::inpin::NotifyAllocator: props.cBuffers="
       << props.cBuffers
       << " cbBuffer="
       << props.cbBuffer
       << " cbAlign="
       << props.cbAlign
       << " cbPrefix="
       << props.cbPrefix
       << endl;
#endif    
    
    return S_OK;
}


HRESULT Inpin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pp)
{
    if (pp == 0)
        return E_POINTER;
        
    ALLOCATOR_PROPERTIES& p = *pp;
    p;
        
    return S_OK;
}


HRESULT Inpin::Receive(IMediaSample* pInSample)
{
    if (pInSample == 0)
        return E_INVALIDARG;

    if (pInSample->IsPreroll() == S_OK)  //bogus for encode
        return S_OK;
        
    Filter::Lock lock;
    
    HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return hr;
    
    if (!bool(m_pPinConnection))
        return VFW_E_NOT_CONNECTED;

    if (m_pFilter->m_state == State_Stopped)
        return VFW_E_NOT_RUNNING;

    if (m_bEndOfStream)
        return VFW_E_SAMPLE_REJECTED_EOS;

    if (m_bFlush)
        return S_FALSE;
        
    const AM_MEDIA_TYPE& mt = m_connection_mtv[0];
    assert(mt.formattype == FORMAT_VideoInfo);
    assert(mt.cbFormat >= sizeof(VIDEOINFOHEADER));
    assert(mt.pbFormat);
    
    const VIDEOINFOHEADER& vih = (VIDEOINFOHEADER&)(*mt.pbFormat);
    const BITMAPINFOHEADER& bmih = vih.bmiHeader;
    
    const LONG w = bmih.biWidth;
    assert(w > 0);
    assert((w % 2) == 0);  //TODO
    
    const LONG h = bmih.biHeight;
    assert(h > 0);
    assert((h % 2) == 0);  //TODO
        
    const long len = pInSample->GetActualDataLength();
    len;
    assert(len >= 0);
    
    img_fmt_t fmt;
        
    if (mt.subtype == MEDIASUBTYPE_YV12)
        fmt = IMG_FMT_YV12;

    else if (mt.subtype == WebmTypes::MEDIASUBTYPE_I420)
        fmt = IMG_FMT_I420;

    else if ((mt.subtype == MEDIASUBTYPE_YUY2) ||
             (mt.subtype == MEDIASUBTYPE_YUYV))
    {
        fmt = IMG_FMT_YUY2;
    }
    else
    {
        assert(false);
        return E_FAIL;
    }
    
    BYTE* inbuf;
    
    hr = pInSample->GetPointer(&inbuf);
    assert(SUCCEEDED(hr));
    assert(inbuf);
    
    BYTE* imgbuf;
    
    switch (fmt)
    {
        case IMG_FMT_YV12:
        case IMG_FMT_I420:
        {            
            assert(len == (w*h + 2*((w+1)/2)*((h+1)/2)));
            
            imgbuf = inbuf;
            assert(imgbuf);

            break;
        }
        case IMG_FMT_YUY2:
        {
            assert(len == ((2*w) * h));            

            fmt = IMG_FMT_YV12;

            imgbuf = ConvertYUY2ToYV12(inbuf, w, h);
            assert(imgbuf);
            
            break;
        }
        default:
            assert(false);
            return E_FAIL;
    }
    
    vpx_image_t img_;
    vpx_image_t* const img = vpx_img_wrap(&img_, fmt, w, h, 1, imgbuf);
    assert(img);
    assert(img == &img_);
    
    //TODO: set this based on vih.rcSource
    const int status = vpx_img_set_rect(img, 0, 0, w, h);
    status;
    assert(status == 0);
    
    m_pFilter->m_outpin_preview.Render(lock, img);

    Outpin& outpin = m_pFilter->m_outpin_video;
    
    //TODO: should we bother checking these here?
    
    if (!bool(outpin.m_pPinConnection))
        return S_FALSE;
        
    if (!bool(outpin.m_pInputPin))
        return S_FALSE;
        
    if (!bool(outpin.m_pAllocator))
        return VFW_E_NO_ALLOCATOR;
        
    __int64 st, sp;

    const HRESULT hrTime = pInSample->GetTime(&st, &sp);
    
    if (FAILED(hrTime))
    {
        if (m_reftime < 0)
            return S_OK;  //throw this sample away
        
        st = m_reftime;

        if (vih.AvgTimePerFrame > 0)
            sp = st + vih.AvgTimePerFrame;
        else
            sp = st + 20 * 10000;  //add 20ms  //TODO: make a better estimate
    }    
    else if (hrTime == S_OK)  //have both start and stop times
    {
        if (st > sp)
            return VFW_E_START_TIME_AFTER_END;
            
        if (sp <= 0)
            return S_OK;  //throw sample away
            
        if (st < 0)
            st = 0;
            
        if (m_reftime < 0)
        {
            if (st > 0)
                st = 0;
                
            m_reftime = st;
        }
        else if (st < m_reftime)
            st = m_reftime;

        if (sp <= st)
        {
            if (vih.AvgTimePerFrame > 0)
                sp = st + vih.AvgTimePerFrame;
            else
                sp = st + 20 * 10000;  //20ms
        }
    }
    else if (st < 0)
        return S_OK;  //throw sample away
        
    else
    {
        if (m_reftime < 0)
        {
            if (st > 0)
                st = 0;
                
            m_reftime = st;
        }
        else if (st < m_reftime)
            st = m_reftime;
            
        if (vih.AvgTimePerFrame > 0)
            sp = st + vih.AvgTimePerFrame;
        else
            sp = st + 20 * 10000;  //add 20ms
    }
    
    assert(m_reftime >= 0);
    assert(st >= m_reftime);
    assert(sp > st);
    
    m_reftime = sp;

    const __int64 duration_ = sp - st;
    assert(duration_ > 0);
    
    const unsigned long d = static_cast<unsigned long>(duration_);
        
    //odbgstream os;
    //os << std::fixed << std::setprecision(1);
    //os << "vp8enc::inpin::receive:"
    //   << " st[ms]=" << st / 10000
    //   << " sp[ms]=" << sp / 10000
    //   << " dt[ms]=" << duration_ / 10000
    //   << endl;
    
    hr = pInSample->IsDiscontinuity();
    const bool bDiscontinuity = (hr == S_OK);

    vpx_enc_frame_flags_t f = 0;
    
    if (bDiscontinuity || (st <= 0))
        f |= VPX_EFLAG_FORCE_KF;
    
    const ULONG deadline = m_pFilter->m_cfg.deadline;
    
    const vpx_codec_err_t err = vpx_codec_encode(&m_ctx, img, st, d, f, deadline);
    err;
    assert(err == VPX_CODEC_OK);  //TODO
    
    vpx_codec_iter_t iter = 0;
    
    for (;;)
    {    
        const vpx_codec_cx_pkt_t* const pkt = vpx_codec_get_cx_data(&m_ctx, &iter);
        
        if (pkt == 0)
            break;
            
        assert(pkt->kind == VPX_CODEC_CX_FRAME_PKT);
        
        AppendFrame(pkt);
    }
        
    while (!m_pending.empty())
    {
        if (!bool(outpin.m_pAllocator))
            return VFW_E_NO_ALLOCATOR;

        lock.Release();
        
        GraphUtil::IMediaSamplePtr pOutSample;

        hr = outpin.m_pAllocator->GetBuffer(&pOutSample, 0, 0, 0);
        
        if (FAILED(hr))
            return hr;
            
        assert(bool(pOutSample));
        
        hr = lock.Seize(m_pFilter);

        if (FAILED(hr))
            return hr;
            
        PopulateSample(pOutSample);  //consume pending frame
        
        if (!bool(outpin.m_pInputPin))
            return S_FALSE;
        
        lock.Release();
    
        hr = outpin.m_pInputPin->Receive(pOutSample);
        
        if (hr != S_OK)
            return hr;
    
        hr = lock.Seize(m_pFilter);

        if (FAILED(hr))
            return hr;
    }
    
    return S_OK;
}


void Inpin::PopulateSample(IMediaSample* p)
{
    assert(p);
    assert(!m_pending.empty());

#ifdef _DEBUG
    {
        AM_MEDIA_TYPE* pmt;
    
        const HRESULT hr = p->GetMediaType(&pmt);
        assert(FAILED(hr) || (pmt == 0));
    }    
#endif

    _COM_SMARTPTR_TYPEDEF(IVP8Sample, __uuidof(IVP8Sample));

    const IVP8SamplePtr pSample(p);
    assert(bool(pSample));
    
    IVP8Sample::Frame& f = pSample->GetFrame();
    assert(f.buf == 0);  //should have already been reclaimed
    
    f = m_pending.front();
    assert(f.buf);
    
    m_pending.pop_front();

    HRESULT hr = p->SetPreroll(FALSE);
    assert(SUCCEEDED(hr));
    
    hr = p->SetDiscontinuity(m_bDiscontinuity ? TRUE : FALSE);
    assert(SUCCEEDED(hr));
    
    m_bDiscontinuity = false;
}


void Inpin::AppendFrame(const vpx_codec_cx_pkt_t* pkt)
{
    assert(pkt);
    assert(pkt->kind == VPX_CODEC_CX_FRAME_PKT);
    
    IVP8Sample::Frame f;
    
    const HRESULT hr = m_pFilter->m_outpin_video.GetFrame(f);
    assert(SUCCEEDED(hr));
    assert(f.buf);
    
    const size_t len_ = pkt->data.frame.sz;
    const long len = static_cast<long>(len_);
    
    const long size = f.buflen - f.off;
    assert(size >= len);
    
    BYTE* tgt = f.buf + f.off;
    //assert(intptr_t(tgt - props.cbPrefix) % props.cbAlign == 0);    
    
    memcpy(tgt, pkt->data.frame.buf, len);
    
    f.len = len;
    
    f.start = pkt->data.frame.pts;
    assert(f.start >= 0);
    
    f.stop = f.start + pkt->data.frame.duration;
    assert(f.stop > f.start);
    
    const uint32_t bKey = pkt->data.frame.flags & VPX_FRAME_IS_KEY;
    
    f.key = bKey ? true : false;
    
    m_pending.push_back(f);
    
#if 0 //def _DEBUG
    odbgstream os;
    os << "vp8encoder::inpin::appendframe: pending.size=" << m_pending.size() << endl;
#endif
}


HRESULT Inpin::ReceiveMultiple(
    IMediaSample** pSamples,
    long n,    //in
    long* pm)  //out
{
    if (pm == 0)
        return E_POINTER;
        
    long& m = *pm;    //out
    m = 0;
    
    if (n <= 0)
        return S_OK;  //weird
    
    if (pSamples == 0)
        return E_INVALIDARG;
        
    for (long i = 0; i < n; ++i)
    {
        IMediaSample* const pSample = pSamples[i];
        assert(pSample);
        
        const HRESULT hr = Receive(pSample);
        
        if (hr != S_OK)
            return hr;
        
        ++m;
    }

    return S_OK;
}


HRESULT Inpin::ReceiveCanBlock()
{
    Filter::Lock lock;
    
    const HRESULT hr = lock.Seize(m_pFilter);
    
    if (FAILED(hr))
        return S_OK;  //?
        
    if (IMemInputPin* pPin = m_pFilter->m_outpin_video.m_pInputPin)
    {
        lock.Release();
        return pPin->ReceiveCanBlock();
    }

    return S_FALSE;
}


HRESULT Inpin::OnDisconnect()
{    
    return m_pFilter->m_outpin_video.OnInpinDisconnect();
}


std::wstring Inpin::GetName() const
{
    return L"YUV";
}


void Inpin::PurgePending()
{
    while (!m_pending.empty())
    {
        IVP8Sample::Frame& f = m_pending.front();
        assert(f.buf);
        
        delete[] f.buf;
        
        m_pending.pop_front();
    }
}
        

HRESULT Inpin::Start()
{
    m_bDiscontinuity = true;
    m_bEndOfStream = false;
    m_bFlush = false;
    m_reftime = _I64_MIN;  //nonce
    
    PurgePending();
    
    const AM_MEDIA_TYPE& mt = m_connection_mtv[0];
    assert(mt.formattype == FORMAT_VideoInfo);
    assert(mt.cbFormat >= sizeof(VIDEOINFOHEADER));
    assert(mt.pbFormat);
    
    const VIDEOINFOHEADER& vih = (VIDEOINFOHEADER&)(*mt.pbFormat);
    const BITMAPINFOHEADER& bmih = vih.bmiHeader;
    
    const LONG w = bmih.biWidth;
    assert(w > 0);
    assert((w % 2) == 0);  //TODO
    
    const LONG h = bmih.biHeight;
    assert(h > 0);
    assert((h % 2) == 0);  //TODO
        
    vpx_codec_iface_t& vp8 = vpx_codec_vp8_cx_algo;

    vpx_codec_enc_cfg_t& tgt = m_cfg;

    vpx_codec_err_t err = vpx_codec_enc_config_default(&vp8, &tgt, 0);
    
    if (err != VPX_CODEC_OK)
        return E_FAIL;
        
    const Filter::Config& src = m_pFilter->m_cfg;
        
    if (src.threads > 0)
        tgt.g_threads = src.threads;
        
    tgt.g_w = w;
    tgt.g_h = h;
    tgt.g_timebase.num = 1;          
    tgt.g_timebase.den = 10000000;  //100-ns ticks
    tgt.g_error_resilient = src.error_resilient ? 1 : 0;
    tgt.g_pass = VPX_RC_ONE_PASS;  //TODO
    tgt.g_lag_in_frames = src.lag_in_frames;

    switch (src.end_usage)
    {
        case kEndUsageVBR:
        default:
            tgt.rc_end_usage = VPX_VBR;
            break;
            
        case kEndUsageCBR:
            tgt.rc_end_usage = VPX_CBR;
            break;
    }
    
    if (src.target_bitrate > 0)
        tgt.rc_target_bitrate = src.target_bitrate;
        
    if (src.min_quantizer >= 0)
        tgt.rc_min_quantizer = src.min_quantizer;
        
    if (src.max_quantizer >= 0)
        tgt.rc_max_quantizer = src.max_quantizer;
        
    tgt.rc_undershoot_pct = src.undershoot_pct;
    tgt.rc_overshoot_pct = src.overshoot_pct;
    
    if (src.decoder_buffer_size > 0)
        tgt.rc_buf_sz = src.decoder_buffer_size;
        
    if (src.decoder_buffer_initial_size > 0)
        tgt.rc_buf_initial_sz = src.decoder_buffer_initial_size;
        
    if (src.decoder_buffer_optimal_size > 0)
        tgt.rc_buf_optimal_sz = src.decoder_buffer_optimal_size;
        
    switch (src.keyframe_mode)
    {
        case kKeyframeModeDefault:
        default:
            break;
            
        case kKeyframeModeDisabled:
            tgt.kf_mode = VPX_KF_DISABLED;
            break;
            
        case kKeyframeModeAuto:
            tgt.kf_mode = VPX_KF_AUTO;
            break;
    }
    
    if (src.keyframe_min_interval >= 0)
        tgt.kf_min_dist = src.keyframe_min_interval;
        
    if (src.keyframe_max_interval >= 0)
        tgt.kf_max_dist = src.keyframe_max_interval;
        
    //TODO: more params here

    err = vpx_codec_enc_init(&m_ctx, &vp8, &tgt, 0);
    
    if (err != VPX_CODEC_OK)
    {
        const char* str = vpx_codec_error_detail(&m_ctx);
        str;
    
        return E_FAIL;
    }

    vp8e_token_partitions token_partitions;
    
    switch (src.token_partitions)
    {
        case 0:
        case 1:
        case 2:
        case 3:
            token_partitions = static_cast<vp8e_token_partitions>(src.token_partitions);
            break;
            
        default:                
            token_partitions = VP8_ONE_TOKENPARTITION;
            break;            
    }
    
    err = vpx_codec_control(&m_ctx, VP8E_SET_TOKEN_PARTITIONS, token_partitions);

    if (err != VPX_CODEC_OK)
    {
        const vpx_codec_err_t err = vpx_codec_destroy(&m_ctx);
        err;
        assert(err == VPX_CODEC_OK);
        
        return E_FAIL;
    }
        
    return S_OK;
}

void Inpin::Stop()
{
    const vpx_codec_err_t err = vpx_codec_destroy(&m_ctx);
    err;
    assert(err == VPX_CODEC_OK);
    
    memset(&m_ctx, 0, sizeof m_ctx);
}


BYTE* Inpin::ConvertYUY2ToYV12(
    const BYTE* srcbuf,
    ULONG w,
    ULONG h)
{
    assert(srcbuf);
    assert((w % 2) == 0);  //TODO
    assert((h % 2) == 0);  //TODO
    
    const ULONG len = w*h + 2*((w+1)/2)*((h+1)/2);
    
    if (m_buflen < len)
    {
        delete[] m_buf;
        
        m_buf = 0;
        m_buflen = 0;
        
        m_buf = new (std::nothrow) BYTE[len];
        assert(m_buf);  //TODO
        
        m_buflen = len;
    }
    
    BYTE* dst_y = m_buf;
    BYTE* dst_v = dst_y + w*h;
    BYTE* dst_u = dst_v + ((w/2)*(h/2));
    
    const ULONG src_stride = 2*w;

    const ULONG dst_y_stride = w;
    const ULONG dst_uv_stride = w/2;
    
    const BYTE* src = srcbuf;
    
    for (ULONG i = 0; i < h; ++i)
    {
        const BYTE* src0_y = src;        
        src = src0_y + src_stride;

        BYTE* dst0_y = dst_y;
        dst_y = dst0_y + dst_y_stride;
        
        for (ULONG j = 0; j < dst_y_stride; ++j)
        {
            *dst0_y = *src0_y;
            
            src0_y += 2;            
            ++dst0_y;
        }
    }
    
    src = srcbuf;
    
    for (ULONG i = 0; i < h; i += 2)
    {
        const BYTE* src0_u = src + 1;
        const BYTE* src1_u = src0_u + src_stride;
        
        const BYTE* src0_v = src + 3;
        const BYTE* src1_v = src0_v + src_stride;
        
        src += 2 * src_stride;
        
        BYTE* dst0_u = dst_u;
        dst_u += dst_uv_stride;
        
        BYTE* dst0_v = dst_v;
        dst_v += dst_uv_stride;
        
        for (ULONG j = 0; j < dst_uv_stride; ++j)
        {
            const UINT u0 = *src0_u;
            const UINT u1 = *src1_u;
            const UINT src_u = (u0 + u1) / 2;  //?
            
            const UINT v0 = *src0_v;
            const UINT v1 = *src1_v;
            const UINT src_v = (v0 + v1) / 2;  //?

            *dst0_u = static_cast<BYTE>(src_u);
            *dst0_v = static_cast<BYTE>(src_v);
            
            src0_u += 4;
            src1_u += 4;
            
            src0_v += 4;
            src1_v += 4;
            
            ++dst0_u;
            ++dst0_v;
        }
    }
    
    return m_buf;
}


}  //end namespace VP8EncoderLib
