#include "granule.h"
#include "storage.h"
#include <library/cpp/actors/core/log.h>

namespace NKikimr::NOlap {

TGranuleAdditiveSummary::ECompactionClass TGranuleMeta::GetCompactionType(const TCompactionLimits& limits) const {
    const TGranuleAdditiveSummary::ECompactionClass classActual = GetAdditiveSummary().GetCompactionClass(
        limits, ModificationLastTime, TMonotonic::Now());
    switch (classActual) {
        case TGranuleAdditiveSummary::ECompactionClass::Split:
        {
            if (GetHardSummary().GetDifferentBorders()) {
                return TGranuleAdditiveSummary::ECompactionClass::Split;
            } else {
                return TGranuleAdditiveSummary::ECompactionClass::NoCompaction;
            }
        }
        case TGranuleAdditiveSummary::ECompactionClass::WaitInternal:
        case TGranuleAdditiveSummary::ECompactionClass::Internal:
        case TGranuleAdditiveSummary::ECompactionClass::NoCompaction:
            return classActual;
    }
}

ui64 TGranuleMeta::Size() const {
    return GetAdditiveSummary().GetGranuleSize();
}

void TGranuleMeta::UpsertPortion(const TPortionInfo& info) {
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "upsert_portion")("portion", info.DebugString())("granule", GetGranuleId());
    auto it = Portions.find(info.GetPortion());
    AFL_VERIFY(info.GetGranule() == GetGranuleId())("event", "incompatible_granule")("portion", info.DebugString())("granule", GetGranuleId());

    AFL_VERIFY(info.Valid())("event", "invalid_portion")("portion", info.DebugString());
    AFL_VERIFY(info.ValidSnapshotInfo())("event", "incorrect_portion_snapshots")("portion", info.DebugString());
    for (auto& record : info.Records) {
        AFL_VERIFY(record.Valid())("event", "incorrect_record")("record", record.DebugString())("portion", info.DebugString());
    }

    Y_VERIFY(Record.Mark <= info.IndexKeyStart());
    if (it == Portions.end()) {
        OnBeforeChangePortion(nullptr, &info);
        Portions.emplace(info.GetPortion(), info);
    } else {
        OnBeforeChangePortion(&it->second, &info);
        it->second = info;
    }
    OnAfterChangePortion();
}

bool TGranuleMeta::ErasePortion(const ui64 portion) {
    auto it = Portions.find(portion);
    if (it == Portions.end()) {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "portion_erased_already")("portion_id", portion)("pathId", Record.PathId);
        return false;
    } else {
        AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "portion_erased")("portion_info", it->second)("pathId", Record.PathId);
    }
    OnBeforeChangePortion(&it->second, nullptr);
    Portions.erase(it);
    OnAfterChangePortion();
    return true;
}

void TGranuleMeta::AddColumnRecord(const TIndexInfo& indexInfo, const TPortionInfo& portion, const TColumnRecord& rec) {
    auto it = Portions.find(portion.GetPortion());
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "add_column_record")("portion_info", portion.DebugString())("record", rec.DebugString());
    if (it == Portions.end()) {
        Y_VERIFY(portion.Records.empty());
        auto portionNew = portion;
        portionNew.AddRecord(indexInfo, rec);
        OnBeforeChangePortion(nullptr, &portionNew);
        Portions.emplace(portion.GetPortion(), std::move(portionNew));
        OnAfterChangePortion();
    } else {
        Y_VERIFY(it->second.IsEqualWithSnapshots(portion));
        auto portionNew = it->second;
        portionNew.AddRecord(indexInfo, rec);
        OnBeforeChangePortion(&it->second, &portionNew);
        it->second = std::move(portionNew);
        OnAfterChangePortion();
    }
}

void TGranuleMeta::OnAfterChangePortion() {
    ModificationLastTime = TMonotonic::Now();
    Owner->UpdateGranuleInfo(*this);
}

void TGranuleMeta::OnBeforeChangePortion(const TPortionInfo* portionBefore, const TPortionInfo* portionAfter) {
    HardSummaryCache = {};
    if (portionBefore) {
        THashMap<TUnifiedBlobId, ui64> blobIdSize;
        for (auto&& i : portionBefore->Records) {
            blobIdSize[i.BlobRange.BlobId] += i.BlobRange.Size;
        }
        for (auto&& i : blobIdSize) {
            PortionInfoGuard.OnDropBlob(portionBefore->IsActive() ? portionBefore->Meta.Produced : NPortion::EProduced::INACTIVE, i.second);
        }
    }
    if (portionAfter) {
        THashMap<TUnifiedBlobId, ui64> blobIdSize;
        for (auto&& i : portionAfter->Records) {
            blobIdSize[i.BlobRange.BlobId] += i.BlobRange.Size;
        }
        for (auto&& i : blobIdSize) {
            PortionInfoGuard.OnNewBlob(portionAfter->IsActive() ? portionAfter->Meta.Produced : NPortion::EProduced::INACTIVE, i.second);
        }
    }
    if (!!AdditiveSummaryCache) {
        auto g = AdditiveSummaryCache->StartEdit(Counters);
        if (portionBefore && portionBefore->IsActive()) {
            g.RemovePortion(*portionBefore);
        }
        if (portionAfter && portionAfter->IsActive()) {
            g.AddPortion(*portionAfter);
        }
    }
}

