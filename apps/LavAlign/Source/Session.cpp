#include "Session.h"

#include <atomic>
#include <thread>

namespace lavalign
{
namespace
{
    bool isAudioFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;bwf;rf64;aif;aiff;flac;ogg;mp3")
               && ! f.getFileName().containsIgnoreCase (".partial.");
    }

    bool hasLetter (const juce::String& s)
    {
        for (auto c : s) if (juce::CharacterFunctions::isLetter (c)) return true;
        return false;
    }

    bool isMixName (const juce::String& n)
    {
        static const juce::StringArray mix { "l", "r", "lr", "mix", "mixl", "mixr", "mix l", "mix r", "mix-l", "mix-r",
                                             "mix_l", "mix_r", "trl", "trr", "ml", "mr", "left", "right", "stereo", "mono mix" };
        return mix.contains (n.trim().toLowerCase());
    }

    bool isBoomName (const juce::String& n)
    {
        const auto s = n.toLowerCase();
        for (auto* k : { "boom", "shotgun", "mkh", "cmit", "ntg", "schoeps", "416", "8060", "8050" })
            if (s.contains (k)) return true;
        return s == "bm" || s.matchesWildcard ("bm?", true);
    }

    bool isLavName (const juce::String& n)
    {
        const auto s = n.toLowerCase();
        for (auto* k : { "lav", "wireless", "radio", "dpa", "cos11", "cos-11", "b6", "4060", "4061", "6060", "4071" })
            if (s.contains (k)) return true;
        for (auto* k : { "wl", "rf", "tx", "lv" })
            if (s.startsWith (k)) return true;
        return false;
    }

    juce::String outputExtension (const AudioFileInfo& info)
    {
        return info.isWav ? info.file.getFileExtension() : juce::String (".wav");
    }
}

//==============================================================================
int Take::boomChannel() const
{
    for (size_t i = 0; i < channels.size(); ++i)
        if (channels[i].role == Role::boom) return (int) i;
    return -1;
}

std::vector<int> Take::lavChannels() const
{
    std::vector<int> v;
    for (size_t i = 0; i < channels.size(); ++i)
        if (channels[i].role == Role::lav) v.push_back ((int) i);
    return v;
}

juce::String Take::problem() const
{
    for (auto& f : files)
        if (std::abs (f.info.sampleRate - sampleRate) > 0.5)
            return "The files have different sample rates";
    if (lavChannels().empty()) return "Nothing to align (no lav)";
    if (boomChannel() < 0) return "No boom: mark one channel as the boom";
    return {};
}

juce::String Take::signature() const
{
    juce::String s;
    for (auto& f : files) s << f.info.file.getFullPathName() << "|" << juce::String (f.start) << ";";
    return s;
}

//==============================================================================
juce::String Session::keyFor (const juce::String& name, bool isFileName)
{
    if (! isFileName) return name.trim().toLowerCase();
    juce::StringArray tokens;
    tokens.addTokens (name, "-_ .", {});
    tokens.removeEmptyStrings();
    if (tokens.isEmpty()) return name.toLowerCase();
    if (hasLetter (tokens[0]) && tokens[0].length() <= 6) return tokens[0].toLowerCase();
    if (hasLetter (tokens[tokens.size() - 1])) return tokens[tokens.size() - 1].toLowerCase();
    return name.toLowerCase();
}

juce::StringArray Session::addFiles (const juce::Array<juce::File>& input)
{
    juce::StringArray messages;
    juce::Array<juce::File> list;
    for (auto& f : input)
    {
        if (f.isDirectory())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*", juce::File::findFiles))
            {
                const auto& file = entry.getFile();
                // skip earlier results
                if (file.getParentDirectory().getFileName() == "Aligned") continue;
                if (isAudioFile (file)) list.add (file);
            }
        }
        else if (isAudioFile (f))
            list.add (f);
        else if (f.existsAsFile())
            messages.add (f.getFileName() + ": not an audio file");
    }

    const int batch = nextBatch++;
    for (auto& f : list)
    {
        bool dup = false;
        for (auto& existing : files) if (existing.file == f) { dup = true; break; }
        if (dup) continue;
        AudioFileInfo info;
        const auto err = readInfo (f, info);
        if (err.isNotEmpty()) { messages.add (err); continue; }
        if (info.numFrames <= 0) { messages.add (f.getFileName() + ": empty"); continue; }
        files.push_back (info);
        batchOf.push_back (batch);
    }
    regroup();
    return messages;
}

void Session::removeTake (int index)
{
    if (index < 0 || index >= numTakes()) return;
    std::vector<AudioFileInfo> keep;
    std::vector<int> keepBatch;
    for (size_t i = 0; i < files.size(); ++i)
    {
        bool inTake = false;
        for (auto& tf : takes[(size_t) index]->files) if (tf.info.file == files[i].file) inTake = true;
        if (! inTake) { keep.push_back (files[i]); keepBatch.push_back (batchOf[i]); }
    }
    files = std::move (keep);
    batchOf = std::move (keepBatch);
    regroup();
}

