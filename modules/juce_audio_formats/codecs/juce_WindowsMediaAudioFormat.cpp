/*
  ==============================================================================

   This file is part of the JUCE library - "Jules' Utility Class Extensions"
   Copyright 2004-11 by Raw Material Software Ltd.

  ------------------------------------------------------------------------------

   JUCE can be redistributed and/or modified under the terms of the GNU General
   Public License (Version 2), as published by the Free Software Foundation.
   A copy of the license is included in the JUCE distribution, or can be found
   online at www.gnu.org/licenses.

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.rawmaterialsoftware.com/juce for more information.

  ==============================================================================
*/

#if JUCE_WINDOWS

namespace WindowsMediaCodec
{

class JuceIStream   : public ComBaseClassHelper <IStream>
{
public:
    JuceIStream (InputStream& source_) noexcept
        : source (source_)
    {
        resetReferenceCount();
    }

    JUCE_COMRESULT Commit (DWORD)                        { return S_OK; }
    JUCE_COMRESULT Write (void const*, ULONG, ULONG*)    { return E_NOTIMPL; }
    JUCE_COMRESULT Clone (IStream**)                     { return E_NOTIMPL; }
    JUCE_COMRESULT SetSize (ULARGE_INTEGER)              { return E_NOTIMPL; }
    JUCE_COMRESULT Revert()                              { return E_NOTIMPL; }
    JUCE_COMRESULT LockRegion (ULARGE_INTEGER, ULARGE_INTEGER, DWORD)    { return E_NOTIMPL; }
    JUCE_COMRESULT UnlockRegion (ULARGE_INTEGER, ULARGE_INTEGER, DWORD)  { return E_NOTIMPL; }

    JUCE_COMRESULT Read (void* dest, ULONG numBytes, ULONG* bytesRead)
    {
        const int numRead = source.read (dest, numBytes);

        if (bytesRead != nullptr)
            *bytesRead = numRead;

        return numRead == (int) numBytes ? S_OK : S_FALSE;
    }

    JUCE_COMRESULT Seek (LARGE_INTEGER position, DWORD origin, ULARGE_INTEGER* resultPosition)
    {
        int64 newPos = (int64) position.QuadPart;

        if (origin == STREAM_SEEK_CUR)
        {
            newPos += source.getPosition();
        }
        else if (origin == STREAM_SEEK_END)
        {
            const int64 len = source.getTotalLength();
            if (len < 0)
                return E_NOTIMPL;

            newPos += len;
        }

        if (resultPosition != nullptr)
            resultPosition->QuadPart = newPos;

        return source.setPosition (newPos) ? S_OK : E_NOTIMPL;
    }

    JUCE_COMRESULT CopyTo (IStream* destStream, ULARGE_INTEGER numBytesToDo,
                           ULARGE_INTEGER* bytesRead, ULARGE_INTEGER* bytesWritten)
    {
        uint64 totalCopied = 0;
        int64 numBytes = numBytesToDo.QuadPart;

        while (numBytes > 0 && ! source.isExhausted())
        {
            char buffer [1024];

            const int numToCopy = (int) jmin ((int64) sizeof (buffer), (int64) numBytes);
            const int numRead = source.read (buffer, numToCopy);

            if (numRead <= 0)
                break;

            destStream->Write (buffer, numRead, nullptr);
            totalCopied += numRead;
        }

        if (bytesRead != nullptr)      bytesRead->QuadPart = totalCopied;
        if (bytesWritten != nullptr)   bytesWritten->QuadPart = totalCopied;

        return S_OK;
    }

    JUCE_COMRESULT Stat (STATSTG* stat, DWORD)
    {
        if (stat == nullptr)
            return STG_E_INVALIDPOINTER;

        zerostruct (*stat);
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = jmax ((int64) 0, source.getTotalLength());
        return S_OK;
    }


private:
    InputStream& source;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceIStream);
};

//==============================================================================
static const char* wmFormatName = "Windows Media";
static const char* const extensions[] = { ".mp3", ".wmv", ".asf", 0 };

//==============================================================================
class WMAudioReader   : public AudioFormatReader
{
public:
    WMAudioReader (InputStream* const input_)
        : AudioFormatReader (input_, TRANS (wmFormatName)),
          ok (false),
          wmvCoreLib ("Wmvcore.dll"),
          currentPosition (0),
          bufferStart (0), bufferEnd (0)
    {
        typedef HRESULT (*WMCreateSyncReaderType) (IUnknown*, DWORD, IWMSyncReader**);
        WMCreateSyncReaderType wmCreateSyncReader = nullptr;
        wmCreateSyncReader = (WMCreateSyncReaderType) wmvCoreLib.getFunction ("WMCreateSyncReader");

        if (wmCreateSyncReader != nullptr)
        {
            HRESULT hr = wmCreateSyncReader (nullptr, WMT_RIGHT_PLAYBACK, wmSyncReader.resetAndGetPointerAddress());
            hr = wmSyncReader->OpenStream (new JuceIStream (*input));
            hr = wmSyncReader->SetReadStreamSamples (0, false);

            scanFileForDetails();
            ok = sampleRate > 0;
        }
    }

    ~WMAudioReader()
    {
        if (wmSyncReader != nullptr)
        {
            wmSyncReader->Close();
            wmSyncReader = nullptr;
        }
    }

