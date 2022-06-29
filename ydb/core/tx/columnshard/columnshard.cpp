#include "columnshard_impl.h"
#include "columnshard_txs.h"

namespace NKikimr {

IActor* CreateColumnShard(const TActorId& tablet, TTabletStorageInfo* info) {
    return new NColumnShard::TColumnShard(info, tablet);
}

}

namespace NKikimr::NColumnShard {

void TColumnShard::BecomeBroken(const TActorContext& ctx)
{
    Become(&TThis::StateBroken);
    ctx.Send(Tablet(), new TEvents::TEvPoisonPill);
    ctx.Send(IndexingActor, new TEvents::TEvPoisonPill);
    ctx.Send(CompactionActor, new TEvents::TEvPoisonPill);
    ctx.Send(EvictionActor, new TEvents::TEvPoisonPill);
    StopS3Actors(ctx);
}

void TColumnShard::SwitchToWork(const TActorContext& ctx) {
    Become(&TThis::StateWork);
    LOG_S_INFO("Switched to work at " << TabletID() << " actor " << ctx.SelfID);

    IndexingActor = ctx.Register(CreateIndexingActor(TabletID(), ctx.SelfID));
    CompactionActor = ctx.Register(CreateCompactionActor(TabletID(), ctx.SelfID));
    EvictionActor = ctx.Register(CreateEvictionActor(TabletID(), ctx.SelfID));
    InitS3Actors(ctx, true);

    SignalTabletActive(ctx);
}

void TColumnShard::OnActivateExecutor(const TActorContext& ctx) {
    LOG_S_DEBUG("OnActivateExecutor at " << TabletID() << " actor " << ctx.SelfID);
    Executor()->RegisterExternalTabletCounters(TabletCountersPtr.release());
    BlobManager = std::make_unique<TBlobManager>(Info(), Executor()->Generation());

    auto& icb = *AppData(ctx)->Icb;
    BlobManager->RegisterControls(icb);
    Limits.RegisterControls(icb);
    CompactionLimits.RegisterControls(icb);
    Settings.RegisterControls(icb);

    Execute(CreateTxInitSchema(), ctx);
}

void TColumnShard::Handle(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx) {
    LOG_S_DEBUG("Handle TEvents::TEvPoisonPill");
    Y_UNUSED(ev);
    BecomeBroken(ctx);
}

void TColumnShard::Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev, const TActorContext&) {
    auto tabletId = ev->Get()->TabletId;
    auto clientId = ev->Get()->ClientId;

    if (clientId == StatsReportPipe) {
        if (ev->Get()->Status != NKikimrProto::OK) {
            StatsReportPipe = TActorId();
        }
        return;
    }

    if (PipeClientCache->OnConnect(ev)) {
        LOG_S_DEBUG("Connected to tablet at " << TabletID() << ", remote " << tabletId);
        return;
    }

    LOG_S_INFO("Failed to connect at " << TabletID() << ", remote " << tabletId);
}

void TColumnShard::Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev, const TActorContext&) {
    auto tabletId = ev->Get()->TabletId;
    auto clientId = ev->Get()->ClientId;

    LOG_S_DEBUG("Client pipe reset at " << TabletID() << ", remote " << tabletId);

    if (clientId == StatsReportPipe) {
        StatsReportPipe = TActorId();
        return;
    }

    PipeClientCache->OnDisconnect(ev);
}

void TColumnShard::Handle(TEvTabletPipe::TEvServerConnected::TPtr& ev, const TActorContext&) {
    auto tabletId = ev->Get()->TabletId;
    LOG_S_DEBUG("Server pipe connected at tablet " << TabletID() << ", remote " << tabletId);
}

void TColumnShard::Handle(TEvTabletPipe::TEvServerDisconnected::TPtr& ev, const TActorContext&) {
    auto tabletId = ev->Get()->TabletId;
    LOG_S_DEBUG("Server pipe reset at tablet " << TabletID() << ", remote " << tabletId);
}

void TColumnShard::Handle(TEvPrivate::TEvScanStats::TPtr& ev, const TActorContext &ctx) {
    Y_UNUSED(ctx);

    IncCounter(COUNTER_SCANNED_ROWS, ev->Get()->Rows);
    IncCounter(COUNTER_SCANNED_BYTES, ev->Get()->Bytes);
}

