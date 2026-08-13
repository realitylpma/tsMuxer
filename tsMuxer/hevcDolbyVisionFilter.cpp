#include "hevcDolbyVisionFilter.h"

#include <fs/systemlog.h>

#include "hevc.h"
#include "nalUnits.h"
#include "vodCoreException.h"

// A four byte start code. The Matroska reader hands every NAL over in Annex-B form already
// (matroskaParser.cpp writeNalHeader), so the split re-emits the same framing on both sides.
static constexpr uint8_t START_CODE[4] = {0, 0, 0, 1};

HevcDolbyVisionFilter::HevcDolbyVisionFilter(const int demuxedPID)
    : SubTrackFilter(demuxedPID),
      m_firstDemuxCall(true),
      m_blStreamIndex(-1),
      m_elStreamIndex(-1),
      m_unwrapped(0),
      m_rpu(0),
      m_warnedNoWrappers(false)
{
}

void HevcDolbyVisionFilter::fillPids(const PIDSet& acceptedPIDs, const int pid)
{
    m_blStreamIndex = SubTrackFilter::pidToSubPid(pid, BL_SUB_TRACK);
    m_elStreamIndex = SubTrackFilter::pidToSubPid(pid, EL_SUB_TRACK);
    if (acceptedPIDs.find(m_blStreamIndex) == acceptedPIDs.end())
        m_blStreamIndex = -1;
    if (acceptedPIDs.find(m_elStreamIndex) == acceptedPIDs.end())
        m_elStreamIndex = -1;
}

void HevcDolbyVisionFilter::emit(const int streamIndex, const uint8_t* data, const uint8_t* dataEnd,
                                 DemuxedData& demuxedData, int64_t& discardSize) const
{
    if (streamIndex >= 0)
    {
        demuxedData[streamIndex].append(START_CODE, sizeof(START_CODE));
        demuxedData[streamIndex].append(data, dataEnd - data);
    }
    else
    {
        // Only one of the two layers was asked for. The other one is not an error, it is simply
        // not wanted, so its bytes are discarded rather than buffered forever.
        discardSize += dataEnd - data;
    }
}

int HevcDolbyVisionFilter::demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket)
{
    if (m_firstDemuxCall)
    {
        fillPids(acceptedPIDs, m_srcPID);
        m_firstDemuxCall = false;
    }

    uint8_t* dataEnd = avPacket.data + avPacket.size;
    uint8_t* curNal = NALUnit::findNextNAL(avPacket.data, dataEnd);
    int64_t discardSize = 0;

    while (curNal < dataEnd)
    {
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curNal, dataEnd, true);
        uint8_t* nalEnd = nextNal;
        if (nextNal < dataEnd)
        {
            // Back off the trailing zeroes belonging to the next start code.
            while (nalEnd > curNal && nalEnd[-1] == 0) nalEnd--;
        }

        if (nalEnd - curNal >= 2)
        {
            const int nalType = (curNal[0] >> 1) & 0x3F;
            if (nalType == static_cast<int>(HevcUnit::NalType::DVEL))
            {
                // An enhancement layer NAL inside its wrapper. Drop the two wrapper bytes and what
                // is left IS the NAL as the disc carried it, header included. Nothing is unescaped:
                // the NAL was emulation prevented before it was ever wrapped.
                emit(m_elStreamIndex, curNal + 2, nalEnd, demuxedData, discardSize);
                m_unwrapped++;
            }
            else if (nalType == static_cast<int>(HevcUnit::NalType::DVRPU))
            {
                emit(m_elStreamIndex, curNal, nalEnd, demuxedData, discardSize);
                m_rpu++;
            }
            else
            {
                emit(m_blStreamIndex, curNal, nalEnd, demuxedData, discardSize);
            }
        }

        curNal = NALUnit::findNextNAL(nextNal, dataEnd);
    }

    // A track with an RPU but no wrappers is a SINGLE layer Dolby Vision file, profile 5 or 8. It
    // has no enhancement layer to separate out, so splitting it produces an empty second track and
    // a base layer that has lost its RPU. Say so once, loudly, rather than writing a broken disc.
    if (!m_warnedNoWrappers && m_rpu > 32 && m_unwrapped == 0)
    {
        m_warnedNoWrappers = true;
        LTRACE(LT_WARN, 2,
               "Dolby Vision: this track carries an RPU but no enhancement layer, so it is single "
               "layer and cannot be split into two. Mux it as ONE track instead of using subTrack.");
    }

    return avPacket.size - static_cast<int>(discardSize);
}
