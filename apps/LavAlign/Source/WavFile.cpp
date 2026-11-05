#include "WavFile.h"

#include <cmath>
#include <cstring>

namespace lavalign
{
namespace
{
    uint16_t le16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
    uint32_t le32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
    uint64_t le64 (const uint8_t* p) { return (uint64_t) le32 (p) | ((uint64_t) le32 (p + 4) << 32); }

    void put16 (juce::MemoryOutputStream& o, uint16_t v) { o.writeShort ((short) v); }
    void put32 (juce::MemoryOutputStream& o, uint32_t v) { o.writeInt ((int) v); }
    void put64 (juce::MemoryOutputStream& o, uint64_t v) { o.writeInt64 ((juce::int64) v); }

    juce::AudioFormatManager& formats()
    {
        static juce::AudioFormatManager m;
        static bool init = [] { m.registerBasicFormats(); return true; }();
        juce::ignoreUnused (init);
        return m;
    }

    // Track names from a bext description: Sound Devices "sTRK1=Boom", Zoom "zTRK3=Tr1", etc.
    // Numbering is by recorder track slot, so when the numbers run past the channel count the names are
    // taken in order.
    juce::StringArray namesFromDescription (const juce::String& desc, int numChannels)
    {
        std::vector<std::pair<int, juce::String>> found;
        juce::StringArray lines;
        lines.addTokens (desc, "\r\n", {});
        for (auto line : lines)
        {
            line = line.trim();
            const int t = line.indexOf ("TRK");
            const int eq = line.indexOfChar ('=');
            if (t < 0 || eq < t + 4) continue;
            const auto num = line.substring (t + 3, eq);
            if (! num.containsOnly ("0123456789")) continue;
            found.emplace_back (num.getIntValue(), line.substring (eq + 1).trim());
        }
        juce::StringArray names;
        if (found.empty()) return names;
        std::sort (found.begin(), found.end(), [] (auto& a, auto& b) { return a.first < b.first; });
        int maxIdx = 0;
        for (auto& f : found) maxIdx = std::max (maxIdx, f.first);
        names.ensureStorageAllocated (numChannels);
        for (int i = 0; i < numChannels; ++i) names.add ({});
        if (maxIdx <= numChannels)
        {
            for (auto& f : found)
                if (f.first >= 1) names.set (f.first - 1, f.second);
        }
        else if ((int) found.size() == numChannels)
        {
            for (int i = 0; i < numChannels; ++i) names.set (i, found[(size_t) i].second);
        }
        else
            names.clear();
        return names;
    }

    void parseIxml (const juce::String& text, AudioFileInfo& info)
    {
        auto xml = juce::XmlDocument::parse (text);
        if (xml == nullptr) return;
        auto childText = [] (const juce::XmlElement* e, const char* tag) -> juce::String
        {
            if (e == nullptr) return {};
            if (auto* c = e->getChildByName (tag)) return c->getAllSubText().trim();
            return {};
        };
        if (info.scene.isEmpty()) info.scene = childText (xml.get(), "SCENE");
        if (info.take.isEmpty())  info.take  = childText (xml.get(), "TAKE");
        if (info.tape.isEmpty())  info.tape  = childText (xml.get(), "TAPE");
        if (auto* list = xml->getChildByName ("TRACK_LIST"))
        {
            juce::StringArray names;
            for (int i = 0; i < info.numChannels; ++i) names.add ({});
            bool any = false;
            for (auto* track : list->getChildIterator())
            {
                if (! track->hasTagName ("TRACK")) continue;
                auto idxText = childText (track, "INTERLEAVE_INDEX");
                if (idxText.isEmpty()) idxText = childText (track, "CHANNEL_INDEX");
                const int idx = idxText.getIntValue();
                const auto name = childText (track, "NAME");
                if (idx >= 1 && idx <= info.numChannels && name.isNotEmpty()) { names.set (idx - 1, name); any = true; }
            }
            if (any) info.trackNames = names;
        }
    }

