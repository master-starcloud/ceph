// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab


#ifndef CEPH_OSDMAPMAPPING_H
#define CEPH_OSDMAPMAPPING_H

#include <vector>
#include <map>

#include "osd/osd_types.h"
#include "common/WorkQueue.h"

class OSDMap;

/// work queue to perform work on batches of pgids on multiple CPUs
class ParallelPGMapper {
public:
  struct Job {
    utime_t start, finish;
    unsigned shards = 0;
    const OSDMap *osdmap;
    bool aborted = false;
    Context *onfinish = nullptr;

    Mutex lock = {"ParallelPGMapper::Job::lock"};
    Cond cond;

    Job(const OSDMap *om) : start(ceph_clock_now()), osdmap(om) {}
    virtual ~Job() {
      assert(shards == 0);
    }

    // child must implement either form of process
    virtual void process(const vector<pg_t>& pgs) = 0;
    virtual void process(int64_t poolid, unsigned ps_begin, unsigned ps_end) = 0;
    virtual void complete() = 0;

    void set_finish_event(Context *fin) {
      lock.Lock();
      if (shards == 0) {
	// already done.
	lock.Unlock();
	fin->complete(0);
      } else {
	// set finisher
	onfinish = fin;
	lock.Unlock();
      }
    }
    bool is_done() {
      Mutex::Locker l(lock);
      return shards == 0;
    }
    utime_t get_duration() {
      return finish - start;
    }
    void wait() {
      Mutex::Locker l(lock);
      while (shards > 0) {
	cond.Wait(lock);
      }
    }
    bool wait_for(double duration) {
      utime_t until = start;
      until += duration;
      Mutex::Locker l(lock);
      while (shards > 0) {
	if (ceph_clock_now() >= until) {
	  return false;
	}
	cond.Wait(lock);
      }
      return true;
    }
    void abort() {
      Context *fin = nullptr;
      {
	Mutex::Locker l(lock);
	aborted = true;
	fin = onfinish;
	onfinish = nullptr;
	while (shards > 0) {
	  cond.Wait(lock);
	}
      }
      if (fin) {
	fin->complete(-ECANCELED);
      }
    }

    void start_one() {
      Mutex::Locker l(lock);
      ++shards;
    }
    void finish_one();
  };

protected:
  CephContext *cct;

  struct Item {
    Job *job;
    int64_t pool;
    unsigned begin, end;
    vector<pg_t> pgs;

    Item(Job *j, vector<pg_t> pgs) : job(j), pgs(pgs) {}
    Item(Job *j, int64_t p, unsigned b, unsigned e)
      : job(j),
	pool(p),
	begin(b),
	end(e) {}
  };
  std::deque<Item*> q;

  struct WQ : public ThreadPool::WorkQueue<Item> {
    ParallelPGMapper *m;

    WQ(ParallelPGMapper *m_, ThreadPool *tp)
      : ThreadPool::WorkQueue<Item>("ParallelPGMapper::WQ", 0, 0, tp),
        m(m_) {}

    bool _enqueue(Item *i) override {
      m->q.push_back(i);
      return true;
    }
    void _dequeue(Item *i) override {
      ceph_abort();
    }
    Item *_dequeue() override {
      while (!m->q.empty()) {
	Item *i = m->q.front();
	m->q.pop_front();
	if (i->job->aborted) {
	  i->job->finish_one();
	  delete i;
	} else {
	  return i;
	}
      }
      return nullptr;
    }

    void _process(Item *i, ThreadPool::TPHandle &h) override;

    void _clear() override {
      assert(_empty());
    }

    bool _empty() override {
      return m->q.empty();
    }
  } wq;

public:
  ParallelPGMapper(CephContext *cct, ThreadPool *tp)
    : cct(cct),
      wq(this, tp) {}

  void queue(
    Job *job,
    unsigned pgs_per_item,
    const vector<pg_t>& input_pgs);

  void drain() {
    wq.drain();
  }
};


/// a precalculated mapping of every PG for a given OSDMap
class OSDMapMapping {
public:
  MEMPOOL_CLASS_HELPERS();
private:

  struct PoolMapping {
    MEMPOOL_CLASS_HELPERS();

    unsigned size = 0;
    unsigned pg_num = 0;
    mempool::osdmap_mapping::vector<int32_t> table;