void Session::clear()
{
    files.clear(); batchOf.clear(); takes.clear();
}

void Session::regroup()
{
    // union-find over files: timecode overlap (same sample rate), or same batch without timecode
    const size_t n = files.size();
    std::vector<size_t> parent (n);
    for (size_t i = 0; i < n; ++i) parent[i] = i;
    std::function<size_t (size_t)> find = [&] (size_t i) { return parent[i] == i ? i : parent[i] = find (parent[i]); };
    auto unite = [&] (size_t a, size_t b) { parent[find (a)] = find (b); };

    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            const auto& a = files[i]; const auto& b = files[j];
            if (std::abs (a.sampleRate - b.sampleRate) > 0.5) continue;
            if (a.timeReference && b.timeReference)
            {
                const double a0 = (double) *a.timeReference / a.sampleRate, a1 = a0 + a.durationSeconds();
                const double b0 = (double) *b.timeReference / b.sampleRate, b1 = b0 + b.durationSeconds();
                const double overlap = std::min (a1, b1) - std::max (a0, b0);
                if (overlap > 0.25 * std::min (a.durationSeconds(), b.durationSeconds()))
                    unite (i, j);
            }
            else if (! a.timeReference && ! b.timeReference && batchOf[i] == batchOf[j]
                     && a.numChannels == 1 && b.numChannels == 1)
                unite (i, j);
        }

    std::map<size_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < n; ++i) groups[find (i)].push_back (i);

    takes.clear();
    for (auto& [root, members] : groups)
    {
        auto t = std::make_unique<Take>();
        std::sort (members.begin(), members.end(), [&] (size_t a, size_t b)
                   { return files[a].file.getFileName().compareNatural (files[b].file.getFileName()) < 0; });
        t->sampleRate = files[members[0]].sampleRate;
        int64_t start = std::numeric_limits<int64_t>::max();
        for (auto m : members) start = std::min (start, files[m].timeReference.value_or (0));
        for (auto m : members)
        {
            TakeFile tf { files[m], files[m].timeReference.value_or (0) - start };
            t->length = std::max (t->length, tf.start + tf.info.numFrames);
            t->files.push_back (tf);
        }
        for (size_t fi = 0; fi < t->files.size(); ++fi)
        {
            const auto& info = t->files[fi].info;
            const auto base = info.file.getFileNameWithoutExtension();
            for (int c = 0; c < info.numChannels; ++c)
            {
                TakeChannel ch;
                ch.fileIndex = (int) fi;
                ch.channel = c;
                const auto track = c < info.trackNames.size() ? info.trackNames[c].trim() : juce::String();
                if (track.isNotEmpty()) { ch.name = track; ch.key = keyFor (track, false); }
                else if (info.numChannels > 1) { ch.name = "Ch " + juce::String (c + 1); ch.key = "ch " + juce::String (c + 1); }
                else { ch.name = base; ch.key = keyFor (base, true); }
                t->channels.push_back (ch);
            }
        }
        // name: scene/take from metadata, else the first file
        const auto& first = t->files.front().info;
        if (first.scene.isNotEmpty() || first.take.isNotEmpty())
            t->name = (first.scene.isNotEmpty() ? first.scene : juce::String ("?")) + " / " + (first.take.isNotEmpty() ? first.take : juce::String ("?"))
                      + "  (" + first.file.getFileNameWithoutExtension() + (t->files.size() > 1 ? " +" + juce::String ((int) t->files.size() - 1) : juce::String()) + ")";
        else
            t->name = first.file.getFileNameWithoutExtension() + (t->files.size() > 1 ? " +" + juce::String ((int) t->files.size() - 1) : juce::String());
        applyRoles (*t);
        takes.push_back (std::move (t));
    }
    std::sort (takes.begin(), takes.end(), [] (auto& a, auto& b)
    {
        const auto ta = a->files.front().info.timeReference.value_or (0), tb = b->files.front().info.timeReference.value_or (0);
        if (ta != tb) return ta < tb;
        return a->name.compareNatural (b->name) < 0;
    });
}