    void encodeSample (float v, int bits, bool isFloat, uint8_t* dst)
    {
        if (isFloat)
        {
            if (bits == 64) { const double d = v; std::memcpy (dst, &d, 8); }
            else std::memcpy (dst, &v, 4);
            return;
        }
        const double full = std::ldexp (1.0, bits - 1);
        double s = std::round ((double) v * full);
        s = std::clamp (s, -full, full - 1.0);
        const auto i = (int64_t) s;
        switch (bits)
        {
            case 8:  dst[0] = (uint8_t) (i + 128); break;
            case 16: dst[0] = (uint8_t) i; dst[1] = (uint8_t) (i >> 8); break;
            case 24: dst[0] = (uint8_t) i; dst[1] = (uint8_t) (i >> 8); dst[2] = (uint8_t) (i >> 16); break;
            default: dst[0] = (uint8_t) i; dst[1] = (uint8_t) (i >> 8); dst[2] = (uint8_t) (i >> 16); dst[3] = (uint8_t) (i >> 24); break;
        }
    }

    float decodeSample (const uint8_t* p, int bits, bool isFloat)
    {
        if (isFloat)
        {
            if (bits == 64) { double d; std::memcpy (&d, p, 8); return (float) d; }
            float f; std::memcpy (&f, p, 4); return f;
        }
        switch (bits)
        {
            case 8:  return ((int) p[0] - 128) / 128.0f;
            case 16: return (int16_t) le16 (p) / 32768.0f;
            case 24: { int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24); return (float) (v / 2147483648.0); }
            default: return (float) ((int32_t) le32 (p) / 2147483648.0);
        }
    }

    juce::MemoryBlock makeFmt (int numChannels, double sampleRate, int bits, bool isFloat)
    {
        juce::MemoryOutputStream o;
        const int blockAlign = numChannels * bits / 8;
        put16 (o, isFloat ? 3 : 1);
        put16 (o, (uint16_t) numChannels);
        put32 (o, (uint32_t) std::lround (sampleRate));
        put32 (o, (uint32_t) (std::lround (sampleRate) * blockAlign));
        put16 (o, (uint16_t) blockAlign);
        put16 (o, (uint16_t) bits);
        if (isFloat) put16 (o, 0);
        return o.getMemoryBlock();
    }

    struct ChunkData { juce::String id; juce::MemoryBlock data; };

    // Writes a WAV (RIFF, or RF64 when it doesn't fit 4 GB) with the given chunks before the data; the data
    // is produced in blocks by 'fill' (returns bytes written into the buffer, 0 when done).
    juce::String writeWavFile (const juce::File& dest, const juce::MemoryBlock& fmt, const std::vector<ChunkData>& chunks,
                               int64_t dataSize, const std::function<juce::String (juce::OutputStream&)>& writeData)
    {
        int64_t total = 4 + 8 + (int64_t) fmt.getSize() + (int64_t) (fmt.getSize() & 1) + 8 + dataSize + (dataSize & 1);
        for (auto& c : chunks) total += 8 + (int64_t) c.data.getSize() + (int64_t) (c.data.getSize() & 1);
        const bool rf64 = total + 36 > (int64_t) 0xFFFFFFF0;
        if (rf64) total += 36;

        auto temp = dest.getSiblingFile (dest.getFileNameWithoutExtension() + ".partial" + dest.getFileExtension());
        temp.deleteFile();
        {
            juce::FileOutputStream out (temp);
            if (! out.openedOk()) return "Can't write " + temp.getFullPathName();

            juce::MemoryOutputStream h;
            h.write (rf64 ? "RF64" : "RIFF", 4);
            put32 (h, rf64 ? 0xFFFFFFFFu : (uint32_t) total);
            h.write ("WAVE", 4);
            if (rf64)
            {
                h.write ("ds64", 4); put32 (h, 28);
                put64 (h, (uint64_t) total); put64 (h, (uint64_t) dataSize);
                put64 (h, (uint64_t) 0);  // sample count (optional for PCM)
                put32 (h, 0);
            }
            h.write ("fmt ", 4); put32 (h, (uint32_t) fmt.getSize()); h.write (fmt.getData(), fmt.getSize());
            if (fmt.getSize() & 1) h.writeByte (0);
            for (auto& c : chunks)
            {
                h.write (c.id.toRawUTF8(), 4);
                put32 (h, (uint32_t) c.data.getSize());
                h.write (c.data.getData(), c.data.getSize());
                if (c.data.getSize() & 1) h.writeByte (0);
            }
            h.write ("data", 4);
            put32 (h, rf64 ? 0xFFFFFFFFu : (uint32_t) dataSize);
            out.write (h.getData(), h.getDataSize());

            const auto err = writeData (out);
            if (err.isNotEmpty()) { out.flush(); return err; }
            if (dataSize & 1) out.writeByte (0);
            out.flush();
            if (out.getStatus().failed()) return out.getStatus().getErrorMessage();
        }
        if (dest.existsAsFile() && ! dest.deleteFile()) { temp.deleteFile(); return "Can't replace " + dest.getFullPathName(); }
        if (! temp.moveFileTo (dest)) return "Can't rename to " + dest.getFullPathName();
        return {};
    }

