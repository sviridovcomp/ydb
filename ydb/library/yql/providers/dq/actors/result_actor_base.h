#pragma once

#include <ydb/library/yql/providers/dq/actors/actor_helpers.h>
#include <ydb/library/yql/providers/dq/actors/events.h>
#include <ydb/library/yql/providers/dq/actors/proto_builder.h>
#include <ydb/library/yql/providers/dq/api/protos/dqs.pb.h>
#include <ydb/library/yql/providers/dq/common/yql_dq_settings.h>
#include <ydb/library/yql/providers/dq/counters/counters.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/utils/failure_injector/failure_injector.h>

#include <util/stream/holder.h>
#include <util/stream/length.h>

namespace NYql::NDqs::NExecutionHelpers {

    template <class TDerived>
    class TResultActorBase : public NYql::TSynchronizableRichActor<TDerived>, public NYql::TCounters {
    protected:
        using TBase = NYql::TSynchronizableRichActor<TDerived>;

        TResultActorBase(
            const TVector<TString>& columns, 
            const NActors::TActorId& executerId, 
            const TString& traceId, 
            const TDqConfiguration::TPtr& settings, 
            const TString& resultType, 
            NActors::TActorId graphExecutionEventsId, 
            bool discard)
            : TBase(&TDerived::Handler)
            , FullResultTableEnabled(settings->EnableFullResultWrite.Get().GetOrElse(false))
            , ExecuterID(executerId)
            , GraphExecutionEventsId(graphExecutionEventsId)
            , Discard(discard)
            , DataParts()
            , TraceId(traceId)
            , Settings(settings)
            , SizeLimit(
                (Settings && Settings->_AllResultsBytesLimit.Get().Defined()) 
                ? Settings->_AllResultsBytesLimit.Get().GetRef() 
                : 64000000) // GRPC limit
            , RowsLimit(settings ? Settings->_RowsLimitPerWrite.Get() : Nothing())
            , Rows(0)
            , Truncated(false)
            , FullResultWriterID()
            , ResultBuilder(MakeHolder<TProtoBuilder>(resultType, columns))
            , ResultYson()
            , ResultYsonOut(new THoldingStream<TCountingOutput>(MakeHolder<TStringOutput>(ResultYson)))
            , ResultYsonWriter(MakeHolder<NYson::TYsonWriter>(ResultYsonOut.Get(), NYson::EYsonFormat::Binary, ::NYson::EYsonType::Node, true))
            , FullResultSentBytes(0)
            , FullResultReceivedBytes(0)
            , FullResultSentDataParts(0)
            , Issues()
            , FinishCalled(false)
            , BlockingActors()
            , QueryResponse() {
            ResultYsonWriter->OnBeginList();
            YQL_LOG(DEBUG) << "_AllResultsBytesLimit = " << SizeLimit;
            YQL_LOG(DEBUG) << "_RowsLimitPerWrite = " << (RowsLimit.Defined() ? ToString(RowsLimit.GetRef()) : "nothing");
        }

        void OnQueryResult(TEvQueryResponse::TPtr& ev, const NActors::TActorContext&) {
            YQL_LOG_CTX_SCOPE(TraceId);
            YQL_ENSURE(!ev->Get()->Record.HasResultSet() && ev->Get()->Record.GetYson().empty());
            YQL_LOG(DEBUG) << "Shutting down TResultAggregator";

            BlockingActors.clear();
            if (FullResultWriterID) {
                BlockingActors.insert(FullResultWriterID);
                TBase::Send(FullResultWriterID, MakeHolder<NActors::TEvents::TEvPoison>());
            }

            YQL_LOG(DEBUG) << "Waiting for " << BlockingActors.size() << " blocking actors";

            QueryResponse.Reset(ev->Release().Release());
            TBase::Become(&TResultActorBase::ShutdownHandler);
            TBase::Send(TBase::SelfId(), MakeHolder<NActors::TEvents::TEvGone>());
        }