void Session::applyRoles (Take& t)
{
    // One boom track is the reference. Several files can carry it (AAF media: overlapping clips of the same
    // track), so every channel with the boom's key is a boom; a boom of another key becomes a lav.
    juce::String boomKey;
    std::vector<bool> fixed (t.channels.size(), false);
    for (size_t i = 0; i < t.channels.size(); ++i)
    {
        auto& ch = t.channels[i];
        auto it = userRoles.find (ch.key);
        if (it == userRoles.end()) continue;
        ch.role = it->second;
        fixed[i] = true;
        if (ch.role == Role::boom)
        {
            if (boomKey.isEmpty()) boomKey = ch.key;
            else if (ch.key != boomKey) ch.role = Role::lav;
        }
    }
    // guesses for the rest: mixes off, boom / lav by name, then the first remaining channel is the boom
    for (size_t i = 0; i < t.channels.size(); ++i)
    {
        if (fixed[i]) continue;
        auto& ch = t.channels[i];
        if (isMixName (ch.name)) ch.role = Role::off;
        else if (boomKey.isNotEmpty() && ch.key == boomKey) ch.role = Role::boom;
        else if (isBoomName (ch.name) && boomKey.isEmpty()) { ch.role = Role::boom; boomKey = ch.key; }
        else ch.role = Role::lav;
    }
    if (boomKey.isEmpty())
        for (size_t i = 0; i < t.channels.size(); ++i)
            if (! fixed[i] && t.channels[i].role == Role::lav && ! isLavName (t.channels[i].name))
            { boomKey = t.channels[i].key; break; }
    if (boomKey.isNotEmpty())
        for (size_t i = 0; i < t.channels.size(); ++i)
            if (! fixed[i] && t.channels[i].key == boomKey) t.channels[i].role = Role::boom;
}

void Session::setRole (int takeIndex, int channelIndex, Role role)
{
    if (takeIndex < 0 || takeIndex >= numTakes()) return;
    auto& t = *takes[(size_t) takeIndex];
    if (channelIndex < 0 || channelIndex >= (int) t.channels.size()) return;
    if (role == Role::boom)
        for (auto& ch : t.channels)
            if (ch.role == Role::boom && ch.key != t.channels[(size_t) channelIndex].key)
                userRoles[ch.key] = Role::lav;
    userRoles[t.channels[(size_t) channelIndex].key] = role;
    for (auto& tk : takes) applyRoles (*tk);
}

