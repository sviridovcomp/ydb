#pragma once

#include <library/cpp/actors/core/log.h>

#if defined SVLOG_T || \
    defined SVLOG_D || \
    defined SVLOG_I || \
    defined SVLOG_N || \
    defined SVLOG_W || \
    defined SVLOG_E || \
    defined SVLOG_CRIT
#error log macro redefinition
#endif

#define SVLOG_T(stream) LOG_TRACE_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_D(stream) LOG_DEBUG_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_I(stream) LOG_INFO_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_N(stream) LOG_NOTICE_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_W(stream) LOG_WARN_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_E(stream) LOG_ERROR_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)
#define SVLOG_CRIT(stream) LOG_CRIT_S((TlsActivationContext->AsActorContext()), NKikimrServices::SYSTEM_VIEWS, stream)

namespace NKikimr {
namespace NSysView {

enum class EProcessorMode {
    MINUTE, // aggregate stats every minute
    FAST // fast mode for tests
};

} // NSysView
} // NKikimr
