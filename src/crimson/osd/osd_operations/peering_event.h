// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <iostream>
#include <seastar/core/future.hh>

#include "crimson/osd/osdmap_gate.h"
#include "crimson/osd/osd_operation.h"
#include "crimson/osd/osd_operations/background_recovery.h"
#include "osd/osd_types.h"
#include "osd/PGPeeringEvent.h"
#include "osd/PeeringState.h"

namespace ceph {
  class Formatter;
}

namespace crimson::osd {

class OSD;
class ShardServices;
class PG;

  class PGPeeringPipeline {
    struct AwaitMap : OrderedExclusivePhaseT<AwaitMap> {
      static constexpr auto type_name = "PeeringEvent::PGPipeline::await_map";
    } await_map;
    struct Process : OrderedExclusivePhaseT<Process> {
      static constexpr auto type_name = "PeeringEvent::PGPipeline::process";
    } process;
    template <class T>
    friend class PeeringEvent;
    friend class LocalPeeringEvent;
    friend class RemotePeeringEvent;
    friend class PGAdvanceMap;
  };

template <class T>
class PeeringEvent : public PhasedOperationT<T> {
public:
  static constexpr OperationTypeCode type = OperationTypeCode::peering_event;

protected:
  PipelineHandle handle;
  PGPeeringPipeline &pp(PG &pg);

  ShardServices &shard_services;
  PeeringCtx ctx;
  pg_shard_t from;
  spg_t pgid;
  float delay = 0;
  PGPeeringEvent evt;

  const pg_shard_t get_from() const {
    return from;
  }

  const spg_t get_pgid() const {
    return pgid;
  }

  const PGPeeringEvent &get_event() const {
    return evt;
  }

  virtual void on_pg_absent();

  virtual typename PeeringEvent::template interruptible_future<>
  complete_rctx(Ref<PG>);

  virtual seastar::future<> complete_rctx_no_pg() { return seastar::now();}
  virtual seastar::future<Ref<PG>> get_pg() = 0;

public:
  template <typename... Args>
  PeeringEvent(
    ShardServices &shard_services, const pg_shard_t &from, const spg_t &pgid,
    Args&&... args) :
    shard_services(shard_services),
    from(from),
    pgid(pgid),
    evt(std::forward<Args>(args)...)
  {}
  template <typename... Args>
  PeeringEvent(
    ShardServices &shard_services, const pg_shard_t &from, const spg_t &pgid,
    float delay, Args&&... args) :
    shard_services(shard_services),
    from(from),
    pgid(pgid),
    delay(delay),
    evt(std::forward<Args>(args)...)
  {}

  void print(std::ostream &) const final;
  void dump_detail(ceph::Formatter* f) const final;
  seastar::future<> start();
};

class RemotePeeringEvent : public PeeringEvent<RemotePeeringEvent> {
protected:
  OSD &osd;
  crimson::net::ConnectionRef conn;

  void on_pg_absent() final;
  PeeringEvent::interruptible_future<> complete_rctx(Ref<PG> pg) override;
  seastar::future<> complete_rctx_no_pg() override;
  seastar::future<Ref<PG>> get_pg() final;

public:
  class OSDPipeline {
    struct AwaitActive : OrderedExclusivePhaseT<AwaitActive> {
      static constexpr auto type_name =
	"PeeringRequest::OSDPipeline::await_active";
    } await_active;
    friend class RemotePeeringEvent;
  };
  class ConnectionPipeline {
    struct AwaitMap : OrderedExclusivePhaseT<AwaitMap> {
      static constexpr auto type_name =
	"PeeringRequest::ConnectionPipeline::await_map";
    } await_map;
    struct GetPG : OrderedExclusivePhaseT<GetPG> {
      static constexpr auto type_name =
	"PeeringRequest::ConnectionPipeline::get_pg";
    } get_pg;
    friend class RemotePeeringEvent;
  };

  template <typename... Args>
  RemotePeeringEvent(OSD &osd, crimson::net::ConnectionRef conn, Args&&... args) :
    PeeringEvent(std::forward<Args>(args)...),
    osd(osd),
    conn(conn)
  {}

#if 0
  std::tuple<
  > tracking_events;
#endif

  std::tuple<
    StartEvent,
    ConnectionPipeline::AwaitMap::BlockingEvent,
    OSD_OSDMapGate::OSDMapBlocker::BlockingEvent,
    ConnectionPipeline::GetPG::BlockingEvent,
    PGMap::PGCreationBlockingEvent,
    PGPeeringPipeline::AwaitMap::BlockingEvent,
    PG_OSDMapGate::OSDMapBlocker::BlockingEvent,
    PGPeeringPipeline::Process::BlockingEvent,
    BackfillRecovery::BackfillRecoveryPipeline::Process::BlockingEvent,
    OSDPipeline::AwaitActive::BlockingEvent,
#if 0
    PGPipeline::WaitForActive::BlockingEvent,
    PGActivationBlocker::BlockingEvent,
    PGPipeline::RecoverMissing::BlockingEvent,
    PGPipeline::GetOBC::BlockingEvent,
    PGPipeline::WaitRepop::BlockingEvent,
    PGPipeline::SendReply::BlockingEvent,
#endif
    CompletionEvent
  > tracking_events;
private:
  ConnectionPipeline &cp();
  OSDPipeline &op();
};

class LocalPeeringEvent final : public PeeringEvent<LocalPeeringEvent> {
protected:
  seastar::future<Ref<PG>> get_pg() final;

  Ref<PG> pg;

public:
  template <typename... Args>
  LocalPeeringEvent(Ref<PG> pg, Args&&... args) :
    PeeringEvent(std::forward<Args>(args)...),
    pg(pg)
  {}

  virtual ~LocalPeeringEvent();

  std::tuple<
    StartEvent,
    PGPeeringPipeline::AwaitMap::BlockingEvent,
    PG_OSDMapGate::OSDMapBlocker::BlockingEvent,
    PGPeeringPipeline::Process::BlockingEvent,
    BackfillRecovery::BackfillRecoveryPipeline::Process::BlockingEvent,
#if 0
    PGPipeline::WaitForActive::BlockingEvent,
    PGActivationBlocker::BlockingEvent,
    PGPipeline::RecoverMissing::BlockingEvent,
    PGPipeline::GetOBC::BlockingEvent,
    PGPipeline::WaitRepop::BlockingEvent,
    PGPipeline::SendReply::BlockingEvent,
#endif
    CompletionEvent
  > tracking_events;
};


}