void TColumnShard::Handle(TEvPrivate::TEvReadFinished::TPtr& ev, const TActorContext &ctx) {
    Y_UNUSED(ctx);
    ui64 readCookie = ev->Get()->RequestCookie;
    LOG_S_DEBUG("Finished read cookie: " << readCookie << " at tablet " << TabletID());
    InFlightReadsTracker.RemoveInFlightRequest(ev->Get()->RequestCookie, *BlobManager);

    ui64 txId = ev->Get()->TxId;
    if (ScanTxInFlight.count(txId)) {
        TDuration duration = TAppData::TimeProvider->Now() - ScanTxInFlight[txId];
        IncCounter(COUNTER_SCAN_LATENCY, duration);
        ScanTxInFlight.erase(txId);
        SetCounter(COUNTER_SCAN_IN_FLY, ScanTxInFlight.size());
    }
}

void TColumnShard::Handle(TEvPrivate::TEvPeriodicWakeup::TPtr& ev, const TActorContext& ctx) {
    if (ev->Get()->Manual) {
        EnqueueBackgroundActivities();
        return;
    }

    if (LastBackActivation < TInstant::Now() - ActivationPeriod) {
        SendWaitPlanStep(GetOutdatedStep());
    }

    ctx.Schedule(ActivationPeriod, new TEvPrivate::TEvPeriodicWakeup());
}

void TColumnShard::Handle(TEvMediatorTimecast::TEvRegisterTabletResult::TPtr& ev, const TActorContext&) {
    const auto* msg = ev->Get();
    Y_VERIFY(msg->TabletId == TabletID());
    MediatorTimeCastEntry = msg->Entry;
    Y_VERIFY(MediatorTimeCastEntry);
    LOG_S_DEBUG("Registered with mediator time cast at tablet " << TabletID());

    RescheduleWaitingReads();
}

void TColumnShard::Handle(TEvMediatorTimecast::TEvNotifyPlanStep::TPtr& ev, const TActorContext&) {
    const auto* msg = ev->Get();
    Y_VERIFY(msg->TabletId == TabletID());

    Y_VERIFY(MediatorTimeCastEntry);
    ui64 step = MediatorTimeCastEntry->Get(TabletID());
    LOG_S_DEBUG("Notified by mediator time cast with PlanStep# " << step << " at tablet " << TabletID());

    for (auto it = MediatorTimeCastWaitingSteps.begin(); it != MediatorTimeCastWaitingSteps.end();) {
        if (step < *it) {
            break;
        }
        it = MediatorTimeCastWaitingSteps.erase(it);
    }

    RescheduleWaitingReads();
    EnqueueBackgroundActivities(true);
}

void TColumnShard::UpdateBlobMangerCounters() {
    const auto counters = BlobManager->GetCountersUpdate();
    IncCounter(COUNTER_BLOB_MANAGER_GC_REQUESTS, counters.GcRequestsSent);
    IncCounter(COUNTER_BLOB_MANAGER_KEEP_BLOBS, counters.BlobKeepEntries);
    IncCounter(COUNTER_BLOB_MANAGER_DONT_KEEP_BLOBS, counters.BlobDontKeepEntries);
    IncCounter(COUNTER_BLOB_MANAGER_SKIPPED_BLOBS, counters.BlobSkippedEntries);
    IncCounter(COUNTER_SMALL_BLOB_WRITE_COUNT, counters.SmallBlobsWritten);
    IncCounter(COUNTER_SMALL_BLOB_WRITE_BYTES, counters.SmallBlobsBytesWritten);
    IncCounter(COUNTER_SMALL_BLOB_DELETE_COUNT, counters.SmallBlobsDeleted);
    IncCounter(COUNTER_SMALL_BLOB_DELETE_BYTES, counters.SmallBlobsBytesDeleted);
}

void TColumnShard::UpdateInsertTableCounters() {
    auto& prepared = InsertTable->GetCountersPrepared();
    auto& committed = InsertTable->GetCountersCommitted();

    SetCounter(COUNTER_PREPARED_RECORDS, prepared.Rows);
    SetCounter(COUNTER_PREPARED_BYTES, prepared.Bytes);
    SetCounter(COUNTER_COMMITTED_RECORDS, committed.Rows);
    SetCounter(COUNTER_COMMITTED_BYTES, committed.Bytes);

    LOG_S_DEBUG("InsertTable. Prepared: " << prepared.Bytes << " in " << prepared.Rows
        << " records, committed: " << committed.Bytes << " in " << committed.Rows
        << " records at tablet " << TabletID());
}