    size_t row_size() const {
      return
	1 + // acting_primary
	1 + // up_primary
	1 + // num acting
	1 + // num up
	size + // acting
	size;  // up
    }

    PoolMapping(int s, int p)
      : size(s),
	pg_num(p),
	table(pg_num * row_size()) {
    }

    void get(size_t ps,
	     std::vector<int> *up,
	     int *up_primary,
	     std::vector<int> *acting,
	     int *acting_primary) const {
      const int32_t *row = &table[row_size() * ps];
      if (acting_primary) {
	*acting_primary = row[0];
      }
      if (up_primary) {
	*up_primary = row[1];
      }
      if (acting) {
	acting->resize(row[2]);
	for (int i = 0; i < row[2]; ++i) {
	  (*acting)[i] = row[4 + i];
	}
      }
      if (up) {
	up->resize(row[3]);
	for (int i = 0; i < row[3]; ++i) {
	  (*up)[i] = row[4 + size + i];
	}
      }
    }

    void set(size_t ps,
	     const std::vector<int>& up,
	     int up_primary,
	     const std::vector<int>& acting,
	     int acting_primary) {
      int32_t *row = &table[row_size() * ps];
      row[0] = acting_primary;
      row[1] = up_primary;
      row[2] = acting.size();
      row[3] = up.size();
      for (int i = 0; i < row[2]; ++i) {
	row[4 + i] = acting[i];
      }
      for (int i = 0; i < row[3]; ++i) {
	row[4 + size + i] = up[i];
      }
    }
  };

  mempool::osdmap_mapping::map<int64_t,PoolMapping> pools;
  mempool::osdmap_mapping::vector<
    mempool::osdmap_mapping::vector<pg_t>> acting_rmap;  // osd -> pg
  //unused: mempool::osdmap_mapping::vector<std::vector<pg_t>> up_rmap;  // osd -> pg
  epoch_t epoch = 0;
  uint64_t num_pgs = 0;

  void _init_mappings(const OSDMap& osdmap);
  void _update_range(
    const OSDMap& map,
    int64_t pool,
    unsigned pg_begin, unsigned pg_end);

  void _build_rmap(const OSDMap& osdmap);

  void _start(const OSDMap& osdmap) {
    _init_mappings(osdmap);
  }
  void _finish(const OSDMap& osdmap);

  void _dump();

  friend class ParallelPGMapper;

  struct MappingJob : public ParallelPGMapper::Job {
    OSDMapMapping *mapping;
    MappingJob(const OSDMap *osdmap, OSDMapMapping *m)
      : Job(osdmap), mapping(m) {
      mapping->_start(*osdmap);
    }
    void process(const vector<pg_t>& pgs) override {}
    void process(int64_t pool, unsigned ps_begin, unsigned ps_end) override {
      mapping->_update_range(*osdmap, pool, ps_begin, ps_end);
    }
    void complete() override {
      mapping->_finish(*osdmap);
    }
  };

public:
  void get(pg_t pgid,
	   std::vector<int> *up,
	   int *up_primary,
	   std::vector<int> *acting,
	   int *acting_primary) const {
    auto p = pools.find(pgid.pool());
    assert(p != pools.end());
    assert(pgid.ps() < p->second.pg_num);
    p->second.get(pgid.ps(), up, up_primary, acting, acting_primary);
  }

  const mempool::osdmap_mapping::vector<pg_t>& get_osd_acting_pgs(unsigned osd) {
    assert(osd < acting_rmap.size());
    return acting_rmap[osd];
  }
  /* unsued
  const std::vector<pg_t>& get_osd_up_pgs(unsigned osd) {
    assert(osd < up_rmap.size());
    return up_rmap[osd];
  }
  */

  void update(const OSDMap& map);
  void update(const OSDMap& map, pg_t pgid);

  std::unique_ptr<MappingJob> start_update(
    const OSDMap& map,
    ParallelPGMapper& mapper,
    unsigned pgs_per_item) {
    std::unique_ptr<MappingJob> job(new MappingJob(&map, this));
    mapper.queue(job.get(), pgs_per_item, {});
    return job;
  }

  epoch_t get_epoch() const {
    return epoch;
  }

  uint64_t get_num_pgs() const {
    return num_pgs;
  }
};


#endif