    bool readSamples (int** destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      int64 startSampleInFile, int numSamples)
    {
        if (! ok)
            return false;

        if (startSampleInFile != currentPosition)
        {
            currentPosition = startSampleInFile;
            wmSyncReader->SetRange (((QWORD) startSampleInFile * 10000000) / sampleRate, 0);
            bufferStart = bufferEnd = 0;
        }

        while (numSamples > 0)
        {
            if (bufferEnd <= bufferStart)
            {
                INSSBuffer* sampleBuffer = nullptr;
                QWORD sampleTime, duration;
                DWORD flags, outputNum;
                WORD streamNum;

                HRESULT hr = wmSyncReader->GetNextSample (0, &sampleBuffer, &sampleTime,
                                                          &duration, &flags, &outputNum, &streamNum);

                if (SUCCEEDED (hr))
                {
                    BYTE* rawData = nullptr;
                    DWORD dataLength = 0;
                    hr = sampleBuffer->GetBufferAndLength (&rawData, &dataLength);
                    jassert (SUCCEEDED (hr));

                    bufferStart = 0;
                    bufferEnd = (int) dataLength;

                    if (bufferEnd <= 0)
                        return false;

                    buffer.ensureSize (bufferEnd);
                    memcpy (buffer.getData(), rawData, bufferEnd);
                }
                else
                {
                    bufferStart = 0;
                    bufferEnd = 512;
                    buffer.ensureSize (bufferEnd);
                    buffer.fillWith (0);
                }
            }

            const int stride = numChannels * sizeof (int16);
            const int16* const rawData = static_cast <const int16*> (addBytesToPointer (buffer.getData(), bufferStart));
            const int numToDo = jmin (numSamples, (bufferEnd - bufferStart) / stride);

            for (int i = 0; i < numDestChannels; ++i)
            {
                jassert (destSamples[i] != nullptr);

                const int srcChan = jmin (i, (int) numChannels - 1);
                const int16* src = rawData + srcChan;
                int* const dst = destSamples[i] + startOffsetInDestBuffer;

                for (int j = 0; j < numToDo; ++j)
                {
                    dst[j] = ((uint32) *src) << 16;
                    src += numChannels;
                }
            }

            bufferStart += numToDo * stride;
            startOffsetInDestBuffer += numToDo;
            numSamples -= numToDo;
            currentPosition += numToDo;
        }

        return true;
    }

    bool ok;

private:
    DynamicLibrary wmvCoreLib;
    ComSmartPtr<IWMSyncReader> wmSyncReader;
    int64 currentPosition;
    MemoryBlock buffer;
    int bufferStart, bufferEnd;

    void scanFileForDetails()
    {
        ComSmartPtr<IWMHeaderInfo> wmHeaderInfo;
        HRESULT hr = wmSyncReader.QueryInterface (wmHeaderInfo);

        QWORD lengthInNanoseconds = 0;
        WORD lengthOfLength = sizeof (lengthInNanoseconds);
        WORD wmStreamNum = 0;
        WMT_ATTR_DATATYPE wmAttrDataType;
        hr = wmHeaderInfo->GetAttributeByName (&wmStreamNum, L"Duration", &wmAttrDataType,
                                               (BYTE*) &lengthInNanoseconds, &lengthOfLength);

        ComSmartPtr<IWMStreamConfig> wmStreamConfig;
        ComSmartPtr<IWMProfile> wmProfile;
        hr = wmSyncReader.QueryInterface (wmProfile);
        hr = wmProfile->GetStream (0, wmStreamConfig.resetAndGetPointerAddress());

        ComSmartPtr<IWMMediaProps> wmMediaProperties;
        hr = wmStreamConfig.QueryInterface (wmMediaProperties);

        DWORD sizeMediaType;
        hr = wmMediaProperties->GetMediaType (0, &sizeMediaType);

        HeapBlock<WM_MEDIA_TYPE> mediaType;
        mediaType.malloc (sizeMediaType, 1);
        hr = wmMediaProperties->GetMediaType (mediaType, &sizeMediaType);

        if (mediaType->majortype == WMMEDIATYPE_Audio)
        {
            const WAVEFORMATEX* const inputFormat = reinterpret_cast<WAVEFORMATEX*> (mediaType->pbFormat);

            sampleRate = inputFormat->nSamplesPerSec;
            numChannels = inputFormat->nChannels;
            bitsPerSample = inputFormat->wBitsPerSample;
            lengthInSamples = (lengthInNanoseconds * sampleRate) / 10000000;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WMAudioReader);
};

}

//==============================================================================
WindowsMediaAudioFormat::WindowsMediaAudioFormat()
    : AudioFormat (TRANS (WindowsMediaCodec::wmFormatName), StringArray (WindowsMediaCodec::extensions))
{
}

WindowsMediaAudioFormat::~WindowsMediaAudioFormat() {}

Array<int> WindowsMediaAudioFormat::getPossibleSampleRates()    { return Array<int>(); }
Array<int> WindowsMediaAudioFormat::getPossibleBitDepths()      { return Array<int>(); }

bool WindowsMediaAudioFormat::canDoStereo()     { return true; }
bool WindowsMediaAudioFormat::canDoMono()       { return true; }

//==============================================================================
AudioFormatReader* WindowsMediaAudioFormat::createReaderFor (InputStream* sourceStream, bool deleteStreamIfOpeningFails)
{
    ScopedPointer<WindowsMediaCodec::WMAudioReader> r (new WindowsMediaCodec::WMAudioReader (sourceStream));

    if (r->ok)
        return r.release();

    if (! deleteStreamIfOpeningFails)
        r->input = nullptr;

    return nullptr;
}

AudioFormatWriter* WindowsMediaAudioFormat::createWriterFor (OutputStream* /*streamToWriteTo*/, double /*sampleRateToUse*/,
                                                             unsigned int /*numberOfChannels*/, int /*bitsPerSample*/,
                                                             const StringPairArray& /*metadataValues*/, int /*qualityOptionIndex*/)
{
    jassertfalse; // not yet implemented!
    return nullptr;
}

#endif
