#include "read_rule_creator.h"

#include <ydb/core/yq/libs/common/util.h>
#include <ydb/core/yq/libs/events/events.h>

#include <ydb/core/protos/services.pb.h>
#include <ydb/public/sdk/cpp/client/ydb_persqueue_public/persqueue.h>

#include <ydb/library/yql/providers/dq/api/protos/service.pb.h>
#include <ydb/library/yql/providers/pq/proto/dq_task_params.pb.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>

#define LOG_E(stream) \
    LOG_ERROR_S(*TlsActivationContext, NKikimrServices::STREAMS, QueryId << ": " << stream)

#define LOG_I(stream) \
    LOG_INFO_S(*TlsActivationContext, NKikimrServices::STREAMS, QueryId << ": " << stream)

#define LOG_D(stream) \
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::STREAMS, QueryId << ": " << stream)


namespace NYq {
namespace {

using namespace NActors;

struct TEvPrivate {
    // Event ids.
    enum EEv : ui32 {
        EvBegin = EventSpaceBegin(NActors::TEvents::ES_PRIVATE),

        EvSingleReadRuleCreatorResult = EvBegin,
        EvAddReadRuleStatus,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE)");

    // Events.

    struct TEvSingleReadRuleCreatorResult : TEventLocal<TEvSingleReadRuleCreatorResult, EvSingleReadRuleCreatorResult> {
        TEvSingleReadRuleCreatorResult() = default;

        explicit TEvSingleReadRuleCreatorResult(const NYql::TIssues& issues)
            : Issues(issues)
        {
        }

        NYql::TIssues Issues;
    };

    struct TEvAddReadRuleStatus : TEventLocal<TEvAddReadRuleStatus, EvAddReadRuleStatus> {
        TEvAddReadRuleStatus(NYdb::TStatus status)
            : Status(std::move(status))
        {
        }

        NYdb::TStatus Status;
    };
};

// Actor for creating read rule for one topic.
class TSingleReadRuleCreator : public TActorBootstrapped<TSingleReadRuleCreator> {
public:
    TSingleReadRuleCreator(
        NActors::TActorId owner,
        TString queryId,
        NYdb::TDriver ydbDriver,
        const Fq::Private::TopicConsumer& topicConsumer,
        std::shared_ptr<NYdb::ICredentialsProviderFactory> credentialsProvider,
        ui64 index
    )
        : Owner(owner)
        , QueryId(std::move(queryId))
        , TopicConsumer(topicConsumer)
        , YdbDriver(std::move(ydbDriver))
        , PqClient(YdbDriver, GetPqClientSettings(std::move(credentialsProvider)))
        , Index(index)
    {
    }

    void Bootstrap() {
        Become(&TSingleReadRuleCreator::StateFunc);
        StartRequest();
    }

    static constexpr char ActorName[] = "YQ_SINGLE_READ_RULE_CREATOR";

    TString GetTopicPath() const {
        TStringBuilder ret;
        ret << TopicConsumer.database();
        if (ret && ret.back() != '/') {
            ret << '/';
        }
        ret << TopicConsumer.topic_path();
        return std::move(ret);
    }

    void StartRequest() {
        Y_VERIFY(!RequestInFlight);
        RequestInFlight = true;
        LOG_D("Make request for read rule creation for topic `" << TopicConsumer.topic_path() << "` [" << Index << "]");
        PqClient.AddReadRule(
            GetTopicPath(),
            NYdb::NPersQueue::TAddReadRuleSettings()
                .ReadRule(
                    NYdb::NPersQueue::TReadRuleSettings()
                        .ConsumerName(TopicConsumer.consumer_name())
                        .ServiceType("yandex-query")
                        .SupportedCodecs({
                            NYdb::NPersQueue::ECodec::RAW,
                            NYdb::NPersQueue::ECodec::GZIP,
                            NYdb::NPersQueue::ECodec::LZOP,
                            NYdb::NPersQueue::ECodec::ZSTD
                        })
                )
        ).Subscribe(
            [actorSystem = TActivationContext::ActorSystem(), selfId = SelfId()](const NYdb::TAsyncStatus& status) {
                actorSystem->Send(selfId, new TEvPrivate::TEvAddReadRuleStatus(status.GetValue()));
            }
        );
    }