    std::vector<ChunkData> readChunks (const AudioFileInfo& info, bool includeIxml)
    {
        std::vector<ChunkData> chunks;
        juce::FileInputStream in (info.file);
        if (! in.openedOk()) return chunks;
        for (auto& c : info.otherChunks)
        {
            if (! includeIxml && c.id == "iXML") continue;
            if (c.size > (int64_t) 256 * 1024 * 1024) continue;  // not metadata
            ChunkData d { c.id, {} };
            d.data.setSize ((size_t) c.size);
            in.setPosition (c.offset);
            in.read (d.data.getData(), (int) c.size);
            chunks.push_back (std::move (d));
        }
        return chunks;
    }
}

//==============================================================================
juce::String AudioFileInfo::timecodeString (double fps) const
{
    if (! timeReference || sampleRate <= 0) return {};
    const double secs = (double) *timeReference / sampleRate;
    const auto total = (int64_t) std::floor (secs);
    const int frames = (int) std::floor ((secs - (double) total) * fps + 1e-6);
    return juce::String::formatted ("%02d:%02d:%02d:%02d", (int) (total / 3600 % 24), (int) (total / 60 % 60), (int) (total % 60), frames);
}

juce::String readInfo (const juce::File& file, AudioFileInfo& info)
{
    info = {};
    info.file = file;
    juce::FileInputStream in (file);
    if (! in.openedOk()) return "Can't open " + file.getFileName();

    uint8_t hdr[12] {};
    const bool riff = in.read (hdr, 12) == 12
                      && (std::memcmp (hdr, "RIFF", 4) == 0 || std::memcmp (hdr, "RF64", 4) == 0 || std::memcmp (hdr, "BW64", 4) == 0)
                      && std::memcmp (hdr + 8, "WAVE", 4) == 0;
    if (! riff)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (file));
        if (reader == nullptr) return file.getFileName() + " is not an audio file LavAlign can read";
        info.sampleRate = reader->sampleRate;
        info.numChannels = (int) reader->numChannels;
        info.numFrames = reader->lengthInSamples;
        info.bitsPerSample = (int) reader->bitsPerSample;
        info.isFloat = reader->usesFloatingPointData;
        return {};
    }

    info.isWav = true;
    info.isRf64 = std::memcmp (hdr, "RIFF", 4) != 0;
    const int64_t fileSize = in.getTotalLength();
    int64_t ds64DataSize = -1;
    bool haveFmt = false, haveData = false;
    juce::String bextDescription;

    while (in.getPosition() + 8 <= fileSize)
    {
        uint8_t ch[8];
        if (in.read (ch, 8) != 8) break;
        const juce::String id (juce::CharPointer_ASCII (reinterpret_cast<const char*> (ch)), 4);
        int64_t size = le32 (ch + 4);
        const int64_t pos = in.getPosition();

        if (id == "ds64")
        {
            uint8_t d[24];
            if (in.read (d, 24) == 24) ds64DataSize = (int64_t) le64 (d + 8);
        }
        else if (id == "fmt ")
        {
            info.fmtChunk.setSize ((size_t) size);
            in.read (info.fmtChunk.getData(), (int) size);
            auto* f = static_cast<const uint8_t*> (info.fmtChunk.getData());
            if (size < 16) return "Broken fmt chunk";
            int tag = le16 (f);
            info.numChannels = le16 (f + 2);
            info.sampleRate = le32 (f + 4);
            info.blockAlign = le16 (f + 12);
            info.bitsPerSample = le16 (f + 14);
            if (tag == 0xFFFE && size >= 26) tag = le16 (f + 24);
            if (tag == 3) info.isFloat = true;
            else if (tag != 1) return file.getFileName() + ": unsupported WAV encoding (format " + juce::String (tag) + ")";
            haveFmt = true;
        }
        else if (id == "data")
        {
            if (size == 0xFFFFFFFF && ds64DataSize >= 0) size = ds64DataSize;
            if (info.isRf64 && ds64DataSize >= 0) size = ds64DataSize;
            size = std::min (size, fileSize - pos);  // an interrupted recording can claim more than is there
            info.dataOffset = pos;
            info.dataSize = size;
            haveData = true;
        }
        else
        {
            info.otherChunks.push_back ({ id, pos, std::min (size, fileSize - pos) });
            if (id == "bext" && size >= 346)
            {
                juce::MemoryBlock b ((size_t) std::min<int64_t> (size, 602));
                in.read (b.getData(), (int) b.getSize());
                auto* p = static_cast<const uint8_t*> (b.getData());
                bextDescription = juce::String (juce::CharPointer_UTF8 (reinterpret_cast<const char*> (p)), (size_t) strnlen (reinterpret_cast<const char*> (p), 256));
                info.timeReference = (int64_t) le64 (p + 338);
            }
            else if (id == "iXML" && size < 4 * 1024 * 1024)
            {
                juce::MemoryBlock b ((size_t) size);
                in.read (b.getData(), (int) size);
                parseIxml (juce::String::fromUTF8 (static_cast<const char*> (b.getData()), (int) strnlen (static_cast<const char*> (b.getData()), (size_t) size)), info);
            }
        }
        in.setPosition (pos + size + (size & 1));
    }

    if (! haveFmt || ! haveData) return file.getFileName() + ": not a complete WAV file";
    if (info.blockAlign <= 0 || info.numChannels <= 0) return file.getFileName() + ": broken WAV header";
    const int bytes = info.bitsPerSample / 8;
    if ((info.isFloat && bytes != 4 && bytes != 8) || (! info.isFloat && (bytes < 1 || bytes > 4)) || info.bitsPerSample % 8 != 0)
        return file.getFileName() + ": unsupported sample format (" + juce::String (info.bitsPerSample) + " bit)";
    if (info.blockAlign < info.numChannels * bytes) return file.getFileName() + ": broken WAV header";
    info.numFrames = info.dataSize / info.blockAlign;

    info.description = bextDescription;
    if (info.trackNames.isEmpty())
        info.trackNames = namesFromDescription (bextDescription, info.numChannels);
    // scene / take from the Zoom / Sound Devices description when there is no iXML
    juce::StringArray lines;
    lines.addTokens (bextDescription, "\r\n", {});
    for (auto& l : lines)
    {
        auto t = l.trim();
        auto value = t.fromFirstOccurrenceOf ("=", false, false).trim();
        if (info.scene.isEmpty() && t.matchesWildcard ("?SCENE=*", true)) info.scene = value;
        if (info.take.isEmpty()  && t.matchesWildcard ("?TAKE=*", true))  info.take = value;
        if (info.tape.isEmpty()  && t.matchesWildcard ("?TAPE=*", true))  info.tape = value;
    }
    return {};
}