void TColumnShard::UpdateIndexCounters() {
    if (!PrimaryIndex) {
        return;
    }

    auto& stats = PrimaryIndex->GetTotalStats();
    SetCounter(COUNTER_INDEX_TABLES, stats.Tables);
    SetCounter(COUNTER_INDEX_GRANULES, stats.Granules);
    SetCounter(COUNTER_INDEX_EMPTY_GRANULES, stats.EmptyGranules);
    SetCounter(COUNTER_INDEX_OVERLOADED_GRANULES, stats.OverloadedGranules);
    SetCounter(COUNTER_INDEX_COLUMN_RECORDS, stats.ColumnRecords);
    SetCounter(COUNTER_INDEX_COLUMN_METADATA_BYTES, stats.ColumnMetadataBytes);
    SetCounter(COUNTER_INSERTED_PORTIONS, stats.Inserted.Portions);
    SetCounter(COUNTER_INSERTED_BLOBS, stats.Inserted.Blobs);
    SetCounter(COUNTER_INSERTED_ROWS, stats.Inserted.Rows);
    SetCounter(COUNTER_INSERTED_BYTES, stats.Inserted.Bytes);
    SetCounter(COUNTER_INSERTED_RAW_BYTES, stats.Inserted.RawBytes);
    SetCounter(COUNTER_COMPACTED_PORTIONS, stats.Compacted.Portions);
    SetCounter(COUNTER_COMPACTED_BLOBS, stats.Compacted.Blobs);
    SetCounter(COUNTER_COMPACTED_ROWS, stats.Compacted.Rows);
    SetCounter(COUNTER_COMPACTED_BYTES, stats.Compacted.Bytes);
    SetCounter(COUNTER_COMPACTED_RAW_BYTES, stats.Compacted.RawBytes);
    SetCounter(COUNTER_SPLIT_COMPACTED_PORTIONS, stats.SplitCompacted.Portions);
    SetCounter(COUNTER_SPLIT_COMPACTED_BLOBS, stats.SplitCompacted.Blobs);
    SetCounter(COUNTER_SPLIT_COMPACTED_ROWS, stats.SplitCompacted.Rows);
    SetCounter(COUNTER_SPLIT_COMPACTED_BYTES, stats.SplitCompacted.Bytes);
    SetCounter(COUNTER_SPLIT_COMPACTED_RAW_BYTES, stats.SplitCompacted.RawBytes);
    SetCounter(COUNTER_INACTIVE_PORTIONS, stats.Inactive.Portions);
    SetCounter(COUNTER_INACTIVE_BLOBS, stats.Inactive.Blobs);
    SetCounter(COUNTER_INACTIVE_ROWS, stats.Inactive.Rows);
    SetCounter(COUNTER_INACTIVE_BYTES, stats.Inactive.Bytes);
    SetCounter(COUNTER_INACTIVE_RAW_BYTES, stats.Inactive.RawBytes);

    SetCounter(COUNTER_EVICTED_PORTIONS, stats.Evicted.Portions);
    SetCounter(COUNTER_EVICTED_BLOBS, stats.Evicted.Blobs);
    SetCounter(COUNTER_EVICTED_ROWS, stats.Evicted.Rows);
    SetCounter(COUNTER_EVICTED_BYTES, stats.Evicted.Bytes);
    SetCounter(COUNTER_EVICTED_RAW_BYTES, stats.Evicted.RawBytes);
}

ui64 TColumnShard::MemoryUsage() const {
    ui64 memory =
        Tables.size() * sizeof(TTableInfo) +
        PathsToDrop.size() * sizeof(ui64) +
        Ttl.PathsCount() * sizeof(TTtl::TDescription) +
        SchemaPresets.size() * sizeof(TSchemaPreset) +
        AltersInFlight.size() * sizeof(TAlterMeta) +
        CommitsInFlight.size() * sizeof(TCommitMeta) +
        TabletCounters->Simple()[COUNTER_PREPARED_RECORDS].Get() * sizeof(NOlap::TInsertedData) +
        TabletCounters->Simple()[COUNTER_COMMITTED_RECORDS].Get() * sizeof(NOlap::TInsertedData);
    if (PrimaryIndex) {
        memory += PrimaryIndex->MemoryUsage();
    }
    return memory;
}