void TGranuleMeta::OnCompactionFinished() {
    AllowInsertionFlag = false;
    Y_VERIFY(Activity.erase(EActivity::InternalCompaction) || Activity.erase(EActivity::SplitCompaction));
    AFL_DEBUG(NKikimrServices::TX_COLUMNSHARD)("event", "OnCompactionFinished")("info", DebugString());
    CompactionPriorityInfo.OnCompactionFinished();
    Owner->UpdateGranuleInfo(*this);
}

void TGranuleMeta::OnCompactionFailed(const TString& reason) {
    AllowInsertionFlag = false;
    Y_VERIFY(Activity.erase(EActivity::InternalCompaction) || Activity.erase(EActivity::SplitCompaction));
    AFL_WARN(NKikimrServices::TX_COLUMNSHARD)("event", "OnCompactionFailed")("reason", reason)("info", DebugString());
    CompactionPriorityInfo.OnCompactionFailed();
    Owner->UpdateGranuleInfo(*this);
}

void TGranuleMeta::OnCompactionStarted(const bool inGranule) {
    AllowInsertionFlag = false;
    Y_VERIFY(Activity.empty());
    if (inGranule) {
        Activity.emplace(EActivity::InternalCompaction);
    } else {
        Activity.emplace(EActivity::SplitCompaction);
    }
}

void TGranuleMeta::RebuildHardMetrics() const {
    TGranuleHardSummary result;
    std::map<ui32, TColumnSummary> packedSizeByColumns;
    bool differentBorders = false;
    THashSet<NArrow::TReplaceKey> borders;

    for (auto&& i : Portions) {
        if (!i.second.IsActive()) {
            continue;
        }
        if (!differentBorders) {
            borders.insert(i.second.IndexKeyStart());
            borders.insert(i.second.IndexKeyEnd());
            differentBorders = (borders.size() > 1);
        }
        for (auto&& c : i.second.Records) {
            auto it = packedSizeByColumns.find(c.ColumnId);
            if (it == packedSizeByColumns.end()) {
                it = packedSizeByColumns.emplace(c.ColumnId, TColumnSummary(c.ColumnId)).first;
            }
            it->second.AddBlobsData(i.second.IsInserted(), c.BlobRange.Size);
        }
        for (auto&& c : packedSizeByColumns) {
            c.second.AddRecordsData(i.second.IsInserted(), i.second.NumRows());
        }
    }
    {
        std::vector<TColumnSummary> transpSorted;
        transpSorted.reserve(packedSizeByColumns.size());
        for (auto&& i : packedSizeByColumns) {
            transpSorted.emplace_back(i.second);
        }
        const auto pred = [](const TColumnSummary& l, const TColumnSummary& r) {
            return l.GetPackedBlobsSize() > r.GetPackedBlobsSize();
        };
        std::sort(transpSorted.begin(), transpSorted.end(), pred);
        std::swap(result.ColumnIdsSortedBySizeDescending, transpSorted);
    }
    result.DifferentBorders = differentBorders;
    HardSummaryCache = result;
}

void TGranuleMeta::RebuildAdditiveMetrics() const {
    TGranuleAdditiveSummary result;
    {
        auto g = result.StartEdit(Counters);
        for (auto&& i : Portions) {
            if (!i.second.IsActive()) {
                continue;
            }
            g.AddPortion(i.second);
        }
    }
    AdditiveSummaryCache = result;
}

const NKikimr::NOlap::TGranuleAdditiveSummary& TGranuleMeta::GetAdditiveSummary() const {
    if (!AdditiveSummaryCache) {
        RebuildAdditiveMetrics();
    }
    return *AdditiveSummaryCache;
}

TGranuleMeta::TGranuleMeta(const TGranuleRecord& rec, std::shared_ptr<TGranulesStorage> owner, const NColumnShard::TGranuleDataCounters& counters)
    : Owner(owner)
    , Counters(counters)
    , PortionInfoGuard(Owner->GetCounters().BuildPortionBlobsGuard())
    , Record(rec)
{

}

} // namespace NKikimr::NOlap