juce::String readChannels (const AudioFileInfo& info, const std::vector<int>& channels,
                           std::vector<std::vector<float>>& out, const IoProgress& progress)
{
    out.assign (channels.size(), {});
    for (auto& v : out) v.assign ((size_t) info.numFrames, 0.0f);
    for (int c : channels)
        if (c < 0 || c >= info.numChannels) return "Channel out of range";

    if (! info.isWav)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (info.file));
        if (reader == nullptr) return "Can't read " + info.file.getFileName();
        const int block = 1 << 16;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, block);
        for (int64_t pos = 0; pos < info.numFrames; pos += block)
        {
            const int num = (int) std::min<int64_t> (block, info.numFrames - pos);
            reader->read (buf.getArrayOfWritePointers(), (int) reader->numChannels, pos, num);
            for (size_t i = 0; i < channels.size(); ++i)
                std::copy (buf.getReadPointer (channels[i]), buf.getReadPointer (channels[i]) + num, out[i].begin() + pos);
            if (progress && ! progress ((float) pos / (float) info.numFrames)) return "Cancelled";
        }
        return {};
    }

    juce::FileInputStream in (info.file);
    if (! in.openedOk()) return "Can't open " + info.file.getFileName();
    in.setPosition (info.dataOffset);
    const int bytes = info.bitsPerSample / 8;
    const int64_t framesPerBlock = std::max<int64_t> (1, (1 << 20) / info.blockAlign);
    std::vector<uint8_t> buf ((size_t) (framesPerBlock * info.blockAlign));
    for (int64_t pos = 0; pos < info.numFrames; pos += framesPerBlock)
    {
        const int64_t num = std::min (framesPerBlock, info.numFrames - pos);
        const int got = in.read (buf.data(), (int) (num * info.blockAlign));
        const int64_t frames = got / info.blockAlign;
        for (size_t c = 0; c < channels.size(); ++c)
        {
            const int off = channels[c] * bytes;
            float* dst = out[c].data() + pos;
            for (int64_t i = 0; i < frames; ++i)
                dst[i] = decodeSample (&buf[(size_t) (i * info.blockAlign + off)], info.bitsPerSample, info.isFloat);
        }
        if (frames < num) break;
        if (progress && ! progress ((float) pos / (float) info.numFrames)) return "Cancelled";
    }
    return {};
}

