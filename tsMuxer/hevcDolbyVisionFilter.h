#ifndef HEVC_DOLBY_VISION_FILTER_H_
#define HEVC_DOLBY_VISION_FILTER_H_

#include "abstractDemuxer.h"
#include "subTrackFilter.h"

// Split one merged Dolby Vision video track back into its two layers.
//
// A Matroska file carries a dual layer title as ONE track: the base layer NALs, then the
// enhancement layer's NALs each wrapped in an unspecified NAL of type 63, then the RPU. A disc
// wants them apart again, base layer on one PID and enhancement layer on another. This is the
// exact inverse of what MatroskaMuxer does on the way in, and it is a pure byte transform: strip
// the two wrapper bytes and route.
//
// It is used exactly like the MVC filter, through subTrack= on the meta line, so one file and one
// track can feed two tsMuxeR streams:
//
//     V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=1     the base layer
//     V_MPEGH/ISO/HEVC, "film.mkv", track=1, subTrack=2     the enhancement layer
//
// THE CLASSIFIER MUST BE EXACT. If a single type 62 or 63 NAL leaks onto the base layer side, that
// reader reports itself as carrying Dolby Vision, the muxer sends BOTH tracks to the enhancement
// layer PID, and the disc is wrong. So the rule is stated positively: types 62 and 63 go to the
// enhancement layer, everything else goes to the base layer, and nothing is ever sent to both.
class HevcDolbyVisionFilter final : public SubTrackFilter
{
   public:
    explicit HevcDolbyVisionFilter(int demuxedPID);
    ~HevcDolbyVisionFilter() override = default;

    int demuxPacket(DemuxedData& demuxedData, const PIDSet& acceptedPIDs, AVPacket& avPacket) override;

    // Sub-track numbers as they appear in subTrack= on the meta line.
    static constexpr int BL_SUB_TRACK = 1;
    static constexpr int EL_SUB_TRACK = 2;

   private:
    void fillPids(const PIDSet& acceptedPIDs, int pid);
    void emit(int streamIndex, const uint8_t* data, const uint8_t* dataEnd, DemuxedData& demuxedData,
              int64_t& discardSize) const;

    bool m_firstDemuxCall;
    int m_blStreamIndex;
    int m_elStreamIndex;
    int64_t m_unwrapped;  // enhancement NALs whose type 63 wrapper was removed
    int64_t m_rpu;        // RPU NALs passed through unchanged
    bool m_warnedNoWrappers;
};

#endif  // HEVC_DOLBY_VISION_FILTER_H_
