/*
 * Copyright (C) 2006 Tommi Maekitalo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * is provided AS IS, WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, and
 * NON-INFRINGEMENT.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include <zeno/fileimpl.h>
#include <zeno/error.h>
#include <zeno/article.h>
#include <zeno/dirent.h>
#include <zeno/qunicode.h>
#include <cxxtools/log.h>
#include <tnt/deflatestream.h>
#include <sstream>

log_define("zeno.file.impl")

namespace zeno
{
  //////////////////////////////////////////////////////////////////////
  // FileImpl
  //
  FileImpl::FileImpl(const char* fname)
    : zenoFile(fname)
  {
    if (!zenoFile)
      throw ZenoFileFormatError(std::string("can't open zeno-file \"") + fname + '"');

    filename = fname;

    const unsigned headerSize = 0x3c;
    char header[headerSize];
    if (!zenoFile.read(header, headerSize) || zenoFile.gcount() !=  headerSize)
      throw ZenoFileFormatError("format-error: header too short in zeno-file");

    size_type rCount = fromLittleEndian<size_type>(header + 0x8);
    offset_type rIndexPos = fromLittleEndian<offset_type>(header + 0x10);
    size_type rIndexLen = fromLittleEndian<size_type>(header + 0x18);
    offset_type rIndexPtrPos = fromLittleEndian<offset_type>(header + 0x20);
    size_type rIndexPtrLen = fromLittleEndian<size_type>(header + 0x28);

    log_debug("read " << rIndexPtrLen << " bytes");
    std::vector<size_type> buffer(rCount);
    zenoFile.seekg(rIndexPtrPos);
    zenoFile.read(reinterpret_cast<char*>(&buffer[0]), rIndexPtrLen);

    indexOffsets.reserve(rCount);
    for (std::vector<size_type>::const_iterator it = buffer.begin();
         it != buffer.end(); ++it)
      indexOffsets.push_back(static_cast<offset_type>(rIndexPos + fromLittleEndian<size_type>(&*it)));

    log_debug("read " << indexOffsets.size() << " index-entries ready");
  }

  Article FileImpl::getArticle(const QUnicodeString& url)
  {
    log_debug("get article \"" << url << '"');
    std::pair<bool, size_type> s = findArticle(url);
    if (!s.first)
    {
      log_warn("article \"" << url << "\" not found");
      return Article();
    }

    Dirent d = readDirentNolock(indexOffsets[s.second]);

    log_info("article \"" << url << "\" size " << d.getSize() << " mime-type " << d.getMimeType());

    return Article(s.second, d, File(this));
  }

  Article FileImpl::getArticle(const std::string& url)
  {
    return getArticle(QUnicodeString(url));
  }

  std::pair<bool, size_type> FileImpl::findArticle(const QUnicodeString& url)
  {
    log_debug("find article \"" << url << '"');

    cxxtools::MutexLock lock(mutex);

    IndexOffsetsType::size_type l = 0;
    IndexOffsetsType::size_type u = indexOffsets.size();
    IndexOffsetsType::size_type uu = u;

    unsigned itcount = 0;
    while (u - l > 1)
    {
      ++itcount;
      IndexOffsetsType::size_type p = l + (u - l) / 2;

      IndexOffsetsType::size_type pp = p;
      std::string title;
      while (p < u)
      {
        log_debug("l=" << l << " u=" << u << " p=" << p << " pp=" << pp);
        Dirent d = readDirentNolock(indexOffsets[p]);
        title = d.getTitle();
        if (!title.empty() || p == u)
          break;
        ++p;
      }

      if (p == u)
      {
        log_debug("u => " << pp << '*');
        u = pp;
      }
      else
      {
        int c = url.compare(QUnicodeString(title));
        log_debug("compare \"" << url << "\" with \"" << title << "\" => " << c);
        if (c < 0)
        {
          log_debug("u => " << p);
          u = uu = p;
        }
        else if (c > 0)
        {
          log_debug("l => " << p);
          l = p;
        }
        else
        {
          log_debug("article found after " << itcount << " iterations");
          return std::pair<bool, size_type>(true, p);
        }
      }
    }

    log_debug("article not found");
    return std::pair<bool, size_type>(false, uu);
  }

  Article FileImpl::getArticle(size_type idx)
  {
    log_debug("getArticle(" << idx << ')');
    cxxtools::MutexLock lock(mutex);
    Dirent d = readDirentNolock(indexOffsets[idx]);
    return Article(idx, d, File(this));
  }

  Dirent FileImpl::getDirent(size_type idx)
  {
    cxxtools::MutexLock lock(mutex);
    return readDirentNolock(indexOffsets[idx]);
  }

  std::string FileImpl::readData(offset_type off, size_type count)
  {
    cxxtools::MutexLock lock(mutex);
    return readDataNolock(off, count);
  }

  std::string FileImpl::readDataNolock(offset_type off, size_type count)
  {
    zenoFile.seekg(off);
    return readDataNolock(count);
  }

  std::string FileImpl::readDataNolock(size_type count)
  {
    std::string data;
    char buffer[256];
    while (count > 0)
    {
      zenoFile.read(buffer, std::min(static_cast<size_type>(sizeof(buffer)), count));
      if (!zenoFile)
        throw ZenoFileFormatError("format-error: error reading data");
      data.append(buffer, zenoFile.gcount());
      count -= zenoFile.gcount();
    }
    return data;
  }

  Dirent FileImpl::readDirentNolock(offset_type off)
  {
    log_debug("read directory entry at offset " << off);
    zenoFile.seekg(off);
    return readDirentNolock();
  }

  Dirent FileImpl::readDirentNolock()
  {
    char header[26];
    if (!zenoFile.read(header, 26) || zenoFile.gcount() != 26)
      throw ZenoFileFormatError("format-error: can't read index-header");

    Dirent dirent(header);

    std::string extra;
    if (dirent.getExtraLen() > 0)
      extra = readDataNolock(dirent.getExtraLen() - 1);

    if (zenoFile.get() != 0)
      throw ZenoFileFormatError("format-error: can't read index-header");

    dirent.setExtra(extra);
    log_debug("title=" << dirent.getTitle());

    return dirent;
  }

  void FileImpl::cacheData(offset_type off, size_type count)
  {
    log_debug("cacheData(" << off << ", " << count << ')');

    cxxtools::MutexLock lock(mutex);
    std::ostringstream data;
    tnt::DeflateStream zstream(data);
    zenoFile.seekg(off);

    char buffer[256];
    size_type c = count;
    while (c > 0)
    {
      zenoFile.read(buffer, std::min(static_cast<size_type>(sizeof(buffer)), c));
      if (!zenoFile)
        throw ZenoFileFormatError("format-error: error reading data");
      zstream.write(buffer, zenoFile.gcount());
      c -= zenoFile.gcount();
    }
    zstream.end();
    zcache = data.str();
    zcacheOffset = off;
    zcacheCount = count;

    log_debug(zcacheCount << " bytes in cache, " << zcache.size() << " compressed");
  }
}
