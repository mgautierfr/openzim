/*
 * Copyright (C) 2008 Tommi Maekitalo
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

#include "zenocompressor.h"
#include <cxxtools/thread.h>
#include <tntdb/connect.h>
#include <cxxtools/log.h>
#include <zeno/deflatestream.h>
#include <zeno/bzip2stream.h>

log_define("zeno.compressor")

namespace zeno
{
  class ZenoCompressorThread : public cxxtools::AttachedThread
  {
      ZenoCompressorContext& zenoCompressor;

    public:
      explicit ZenoCompressorThread(ZenoCompressorContext& zenoCompressor_)
        : zenoCompressor(zenoCompressor_)
        {
          log_debug("create compressor thread");
          create();
        }

    protected:
      void run();
  };

  class ZenoCompressorContext
  {
      friend class ZenoCompressorThread;

      cxxtools::Mutex dbmutex;
      tntdb::Connection conn;
      tntdb::Statement insData;
      tntdb::Statement updArticle;

      cxxtools::Mutex mutex;
      cxxtools::Condition notEmpty;
      cxxtools::Condition notFull;

      struct CompressJobX : public CompressJob
      {
        CompressJobX() { }
        CompressJobX(const CompressJob& j, unsigned did_)
          : CompressJob(j),
            did(did_)
          { }

        unsigned did;
        std::string zdata;
      };

      std::deque<CompressJobX> queue;
      bool stop;

      unsigned datapos;
      unsigned nextDid; // next data id to assign
      unsigned insDid;  // next data id to insert
      typedef std::map<unsigned, CompressJobX> ReadyJobsMap;  // map did to job
      ReadyJobsMap readyJobs;  // map did to job

      static const unsigned maxQueueSize = 10;

      void threadFn();
      void doJob(CompressJobX& job);

    public:
      ZenoCompressorContext(const std::string& dburl, unsigned zid);
      void put(const CompressJob& job);
      void end();
  };

  void ZenoCompressorThread::run()
  {
    log_debug("compressor thread started");
    zenoCompressor.threadFn();
  }

  ZenoCompressor::ZenoCompressor(const std::string& dburl, unsigned zid, unsigned numThreads)
    : context(new ZenoCompressorContext(dburl, zid))
  {
    while (numThreads > compressorThreads.size())
      compressorThreads.push_back(new ZenoCompressorThread(*context));
  }

  ZenoCompressor::~ZenoCompressor()
  {
    context->end();
    for (unsigned n = 0; n < compressorThreads.size(); ++n)
    {
      log_debug("join thread " << n);
      compressorThreads[n]->join();
      log_debug("thread " << n << " joined");
    }
    log_debug("all threads joined - delete context object");
    delete context;
  }

  void ZenoCompressor::put(const CompressJob& job)
  {
    context->put(job);
  }

  ZenoCompressorContext::ZenoCompressorContext(const std::string& dburl, unsigned zid)
    : conn(tntdb::connect(dburl)),
      insData(conn.prepare(
        "insert into zenodata"
        "  (zid, did, data)"
        " values (:zid, :did, :data)")),
      updArticle(conn.prepare(
        "update zenoarticles"
        "   set datapos    = :datapos"
        " where zid = :zid"
        "   and aid = :aid")),
      stop(false),
      datapos(0),
      nextDid(0),
      insDid(0)
  {
    insData.set("zid", zid);
    updArticle.set("zid", zid);
  }

  void ZenoCompressorContext::end()
  {
    log_debug("end processing");
    stop = true;
    notFull.signal();
    notEmpty.signal();
  }

  void ZenoCompressorContext::put(const CompressJob& job)
  {
    cxxtools::MutexLock lock(mutex);

    while (!stop && queue.size() >= maxQueueSize)
      notFull.wait(lock);

    queue.push_back(CompressJobX(job, nextDid++));
    notEmpty.signal();
  }

  void ZenoCompressorContext::threadFn()
  {
    log_trace("threadFn");
    CompressJobX job;
    while (true)
    {
      {
        log_debug("wait for compress job");
        cxxtools::MutexLock lock(mutex);
        while (!stop && queue.empty())
        {
          log_debug("wait");
          notEmpty.wait(lock);
        }

        if (queue.empty())
        {
          // the stop flag is set, so we wake up the next thread and terminate
          log_debug("stop signal received - end thread");
          notEmpty.signal();
          log_debug("signaled not empty");
          return;
        }

        log_debug("got job");

        job = queue.front();
        queue.pop_front();

        notFull.signal();

        if (!queue.empty())
          notEmpty.signal();
      }

      doJob(job);

      log_debug("job ready");
    }
  }

  void ZenoCompressorContext::doJob(CompressJobX& job)
  {
    if (job.compression == zeno::Dirent::zenocompZip)
    {
      log_debug("zlib compress data " << job.data.size());

      std::ostringstream u;
      zeno::DeflateStream ds(u);
      ds << job.data << std::flush;
      ds.end();
      job.zdata = u.str();
      log_debug("after zlib compression " << job.zdata.size());
    }
    else if (job.compression == zeno::Dirent::zenocompBzip2)
    {
      log_debug("bzip2 compress data " << job.data.size());

      std::ostringstream u;
      zeno::Bzip2Stream ds(u);
      ds << job.data << std::flush;
      ds.end();
      job.zdata = u.str();
      log_debug("after bzip2 compression " << job.zdata.size());
    }
    else
    {
      job.zdata = job.data;
    }

    cxxtools::MutexLock lock(dbmutex);

    log_debug("insert datachunk " << job.did << " with " << job.zdata.size() << " bytes");
    tntdb::Blob bdata(job.zdata.data(), job.zdata.size());
    insData.set("did", job.did)
           .set("data", bdata)
           .execute();

    readyJobs[job.did] = job;

    ReadyJobsMap::iterator j;
    while ((j = readyJobs.find(insDid)) != readyJobs.end())
    {
      for (CompressJob::AidType::const_iterator it = j->second.aid.begin();
        it != j->second.aid.end(); ++it)
      {
        log_debug("update article " << *it << " datapos=" << datapos);
        updArticle.set("datapos", datapos)
                  .set("aid", *it)
                  .execute();
      }

      ++insDid;
      datapos += j->second.zdata.size();
    }
  }
}