void TColumnShard::UpdateResourceMetrics(const TUsage& usage) {
    auto * metrics = Executor()->GetResourceMetrics();
    if (!metrics) {
        return;
    }

    ui64 storageBytes =
        TabletCounters->Simple()[COUNTER_PREPARED_BYTES].Get() +
        TabletCounters->Simple()[COUNTER_COMMITTED_BYTES].Get() +
        TabletCounters->Simple()[COUNTER_INSERTED_BYTES].Get() +
        TabletCounters->Simple()[COUNTER_COMPACTED_BYTES].Get() +
        TabletCounters->Simple()[COUNTER_SPLIT_COMPACTED_BYTES].Get() +
        TabletCounters->Simple()[COUNTER_INACTIVE_BYTES].Get();

    ui64 memory = MemoryUsage();

    const TActorContext& ctx = TlsActivationContext->AsActorContext();
    TInstant now = AppData(ctx)->TimeProvider->Now();
    metrics->CPU.Increment(usage.CPUExecTime, now);
    metrics->Network.Increment(usage.Network, now);
    //metrics->StorageSystem
    metrics->StorageUser.Set(storageBytes);
    metrics->Memory.Set(memory);
    //metrics->ReadThroughput
    //metrics->WriteThroughput

    metrics->TryUpdate(ctx);
}

void TColumnShard::SendPeriodicStats(const TActorContext& ctx) {
    if (!CurrentSchemeShardId || !StorePathId) {
        LOG_S_DEBUG("Disabled periodic stats at tablet " << TabletID());
        return;
    }

    TInstant now = AppData(ctx)->TimeProvider->Now();
    if (LastStatsReport + StatsReportInterval > now) {
        return;
    }
    LastStatsReport = now;

    if (!StatsReportPipe) {
        LOG_S_DEBUG("Create periodic stats pipe to " << CurrentSchemeShardId << " at tablet " << TabletID());
        NTabletPipe::TClientConfig clientConfig;
        StatsReportPipe = ctx.Register(NTabletPipe::CreateClient(ctx.SelfID, CurrentSchemeShardId, clientConfig));
    }

    auto ev = std::make_unique<TEvDataShard::TEvPeriodicTableStats>(TabletID(), StorePathId);
    {
        ev->Record.SetShardState(2); // NKikimrTxDataShard.EDatashardState.Ready
        ev->Record.SetGeneration(Executor()->Generation());
        ev->Record.SetRound(StatsReportRound++);
        ev->Record.SetNodeId(ctx.ExecutorThread.ActorSystem->NodeId);
        ev->Record.SetStartTime(StartTime().MilliSeconds());

        if (auto* resourceMetrics = Executor()->GetResourceMetrics()) {
            resourceMetrics->Fill(*ev->Record.MutableTabletMetrics());
        }

        auto* tabletStats = ev->Record.MutableTableStats();
        tabletStats->SetTxRejectedByOverload(TabletCounters->Cumulative()[COUNTER_WRITE_OVERLOAD].Get());
        tabletStats->SetTxRejectedBySpace(TabletCounters->Cumulative()[COUNTER_OUT_OF_SPACE].Get());
        tabletStats->SetInFlightTxCount(Executor()->GetStats().TxInFly);

        if (PrimaryIndex) {
            const auto& indexStats = PrimaryIndex->GetTotalStats();
            NOlap::TSnapshot lastIndexUpdate = PrimaryIndex->LastUpdate();
            auto activeIndexStats = indexStats.Active(); // data stats excluding inactive and evicted

            tabletStats->SetRowCount(activeIndexStats.Rows);
            tabletStats->SetDataSize(activeIndexStats.Bytes + TabletCounters->Simple()[COUNTER_COMMITTED_BYTES].Get());
            // TODO: we need row/dataSize counters for evicted data (managed by tablet but stored outside)
            //tabletStats->SetIndexSize(); // TODO: calc size of internal tables
            //tabletStats->SetLastAccessTime(); // TODO: last read/write time
            tabletStats->SetLastUpdateTime(lastIndexUpdate.PlanStep);
        }
    }

    LOG_S_DEBUG("Sending periodic stats at tablet " << TabletID());
    NTabletPipe::SendData(ctx, StatsReportPipe, ev.release());
}

}
