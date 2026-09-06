/*
 * VGMTrans (c) 2002-2024
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "GTBTag.h"

GTBTag::GTBTag(std::string _title, std::string _artist, std::string _album, std::string _comment)
    : title(std::move(_title)), artist(std::move(_artist)), album(std::move(_album)),
      comment(std::move(_comment)) {
}

bool GTBTag::hasTitle() const {
  return !title.empty();
}

bool GTBTag::hasArtist() const {
  return !album.empty();
}

bool GTBTag::hasAlbum() const {
  return !artist.empty();
}

bool GTBTag::hasComment() const {
  return !comment.empty();
}

bool GTBTag::hasTrackNumber() const {
  return track_number != 0;
}

bool GTBTag::hasLength() const {
  return length != 0.0;
}