        void OnReceiveData(NYql::NDqProto::TData&& data) {
            YQL_LOG_CTX_SCOPE(TraceId);

            if (Discard) {
                return;
            }

            if (FullResultTableEnabled && FullResultWriterID) {
                WriteToFullResultTable(MakeHolder<NDqProto::TData>(data));
            } else {
                DataParts.emplace_back(data); // todo: seems like DataParts stores at most 1 element (replace with holder)

                bool full = true;
                bool exceedRows = false;
                try {
                    TFailureInjector::Reach("result_actor_base_fail_on_response_write", [] { throw yexception() << "result_actor_base_fail_on_response_write"; });
                    full = ResultBuilder->WriteYsonData(DataParts.back(), [this, &exceedRows](const TString& rawYson) {
                        if (RowsLimit && Rows + 1 > *RowsLimit) {
                            exceedRows = true;
                            return false;
                        } else if (ResultYsonOut->Counter() + rawYson.size() > SizeLimit) {
                            return false;
                        }
                        ResultYsonWriter->OnListItem();
                        ResultYsonWriter->OnRaw(rawYson);
                        ++Rows;
                        return true;
                    });
                } catch (...) {
                    OnError(CurrentExceptionMessage(), false, true);
                    return;
                }

                if (full) {
                    return;
                }

                Truncated = true;
                if (FullResultTableEnabled) {
                    FlushCurrent();
                } else {
                    TString issueMsg;
                    if (exceedRows) {
                        issueMsg = TStringBuilder() << "Rows limit reached: " << *RowsLimit;
                    } else {
                        issueMsg = TStringBuilder() << "Size limit reached: " << SizeLimit;
                    }
                    TIssue issue(issueMsg);
                    issue.Severity = TSeverityIds::S_WARNING;
                    Issues.AddIssues({issue});
                    Finish();
                }
            }
        }

        void OnFullResultWriterShutdown() {
            YQL_LOG_CTX_SCOPE(TraceId);
            YQL_LOG(DEBUG) << "Got TEvGone";

            FullResultWriterID = {};
        }

        void OnFullResultWriterResponse(NYql::NDqs::TEvDqFailure::TPtr& ev, const NActors::TActorContext&) {
            YQL_LOG_CTX_SCOPE(TraceId);
            YQL_LOG(DEBUG) << __FUNCTION__;
            if (ev->Get()->Record.IssuesSize() == 0) {
                DoFinish();
            } else {
                TBase::Send(ExecuterID, ev->Release().Release());
            }
        }

        void OnError(const TString& message, bool retriable, bool needFallback) {
            YQL_LOG(ERROR) << "OnError " << message;
            auto issueCode = needFallback
                ? TIssuesIds::DQ_GATEWAY_NEED_FALLBACK_ERROR
                : TIssuesIds::DQ_GATEWAY_ERROR;
            const auto issue = TIssue(message).SetCode(issueCode, TSeverityIds::S_ERROR);
            Issues.AddIssues({issue});  // remember issue to pass it with TEvQueryResponse, cause executor_actor ignores TEvDqFailure after finish
            auto req = MakeHolder<TEvDqFailure>(issue, retriable, needFallback);
            FlushCounters(req->Record);
            TBase::Send(ExecuterID, req.Release());
        }

        void Finish() {
            YQL_LOG(DEBUG) << __FUNCTION__ << ", truncated=" << Truncated;
            YQL_ENSURE(!FinishCalled);
            FinishCalled = true;

            if (FullResultWriterID) {
                NDqProto::TPullResponse response;
                response.SetResponseType(NDqProto::EPullResponseType::FINISH);
                TBase::Send(FullResultWriterID, MakeHolder<TEvPullDataResponse>(response));
            } else {
                DoFinish();
            }
        }

        void OnUndelivered(NActors::TEvents::TEvUndelivered::TPtr& ev) {
            YQL_LOG_CTX_SCOPE(TraceId);
            TString message = "Undelivered from " + ToString(ev->Sender) + " to " + ToString(TBase::SelfId())
                + " reason: " + ToString(ev->Get()->Reason) + " sourceType: " + ToString(ev->Get()->SourceType >> 16)
                + "." + ToString(ev->Get()->SourceType & 0xFFFF);
            OnError(message, true, true);
        }