    void Handle(TEvPrivate::TEvAddReadRuleStatus::TPtr& ev) {
        Y_VERIFY(RequestInFlight);
        RequestInFlight = false;
        const NYdb::TStatus& status = ev->Get()->Status;
        if (status.IsSuccess() || status.GetStatus() == NYdb::EStatus::ALREADY_EXISTS) {
            Send(Owner, MakeHolder<TEvPrivate::TEvSingleReadRuleCreatorResult>(), 0, Index);
            PassAway();
        } else {
            if (!RetryState) {
                RetryState = NYdb::NPersQueue::IRetryPolicy::GetExponentialBackoffPolicy()->CreateRetryState();
            }
            TMaybe<TDuration> nextRetryDelay = RetryState->GetNextRetryDelay(status.GetStatus());
            if (status.GetStatus() == NYdb::EStatus::SCHEME_ERROR) {
                nextRetryDelay = Nothing(); // Not retryable
            }

            LOG_D("Failed to add read rule to `" << TopicConsumer.topic_path() << "`: " << status.GetIssues().ToOneLineString() << ". Status: " << status.GetStatus() << ". Retry after: " << nextRetryDelay);
            if (!nextRetryDelay) { // Not retryable
                Send(Owner, MakeHolder<TEvPrivate::TEvSingleReadRuleCreatorResult>(status.GetIssues()), 0, Index);
                PassAway();
            } else {
                if (!CheckFinish()) {
                    Schedule(*nextRetryDelay, new NActors::TEvents::TEvWakeup());
                }
            }
        }
    }

    void Handle(NActors::TEvents::TEvWakeup::TPtr&) {
        if (!CheckFinish()) {
            StartRequest();
        }
    }

    void Handle(NActors::TEvents::TEvPoison::TPtr& ev) {
        Y_VERIFY(ev->Sender == Owner);
        Finishing = true;
        CheckFinish();
    }

    bool CheckFinish() {
        if (Finishing && !RequestInFlight) {
            Send(Owner, MakeHolder<TEvPrivate::TEvSingleReadRuleCreatorResult>(), 0, Index);
            PassAway();
            return true;
        }
        return false;
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvAddReadRuleStatus, Handle);
        hFunc(NActors::TEvents::TEvWakeup, Handle);
        hFunc(NActors::TEvents::TEvPoison, Handle);
    )

private:
    NYdb::NPersQueue::TPersQueueClientSettings GetPqClientSettings(std::shared_ptr<NYdb::ICredentialsProviderFactory> credentialsProvider) {
        return NYdb::NPersQueue::TPersQueueClientSettings()
            .ClusterDiscoveryMode(NYdb::NPersQueue::EClusterDiscoveryMode::Off)
            .Database(TopicConsumer.database())
            .DiscoveryEndpoint(TopicConsumer.cluster_endpoint())
            .CredentialsProviderFactory(std::move(credentialsProvider))
            .DiscoveryMode(NYdb::EDiscoveryMode::Async)
            .SslCredentials(NYdb::TSslCredentials(TopicConsumer.use_ssl()));
    }

private:
    const NActors::TActorId Owner;
    const TString QueryId;
    const Fq::Private::TopicConsumer TopicConsumer;
    NYdb::TDriver YdbDriver;
    NYdb::NPersQueue::TPersQueueClient PqClient;
    ui64 Index = 0;
    NYdb::NPersQueue::IRetryPolicy::IRetryState::TPtr RetryState;
    bool RequestInFlight = false;
    bool Finishing = false;
};

// Actor for creating read rules for all topics in the query.
class TReadRuleCreator : public TActorBootstrapped<TReadRuleCreator> {
public:
    TReadRuleCreator(
        NActors::TActorId owner,
        TString queryId,
        NYdb::TDriver ydbDriver,
        const ::google::protobuf::RepeatedPtrField<Fq::Private::TopicConsumer>& topicConsumers,
        TVector<std::shared_ptr<NYdb::ICredentialsProviderFactory>> credentials
    )
        : Owner(owner)
        , QueryId(std::move(queryId))
        , YdbDriver(std::move(ydbDriver))
        , TopicConsumers(VectorFromProto(topicConsumers))
        , Credentials(std::move(credentials))
    {
        Y_VERIFY(!TopicConsumers.empty());
        Results.resize(TopicConsumers.size());
    }