juce::String writeReplaced (const AudioFileInfo& source, const juce::File& dest,
                            const std::map<int, const float*>& replaced, const IoProgress& progress)
{
    if (dest == source.file) return "Refusing to overwrite the original file";

    if (! source.isWav)
    {
        // read every channel through JUCE and write a plain WAV in a matching format
        std::vector<int> all;
        for (int c = 0; c < source.numChannels; ++c) all.push_back (c);
        std::vector<std::vector<float>> data;
        auto err = readChannels (source, all, data);
        if (err.isNotEmpty()) return err;
        std::vector<const float*> ptrs;
        for (int c = 0; c < source.numChannels; ++c)
        {
            auto it = replaced.find (c);
            ptrs.push_back (it != replaced.end() ? it->second : data[(size_t) c].data());
        }
        const bool fl = source.isFloat || source.bitsPerSample > 24;
        return writeSimpleWav (dest, ptrs, source.numFrames, source.sampleRate, fl ? 32 : std::max (16, source.bitsPerSample), fl);
    }

    const auto chunks = readChunks (source, true);
    juce::FileInputStream in (source.file);
    if (! in.openedOk()) return "Can't open " + source.file.getFileName();
    const int bytes = source.bitsPerSample / 8;
    const int64_t dataSize = source.numFrames * source.blockAlign;

    return writeWavFile (dest, source.fmtChunk, chunks, dataSize, [&] (juce::OutputStream& out) -> juce::String
    {
        in.setPosition (source.dataOffset);
        const int64_t framesPerBlock = std::max<int64_t> (1, (1 << 20) / source.blockAlign);
        std::vector<uint8_t> buf ((size_t) (framesPerBlock * source.blockAlign));
        for (int64_t pos = 0; pos < source.numFrames; pos += framesPerBlock)
        {
            const int64_t num = std::min (framesPerBlock, source.numFrames - pos);
            const int want = (int) (num * source.blockAlign);
            const int got = in.read (buf.data(), want);
            if (got < want) std::fill (buf.begin() + std::max (0, got), buf.begin() + want, (uint8_t) 0);
            for (auto& [ch, samples] : replaced)
            {
                const int off = ch * bytes;
                for (int64_t i = 0; i < num; ++i)
                    encodeSample (samples[pos + i], source.bitsPerSample, source.isFloat, &buf[(size_t) (i * source.blockAlign + off)]);
            }
            if (! out.write (buf.data(), (size_t) want)) return "Write failed (disk full?)";
            if (progress && ! progress ((float) pos / (float) source.numFrames)) return "Cancelled";
        }
        return {};
    });
}

