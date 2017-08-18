/*
*      Copyright (C) 2016-2016 peak3d
*      http://www.peak3d.de
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  <http://www.gnu.org/licenses/>.
*
*/

#include "AdaptiveTree.h"
#include <string.h>
#include <algorithm>
#include <stdlib.h>

namespace adaptive
{
  void AdaptiveTree::Segment::SetRange(const char *range)
  {
    const char *delim(strchr(range, '-'));
    if (delim)
    {
      range_begin_ = strtoull(range, 0, 10);
      range_end_ = strtoull(delim + 1, 0, 10);
    }
    else
      range_begin_ = range_end_ = 0;
  }

  AdaptiveTree::AdaptiveTree()
    : current_period_(0)
    , parser_(0)
    , currentNode_(0)
    , segcount_(0)
    , overallSeconds_(0)
    , stream_start_(0)
    , available_time_(0)
    , publish_time_(0)
    , base_time_(0)
    , minPresentationOffset(0)
    , has_timeshift_buffer_(false)
    , download_speed_(0.0)
    , average_download_speed_(0.0f)
    , encryptionState_(ENCRYTIONSTATE_UNENCRYPTED)
    , included_types_(0)
  {
    psshSets_.push_back(PSSH());
  }

  AdaptiveTree::~AdaptiveTree()
  {
    for (std::vector<Period*>::const_iterator bp(periods_.begin()), ep(periods_.end()); bp != ep; ++bp)
      for (std::vector<AdaptationSet*>::const_iterator ba((*bp)->adaptationSets_.begin()), ea((*bp)->adaptationSets_.end()); ba != ea; ++ba)
        for (std::vector<Representation*>::const_iterator br((*ba)->repesentations_.begin()), er((*ba)->repesentations_.end()); br != er; ++br)
          if ((*br)->flags_ & Representation::URLSEGMENTS)
          {
            for (std::vector<Segment>::iterator bs((*br)->segments_.data.begin()), es((*br)->segments_.data.end()); bs != es; ++bs)
              delete[] bs->url;
            for (std::vector<Segment>::iterator bs((*br)->newSegments_.data.begin()), es((*br)->newSegments_.data.end()); bs != es; ++bs)
              delete[] bs->url;
          }
  }

  bool AdaptiveTree::has_type(StreamType t)
  {
    if (periods_.empty())
      return false;

    for (std::vector<AdaptationSet*>::const_iterator b(periods_[0]->adaptationSets_.begin()), e(periods_[0]->adaptationSets_.end()); b != e; ++b)
      if ((*b)->type_ == t)
        return true;
    return false;
  }

  uint32_t AdaptiveTree::estimate_segcount(uint32_t duration, uint32_t timescale)
  {
    duration /= timescale;
    return static_cast<uint32_t>((overallSeconds_ / duration)*1.01);
  }

  void AdaptiveTree::set_download_speed(double speed)
  {
    std::lock_guard<std::mutex> lck(m_mutex);

    download_speed_ = speed;
    if (!average_download_speed_)
      average_download_speed_ = download_speed_;
    else
      average_download_speed_ = average_download_speed_*0.9 + download_speed_*0.1;
  };

  void AdaptiveTree::SetFragmentDuration(const AdaptationSet* adp, const Representation* rep, size_t pos, uint64_t timestamp, uint32_t fragmentDuration, uint32_t movie_timescale)
  {
    if (!has_timeshift_buffer_)
      return;

    //Get a modifiable adaptationset
    AdaptationSet *adpm(const_cast<AdaptationSet *>(adp));

    // Check if its the last frame we watch
    if (adp->segment_durations_.data.size())
    {
      if (pos == adp->segment_durations_.data.size() - 1)
      {
        adpm->segment_durations_.insert(static_cast<std::uint64_t>(fragmentDuration)*adp->timescale_ / movie_timescale);
      }
      else
      {
        ++const_cast<Representation*>(rep)->expired_segments_;
        return;
      }
    }
    else if (pos != rep->segments_.data.size() - 1)
      return;

    Segment seg(*(rep->segments_[pos]));

    if (!timestamp)
      fragmentDuration = static_cast<std::uint32_t>((static_cast<std::uint64_t>(fragmentDuration)*rep->timescale_) / movie_timescale);
    else
      fragmentDuration = static_cast<uint32_t>(timestamp - base_time_ - seg.startPTS_);

    seg.startPTS_ += fragmentDuration;
    seg.range_begin_ += fragmentDuration;
    seg.range_end_ ++;

    for (std::vector<Representation*>::iterator b(adpm->repesentations_.begin()), e(adpm->repesentations_.end()); b != e; ++b)
      (*b)->segments_.insert(seg);
  }

  void AdaptiveTree::OnDataArrived(Representation *rep, const Segment *seg, const uint8_t *src, uint8_t *dst, size_t dstOffset, size_t dataSize)
  { 
    memcpy(dst + dstOffset, src, dataSize);
  }

  uint8_t AdaptiveTree::insert_psshset(StreamType type)
  {
    if (!current_pssh_.empty())
    {
      PSSH pssh;
      pssh.pssh_ = current_pssh_;
      pssh.defaultKID_ = current_defaultKID_;
      pssh.iv = current_iv_;
      switch (type)
      {
      case VIDEO: pssh.media_ = PSSH::MEDIA_VIDEO; break;
      case AUDIO: pssh.media_ = PSSH::MEDIA_AUDIO; break;
      case STREAM_TYPE_COUNT: pssh.media_ = PSSH::MEDIA_VIDEO | PSSH::MEDIA_AUDIO; break;
      default: pssh.media_ = 0; break;
      }

      std::vector<PSSH>::iterator pos(std::find(psshSets_.begin() + 1, psshSets_.end(), pssh));
      if (pos == psshSets_.end())
        pos = psshSets_.insert(psshSets_.end(), pssh);
      else
        pos->media_ |= pssh.media_;
      return static_cast<uint8_t>(pos - psshSets_.begin());
    }
    return 0;
  }

  void AdaptiveTree::SortTree()
  {
    for (std::vector<Period*>::const_iterator bp(periods_.begin()), ep(periods_.end()); bp != ep; ++bp)
    {
      std::sort((*bp)->adaptationSets_.begin(), (*bp)->adaptationSets_.end(), AdaptationSet::compare);
      for (std::vector<AdaptationSet*>::const_iterator ba((*bp)->adaptationSets_.begin()), ea((*bp)->adaptationSets_.end()); ba != ea; ++ba)
      {
        std::sort((*ba)->repesentations_.begin(), (*ba)->repesentations_.end(), Representation::compare);
        for (std::vector<Representation*>::iterator br((*ba)->repesentations_.begin()), er((*ba)->repesentations_.end()); br != er; ++br)
          (*br)->SetScaling();
      }
    }
  }

} // namespace