    static constexpr char ActorName[] = "YQ_READ_RULE_CREATOR";

    void Bootstrap() {
        Become(&TReadRuleCreator::StateFunc);

        Children.reserve(TopicConsumers.size());
        Results.reserve(TopicConsumers.size());
        for (size_t i = 0; i < TopicConsumers.size(); ++i) {
            LOG_D("Create read rule creation actor for `" << TopicConsumers[i].topic_path() << "` [" << i << "]");
            Children.push_back(Register(new TSingleReadRuleCreator(SelfId(), QueryId, YdbDriver, TopicConsumers[i], Credentials[i], i)));
        }
    }

    void Handle(TEvPrivate::TEvSingleReadRuleCreatorResult::TPtr& ev) {
        const ui64 index = ev->Cookie;
        Y_VERIFY(!Results[index]);
        if (ev->Get()->Issues) {
            Ok = false;
        }
        Results[index] = std::move(ev);
        ++ResultsGot;
        SendResultsAndPassAwayIfDone();
    }

    void Handle(NActors::TEvents::TEvPoison::TPtr& ev) {
        Y_VERIFY(ev->Sender == Owner);
        for (const NActors::TActorId& child : Children) {
            Send(child, new NActors::TEvents::TEvPoison());
        }
    }

    void SendResultsAndPassAwayIfDone() {
        Y_VERIFY(ResultsGot <= TopicConsumers.size());
        if (ResultsGot == TopicConsumers.size()) {
            NYql::TIssues issues;
            if (!Ok) {
                NYql::TIssue mainIssue("Failed to create read rules for topics");
                for (auto& result : Results) {
                    for (const NYql::TIssue& issue : result->Get()->Issues) {
                        mainIssue.AddSubIssue(MakeIntrusive<NYql::TIssue>(issue));
                    }
                }
                issues.AddIssue(std::move(mainIssue));
            }
            Send(Owner, MakeHolder<NYq::TEvents::TEvDataStreamsReadRulesCreationResult>(std::move(issues)));
            PassAway();
        }
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvSingleReadRuleCreatorResult, Handle);
        hFunc(NActors::TEvents::TEvPoison, Handle);
    )

private:
    const NActors::TActorId Owner;
    const TString QueryId;
    NYdb::TDriver YdbDriver;
    const TVector<Fq::Private::TopicConsumer> TopicConsumers;
    const TVector<std::shared_ptr<NYdb::ICredentialsProviderFactory>> Credentials;
    size_t ResultsGot = 0;
    bool Ok = true;
    TVector<TEvPrivate::TEvSingleReadRuleCreatorResult::TPtr> Results;
    TVector<NActors::TActorId> Children;
};

} // namespace

NActors::IActor* MakeReadRuleCreatorActor(
    NActors::TActorId owner,
    TString queryId,
    NYdb::TDriver ydbDriver,
    const ::google::protobuf::RepeatedPtrField<Fq::Private::TopicConsumer>& topicConsumers,
    TVector<std::shared_ptr<NYdb::ICredentialsProviderFactory>> credentials
)
{
    return new TReadRuleCreator(
        owner,
        std::move(queryId),
        std::move(ydbDriver),
        topicConsumers,
        std::move(credentials)
    );
}

} // namespace NYq