//==============================================================================
juce::String processTake (const Take& take, const Settings& settings, std::vector<LavOutcome>& outcomes,
                          const Progress& progress, bool keepAudio, bool measure, std::vector<float>* boomOut)
{
    outcomes.clear();
    const auto problem = take.problem();
    if (problem.isNotEmpty()) return problem;

    // The boom may be split over several files (AAF media clips of the boom track): each lav file is
    // compared with the boom file it overlaps most.
    std::vector<int> booms;
    for (size_t i = 0; i < take.channels.size(); ++i)
        if (take.channels[i].role == Role::boom) booms.push_back ((int) i);
    std::map<int, std::vector<float>> boomData;  // by channel index
    auto loadBoom = [&] (int ch) -> const std::vector<float>*
    {
        auto it = boomData.find (ch);
        if (it != boomData.end()) return &it->second;
        std::vector<std::vector<float>> d;
        const auto& c = take.channels[(size_t) ch];
        if (readChannels (take.files[(size_t) c.fileIndex].info, { c.channel }, d).isNotEmpty()) return nullptr;
        return &(boomData[ch] = std::move (d[0]));
    };
    auto overlap = [] (const TakeFile& a, const TakeFile& b)
    {
        return std::max<int64_t> (0, std::min (a.start + a.info.numFrames, b.start + b.info.numFrames) - std::max (a.start, b.start));
    };

    if (boomOut != nullptr)
    {
        boomOut->assign ((size_t) take.length, 0.0f);
        for (int b : booms)
        {
            const auto* data = loadBoom (b);
            if (data == nullptr) return "Can't read the boom";
            const auto start = (size_t) take.files[(size_t) take.channels[(size_t) b].fileIndex].start;
            for (size_t i = 0; i < data->size() && start + i < boomOut->size(); ++i)
                if ((*boomOut)[start + i] == 0.0f) (*boomOut)[start + i] = (*data)[i];
        }
    }

    // lav audio, per file
    struct Job { int channelIndex; const TakeFile* file; std::vector<float> lav; std::shared_ptr<std::vector<float>> boomView; };
    std::vector<Job> jobs;
    juce::String err;
    for (size_t fi = 0; fi < take.files.size(); ++fi)
    {
        std::vector<int> chans, idx;
        for (int c : take.lavChannels())
            if (take.channels[(size_t) c].fileIndex == (int) fi) { chans.push_back (take.channels[(size_t) c].channel); idx.push_back (c); }
        if (chans.empty()) continue;
        const auto& tf = take.files[fi];
        std::vector<std::vector<float>> data;
        err = readChannels (tf.info, chans, data);
        if (err.isNotEmpty()) return err;

        int bestBoom = booms.front();
        for (int b : booms)
            if (overlap (take.files[(size_t) take.channels[(size_t) b].fileIndex], tf)
                > overlap (take.files[(size_t) take.channels[(size_t) bestBoom].fileIndex], tf))
                bestBoom = b;
        const auto* boom = loadBoom (bestBoom);
        if (boom == nullptr) return "Can't read the boom";
        const auto& boomFile = take.files[(size_t) take.channels[(size_t) bestBoom].fileIndex];

        auto view = std::make_shared<std::vector<float>> ((size_t) tf.info.numFrames, 0.0f);
        const int64_t off = tf.start - boomFile.start;  // boom index of this file's sample 0
        for (int64_t i = 0; i < tf.info.numFrames; ++i)
        {
            const int64_t b = i + off;
            if (b >= 0 && b < (int64_t) boom->size()) (*view)[(size_t) i] = (*boom)[(size_t) b];
        }
        for (size_t k = 0; k < chans.size(); ++k)
            jobs.push_back ({ idx[k], &tf, std::move (data[k]), view });
    }
    boomData.clear();
    outcomes.resize (jobs.size());
    std::vector<std::atomic<float>> prog (jobs.size());
    for (auto& p : prog) p = 0.0f;
    std::atomic<bool> cancel { false };
    std::atomic<size_t> next { 0 };
    // lavs run side by side; each one's analysis gets a share of the cores
    Settings perJob = settings;
    if (perJob.threads <= 0)
        perJob.threads = std::max (1, (int) std::thread::hardware_concurrency() / std::max (1, (int) jobs.size()));

    auto worker = [&]
    {
        for (size_t j; (j = next++) < jobs.size();)
        {
            auto& job = jobs[j];
            auto& oc = outcomes[j];
            oc.channelIndex = job.channelIndex;
            const auto n = (int64_t) job.lav.size();
            alignLav (job.boomView->data(), job.lav.data(), n, take.sampleRate, perJob, oc.result,
                      [&] (float p) { prog[j] = p; return ! cancel.load(); }, measure);
            prog[j] = 1.0f;
            if (keepAudio) oc.original = std::move (job.lav);
            else { oc.result.aligned.clear(); oc.result.aligned.shrink_to_fit(); }
            job.lav.clear(); job.lav.shrink_to_fit();
        }
    };
    const size_t numThreads = std::max<size_t> (1, std::min<size_t> (jobs.size(), std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    for (size_t i = 1; i < numThreads; ++i) threads.emplace_back (worker);
    std::thread main (worker);

    bool done = false;
    while (! done)
    {
        done = true;
        float sum = 0;
        for (auto& p : prog) { sum += p; if (p < 1.0f) done = false; }
        if (progress && ! progress (sum / (float) jobs.size())) cancel = true;
        if (cancel || done) break;
        std::this_thread::sleep_for (std::chrono::milliseconds (30));
    }
    main.join();
    for (auto& t : threads) t.join();
    if (cancel) { outcomes.clear(); return "Cancelled"; }
    return {};
}

juce::Array<juce::File> exportTake (const Take& take, const std::vector<LavOutcome>& outcomes,
                                    const ExportOptions& options, juce::String& error, const IoProgress& progress)
{
    juce::Array<juce::File> written;
    error.clear();
    for (size_t fi = 0; fi < take.files.size(); ++fi)
    {
        const auto& info = take.files[fi].info;
        std::map<int, const float*> replaced;
        std::map<int, juce::String> names;
        for (auto& oc : outcomes)
        {
            const auto& ch = take.channels[(size_t) oc.channelIndex];
            if (ch.fileIndex != (int) fi || oc.result.aligned.empty()) continue;
            replaced[ch.channel] = oc.result.aligned.data();
            names[ch.channel] = ch.name;
        }
        if (replaced.empty()) continue;

        auto folder = options.folder != juce::File() ? options.folder : info.file.getParentDirectory().getChildFile ("Aligned");
        if (! folder.isDirectory() && ! folder.createDirectory()) { error = "Can't create " + folder.getFullPathName(); return written; }
        const auto base = info.file.getFileNameWithoutExtension();

        if (info.numChannels == 1 || ! options.splitPoly)
        {
            auto dest = folder.getChildFile (base + options.suffix + outputExtension (info));
            if (dest == info.file) { error = "The output would replace the original: choose another folder or a suffix"; return written; }
            error = writeReplaced (info, dest, replaced, progress);
            if (error.isNotEmpty()) return written;
            written.add (dest);
        }
        else
        {
            for (auto& [ch, data] : replaced)
            {
                auto dest = folder.getChildFile (juce::File::createLegalFileName (base + "_" + names[ch]) + options.suffix + ".wav");
                error = writeMono (info, dest, data, progress);
                if (error.isNotEmpty()) return written;
                written.add (dest);
            }
        }
    }
    return written;
}
}

namespace lavalign
{
void Session::setRoleForKey (const juce::String& key, Role role)
{
    userRoles[key] = role;
    for (auto& tk : takes) applyRoles (*tk);
}
}