    private:
        STFUNC(ShutdownHandler) {
            Y_UNUSED(ctx);
            switch (const ui32 etype = ev->GetTypeRewrite()) {
                HFunc(NActors::TEvents::TEvGone, OnShutdownQueryResult);
                cFunc(NActors::TEvents::TEvPoison::EventType, TBase::PassAway);
                HFunc(TEvDqFailure, OnErrorInShutdownState);
                default:
                    YQL_LOG_CTX_SCOPE(TraceId);
                    YQL_LOG(DEBUG) << "Unexpected event " << etype;
                    break;
            }
        }

    private:
        void OnErrorInShutdownState(NYql::NDqs::TEvDqFailure::TPtr& ev, const NActors::TActorContext&) {
            // FullResultWriter will always send TEvGone after this, so these issues will be passed to executor with TEvQueryResponse 
            TIssues issues;
            IssuesFromMessage(ev->Get()->Record.GetIssues(), issues);
            Issues.AddIssues(issues);
        }

        void OnShutdownQueryResult(NActors::TEvents::TEvGone::TPtr& ev, const NActors::TActorContext&) {
            YQL_LOG_CTX_SCOPE(TraceId);
            auto iter = BlockingActors.find(ev->Sender);
            if (iter != BlockingActors.end()) {
                BlockingActors.erase(iter);
            }

            YQL_LOG(DEBUG) << "Shutting down TResultAggregator, " << BlockingActors.size() << " blocking actors left";

            if (BlockingActors.empty()) {
                EndOnQueryResult();
            }
        }

        void DoFinish() {
            TBase::Send(ExecuterID, new TEvGraphFinished());
        }

        void FlushCurrent() {
            YQL_LOG(DEBUG) << __FUNCTION__;
            YQL_ENSURE(!FullResultWriterID);
            YQL_ENSURE(FullResultTableEnabled);

            NDqProto::TGraphExecutionEvent record;
            record.SetEventType(NDqProto::EGraphExecutionEventType::FULL_RESULT);
            NDqProto::TGraphExecutionEvent::TFullResultDescriptor payload;
            payload.SetResultType(ResultBuilder->GetSerializedType());
            record.MutableMessage()->PackFrom(payload);
            TBase::Send(GraphExecutionEventsId, new TEvGraphExecutionEvent(record));
            TBase::template Synchronize<TEvGraphExecutionEvent>([this](TEvGraphExecutionEvent::TPtr& ev) {
                Y_VERIFY(ev->Get()->Record.GetEventType() == NYql::NDqProto::EGraphExecutionEventType::SYNC);
                YQL_LOG_CTX_SCOPE(TraceId);

                if (auto msg = ev->Get()->Record.GetErrorMessage()) {
                    OnError(msg, false, true);
                } else {
                    NActorsProto::TActorId fullResultWriterProto;
                    ev->Get()->Record.GetMessage().UnpackTo(&fullResultWriterProto);
                    FullResultWriterID = NActors::ActorIdFromProto(fullResultWriterProto);
                    WriteAllDataPartsToFullResultTable();
                }
            });
        }

        void EndOnQueryResult() {
            YQL_LOG(DEBUG) << __FUNCTION__;
            NDqProto::TQueryResponse result = QueryResponse->Record;

            YQL_ENSURE(!result.HasResultSet() && result.GetYson().empty());
            FlushCounters(result);

            if (ResultYsonWriter) {
                ResultYsonWriter->OnEndList();
                ResultYsonWriter.Destroy();
            }
            ResultYsonOut.Destroy();

            *result.MutableYson() = ResultYson;

            if (!Issues.Empty()) {
                NYql::IssuesToMessage(Issues, result.MutableIssues());
            }
            result.SetTruncated(Truncated);

            TBase::Send(ExecuterID, new TEvQueryResponse(std::move(result)));
        }

        void DoPassAway() override {
            YQL_LOG_CTX_SCOPE(TraceId);
            YQL_LOG(DEBUG) << __FUNCTION__;
        }