juce::String writeMono (const AudioFileInfo& source, const juce::File& dest, const float* samples, const IoProgress& progress)
{
    if (dest == source.file) return "Refusing to overwrite the original file";
    const bool fl = source.isFloat;
    const int bits = fl ? (source.bitsPerSample == 64 ? 64 : 32) : std::clamp (source.bitsPerSample, 16, 32);
    std::vector<ChunkData> chunks;
    if (source.isWav) chunks = readChunks (source, false);
    const auto fmt = makeFmt (1, source.sampleRate, bits, fl);
    const int bytes = bits / 8;
    const int64_t dataSize = source.numFrames * bytes;
    return writeWavFile (dest, fmt, chunks, dataSize, [&] (juce::OutputStream& out) -> juce::String
    {
        const int64_t block = 1 << 16;
        std::vector<uint8_t> buf ((size_t) (block * bytes));
        for (int64_t pos = 0; pos < source.numFrames; pos += block)
        {
            const int64_t num = std::min (block, source.numFrames - pos);
            for (int64_t i = 0; i < num; ++i) encodeSample (samples[pos + i], bits, fl, &buf[(size_t) (i * bytes)]);
            if (! out.write (buf.data(), (size_t) (num * bytes))) return "Write failed (disk full?)";
            if (progress && ! progress ((float) pos / (float) source.numFrames)) return "Cancelled";
        }
        return {};
    });
}

juce::String writeSimpleWav (const juce::File& dest, const std::vector<const float*>& channels, int64_t numFrames,
                             double sampleRate, int bits, bool isFloat, std::optional<int64_t> timeReference,
                             const juce::String& ixml)
{
    const int nch = (int) channels.size();
    const auto fmt = makeFmt (nch, sampleRate, bits, isFloat);
    std::vector<ChunkData> chunks;
    if (timeReference)
    {
        juce::MemoryBlock b (602, true);
        auto* p = static_cast<uint8_t*> (b.getData());
        const auto tr = (uint64_t) *timeReference;
        for (int i = 0; i < 8; ++i) p[338 + i] = (uint8_t) (tr >> (8 * i));
        p[346] = 1;  // version
        chunks.push_back ({ "bext", b });
    }
    if (ixml.isNotEmpty())
    {
        juce::MemoryBlock b (ixml.toRawUTF8(), ixml.getNumBytesAsUTF8());
        chunks.push_back ({ "iXML", b });
    }
    const int bytes = bits / 8;
    return writeWavFile (dest, fmt, chunks, numFrames * nch * bytes, [&] (juce::OutputStream& out) -> juce::String
    {
        const int64_t block = 1 << 14;
        std::vector<uint8_t> buf ((size_t) (block * nch * bytes));
        for (int64_t pos = 0; pos < numFrames; pos += block)
        {
            const int64_t num = std::min (block, numFrames - pos);
            for (int64_t i = 0; i < num; ++i)
                for (int c = 0; c < nch; ++c)
                    encodeSample (channels[(size_t) c][pos + i], bits, isFloat, &buf[(size_t) ((i * nch + c) * bytes)]);
            if (! out.write (buf.data(), (size_t) (num * nch * bytes))) return "Write failed";
        }
        return {};
    });
}
}