        bool CanSendToFullResultWriter() {
            // TODO Customize
            return FullResultSentBytes - FullResultReceivedBytes <= 32_MB;
        }

        template <class TCallback>
        void UpdateEventQueueStatus(TCallback callback) {
            YQL_LOG(DEBUG) << "UpdateEQStatus before: sent " << (FullResultSentBytes / 1024.0) << " kB "
                        << " received " << (FullResultReceivedBytes / 1024.0) << " kB "
                        << " diff " << (FullResultSentBytes - FullResultReceivedBytes) / 1024.0 << " kB";
            TBase::Send(FullResultWriterID, new TEvFullResultWriterStatusRequest());
            TBase::template Synchronize<TEvFullResultWriterStatusResponse>([this, callback](TEvFullResultWriterStatusResponse::TPtr& ev) {
                YQL_LOG_CTX_SCOPE(TraceId);
                this->FullResultReceivedBytes = ev->Get()->Record.GetBytesReceived();
                YQL_LOG(DEBUG) << "UpdateEQStatus after: sent " << (FullResultSentBytes / 1024.0) << " kB "
                            << " received " << (FullResultReceivedBytes / 1024.0) << " kB "
                            << " diff " << (FullResultSentBytes - FullResultReceivedBytes) / 1024.0 << " kB";
                if (ev->Get()->Record.HasErrorMessage()) {
                    YQL_LOG(DEBUG) << "Received error message: " << ev->Get()->Record.GetErrorMessage();
                    OnError(ev->Get()->Record.GetErrorMessage(), false, false);
                    return;
                }
                callback();
            });
        }

        void WriteAllDataPartsToFullResultTable() {
            while (FullResultSentDataParts < DataParts.size() && CanSendToFullResultWriter()) {
                UnsafeWriteToFullResultTable(DataParts[FullResultSentDataParts]);
                DataParts[FullResultSentDataParts].Clear();
                ++FullResultSentDataParts;
            }
            if (FullResultSentDataParts == DataParts.size()) {
                return;
            }
            // here we cannot continue since the event queue is overloaded
            // kind of tail recursion (but without recursion)
            UpdateEventQueueStatus([this]() {
                WriteAllDataPartsToFullResultTable();
            });
        }

        void WriteToFullResultTable(TAutoPtr<NDqProto::TData> data) {
            if (CanSendToFullResultWriter()) {
                UnsafeWriteToFullResultTable(*data);
                return;
            }
            UpdateEventQueueStatus([this, data]() {
                WriteToFullResultTable(data);
            });
        }

        void UnsafeWriteToFullResultTable(const NDqProto::TData& data) {
            NDqProto::TPullResponse response;
            response.SetResponseType(NDqProto::EPullResponseType::CONTINUE);
            response.MutableData()->CopyFrom(data);
            ui64 respSize = response.ByteSizeLong();
            TBase::Send(FullResultWriterID, MakeHolder<TEvPullDataResponse>(response));
            FullResultSentBytes += respSize;
        }

    protected:
        const bool FullResultTableEnabled;
        const NActors::TActorId ExecuterID;
        const NActors::TActorId GraphExecutionEventsId;
        const bool Discard;
        TVector<NDqProto::TData> DataParts;
        const TString TraceId;
        TDqConfiguration::TPtr Settings;
        ui64 SizeLimit;
        TMaybe<ui64> RowsLimit;
        ui64 Rows;
        bool Truncated;
        NActors::TActorId FullResultWriterID;
        THolder<TProtoBuilder> ResultBuilder;
        TString ResultYson;
        THolder<TCountingOutput> ResultYsonOut;
        THolder<NYson::TYsonWriter> ResultYsonWriter;

        ui64 FullResultSentBytes;
        ui64 FullResultReceivedBytes;
        ui64 FullResultSentDataParts;

        TIssues Issues;
        bool FinishCalled;

        THashSet<NActors::TActorId> BlockingActors;
        THolder<TEvQueryResponse> QueryResponse;
    };
} // namespace NYql::NDqs::NExecutionHelpers