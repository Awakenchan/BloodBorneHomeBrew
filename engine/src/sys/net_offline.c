// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_NOT_EQUAL ((int32_t)0x80550609)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_INVALID_VALUE ((int32_t)0x80431077)
enum { NET_ERRNO_AFNOSUPPORT = 47, NET_ERRNO_NETUNREACH = 51, NET_AF_INET = 2, NAT_TYPE_3 = 3 };

static _Atomic int last_id;
static int32_t net_errno;

static int fresh_id(void) {
    return atomic_fetch_add(&last_id, 1) + 1;
}

static SYS_ABI int32_t ok(void) {
    return 0;
}

static SYS_ABI int32_t ok_one(int id) {
    (void)id;
    return 0;
}

static SYS_ABI int32_t *net_errno_loc(void) {
    return &net_errno;
}

static SYS_ABI int32_t net_pool_create(const char *name, int size, int flags) {
    (void)name, (void)size, (void)flags;
    return fresh_id();
}

static SYS_ABI int32_t net_unreachable(void) {
    net_errno = NET_ERRNO_NETUNREACH;
    return NET_ENETUNREACH;
}

static SYS_ABI int32_t net_epoll_create(const char *name, int flags) {
    (void)name, (void)flags;
    return fresh_id();
}

static SYS_ABI int32_t net_resolver_create(const char *name, int pool, int flags) {
    (void)name, (void)pool, (void)flags;
    return fresh_id();
}

static SYS_ABI uint16_t net_htons(uint16_t value) {
    return __builtin_bswap16(value);
}

static SYS_ABI uint16_t net_ntohs(uint16_t value) {
    return __builtin_bswap16(value);
}

static SYS_ABI uint32_t net_htonl(uint32_t value) {
    return __builtin_bswap32(value);
}

static SYS_ABI uint32_t net_ntohl(uint32_t value) {
    return __builtin_bswap32(value);
}

static SYS_ABI int32_t net_pton(int family, const char *source, void *target) {
    if (family != NET_AF_INET) {
        net_errno = NET_ERRNO_AFNOSUPPORT;
        return NET_EINVAL;
    }
#ifdef _WIN32
    return runtime_win_inet_pton4(source, target);
#else
    return inet_pton(AF_INET, source, target);
#endif
}

static SYS_ABI const char *net_ntop(int family, const void *source, char *target, uint32_t size) {
    if (family != NET_AF_INET) {
        net_errno = NET_ERRNO_AFNOSUPPORT;
        return NULL;
    }
#ifdef _WIN32
    return runtime_win_inet_ntop4(source, target, size);
#else
    return inet_ntop(AF_INET, source, target, size);
#endif
}

static SYS_ABI int32_t netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    *state = 0;
    return 0;
}

static SYS_ABI int32_t netctl_info(int code, void *info) {
    (void)code, (void)info;
    return NET_CTL_NOT_CONNECTED;
}

static SYS_ABI int32_t netctl_register(void *callback, void *argument, int32_t *id) {
    (void)callback, (void)argument;
    if (!id) return NET_CTL_INVALID_ADDR;
    *id = fresh_id();
    return 0;
}

static SYS_ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1] = 0;
    info[2] = NAT_TYPE_3;
    info[3] = 0;
    return 0;
}

static SYS_ABI int32_t lib_init_id(void) {
    return fresh_id();
}

static SYS_ABI int32_t http_fail(void) {
    return HTTP_NETWORK;
}

static SYS_ABI int32_t http_object(void) {
    return fresh_id();
}

static SYS_ABI int32_t http_epoll(int32_t context, void **handle) {
    (void)context;
    if (!handle) return HTTP_INVALID_VALUE;
    *handle = (void *)(uintptr_t)(0x100 + fresh_id());
    return 0;
}

static SYS_ABI int32_t http_wait(void *handle, void *events, int32_t max, int64_t timeout) {
    (void)handle, (void)events, (void)max;
    if (timeout > 0) host_sleep_ns((uint64_t)timeout * 1000);
    return 0;
}

static SYS_ABI int32_t np_state(int32_t user, int32_t *state) {
    (void)user;
    if (!state) return NP_INVALID_ARGUMENT;
    *state = 1;
    return 0;
}

static SYS_ABI int32_t np_signed_out(void) {
    return NP_SIGNED_OUT;
}

static SYS_ABI int32_t np_register(void *callback, void *argument) {
    (void)callback, (void)argument;
    return fresh_id();
}

static SYS_ABI void np_register_void(void *callback, void *argument) {
    (void)callback, (void)argument;
}

static SYS_ABI int32_t np_request(const void *param) {
    (void)param;
    return fresh_id();
}

static SYS_ABI int32_t np_request_ctx(int32_t context, const void *param) {
    (void)context, (void)param;
    return fresh_id();
}

static SYS_ABI int32_t np_poll(int32_t request, int32_t *result) {
    (void)request;
    if (result) *result = NP_SIGNED_OUT;
    return 0;
}

static SYS_ABI int32_t np_compare(const void *a, const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a, b, 16) ? NP_NOT_EQUAL : 0;
}

static const SysExport offline_exports[] = {
    {"Nlev7Lg8k3A", "sceNetInit", ok},
    {"cTGkc6-TBlI", "sceNetTerm", ok},
    {"HQOwnfMGipQ", "sceNetErrnoLoc", net_errno_loc},
    {"dgJBaeJnGpo", "sceNetPoolCreate", net_pool_create},
    {"K7RlrTkI-mw", "sceNetPoolDestroy", ok_one},
    {"SF47kB2MNTo", "sceNetEpollCreate", net_epoll_create},
    {"Inp1lfL+Jdw", "sceNetEpollDestroy", ok_one},
    {"C4UgDHHPvdw", "sceNetResolverCreate", net_resolver_create},
    {"kJlYH5uMAWI", "sceNetResolverDestroy", ok_one},
    {"iWQWrwiSt8A", "sceNetHtons", net_htons},
    {"Rbvt+5Y2iEw", "sceNetNtohs", net_ntohs},
    {"9T2pDF2Ryqg", "sceNetHtonl", net_htonl},
    {"pQGpHYopAIY", "sceNetNtohl", net_ntohl},
    {"8Kcp5d-q1Uo", "sceNetInetPton", net_pton},
    {"9vA2aW+CHuA", "sceNetInetNtop", net_ntop},
    {"Q4qBuN-c0ZM", "sceNetSocket", net_unreachable},
    {"OXXX4mUk3uk", "sceNetConnect", net_unreachable},
    {"bErx49PgxyY", "sceNetBind", net_unreachable},
    {"kOj1HiAGE54", "sceNetListen", net_unreachable},
    {"PIWqhn9oSxc", "sceNetAccept", net_unreachable},
    {"beRjXBn-z+o", "sceNetSend", net_unreachable},
    {"gvD1greCu0A", "sceNetSendto", net_unreachable},
    {"9wO9XrMsNhc", "sceNetRecv", net_unreachable},
    {"304ooNZxWDY", "sceNetRecvfrom", net_unreachable},
    {"2mKX2Spso7I", "sceNetSetsockopt", net_unreachable},
    {"xphrZusl78E", "sceNetGetsockopt", net_unreachable},
    {"hoOAofhhRvE", "sceNetGetsockname", net_unreachable},
    {"TSM6whtekok", "sceNetShutdown", net_unreachable},
    {"45ggEzakPJQ", "sceNetSocketClose", net_unreachable},
    {"zJGf8xjFnQE", "sceNetSocketAbort", net_unreachable},
    {"ZVw46bsasAk", "sceNetEpollControl", net_unreachable},
    {"drjIbDbA7UQ", "sceNetEpollWait", net_unreachable},
    {"w21YgGGNtBk", "sceNetEpollAbort", net_unreachable},
    {"Nd91WaWmG2w", "sceNetResolverStartNtoa", net_unreachable},
    {"Apb4YDxKsRI", "sceNetResolverStartAton", net_unreachable},
    {"uBPlr0lbuiI", "sceNetCtlGetState", netctl_state},
    {"obuxdTiwkF8", "sceNetCtlGetInfo", netctl_info},
    {"UJ+Z7Q+4ck0", "sceNetCtlRegisterCallback", netctl_register},
    {"iQw3iQPhvUQ", "sceNetCtlCheckCallback", ok},
    {"Rqm2OnZMCz0", "sceNetCtlUnregisterCallback", ok_one},
    {"JO4yuTuMoKI", "sceNetCtlGetNatInfo", netctl_nat},
    {"hdpVEUDFW3s", "sceSslInit", lib_init_id},
    {"0K1yQ6Lv-Yc", "sceSslTerm", ok},
    {"A9cVMUtEp4Y", "sceHttpInit", lib_init_id},
    {"Ik-KpLTlf7Q", "sceHttpTerm", ok},
    {"0gYjPTR-6cY", "sceHttpCreateTemplate", http_object},
    {"4I8vEpuEhZ8", "sceHttpDeleteTemplate", ok},
    {"qgxDBjorUxs", "sceHttpCreateConnectionWithURL", http_object},
    {"Aeu5wVKkF9w", "sceHttpCreateRequestWithURL", http_object},
    {"1e2BNwI-XzE", "sceHttpSendRequest", http_fail},
    {"6381dWF+xsQ", "sceHttpCreateEpoll", http_epoll},
    {"s2-NPIvz+iA", "sceHttpSetNonblock", ok},
    {"0S9tTH0uqTU", "sceHttpSetConnectTimeOut", ok},
    {"f42K37mm5RM", "sceHttpsEnableOption", ok},
    {"mSQCxzWTwVI", "sceHttpsDisableOption", ok},
    {"EY28T2bkN7k", "sceHttpAddRequestHeader", ok},
    {"PTiFIUxCpJc", "sceHttpSetRequestContentLength", ok},
    {"P6A3ytpsiYc", "sceHttpDeleteConnection", ok},
    {"qe7oZ+v4PWA", "sceHttpDeleteRequest", ok},
    {"sWQiqKvYTVA", "sceHttpAbortWaitRequest", ok},
    {"wYhXVfS2Et4", "sceHttpDestroyEpoll", ok},
    {"-xm7kZQNpHI", "sceHttpSetEpoll", ok},
    {"59tL1AQBb8U", "sceHttpUnsetEpoll", ok},
    {"qISjDHrxONc", "sceHttpWaitRequest", http_wait},
    {"0a2TBNfE3BU", "sceHttpGetStatusCode", http_fail},
    {"yuO2H2Uvnos", "sceHttpGetResponseContentLength", http_fail},
    {"P5pdoykPYTk", "sceHttpReadData", http_fail},
    {"eQH7nWPcAgc", "sceNpGetState", np_state},
    {"XDncXQIJUSk", "sceNpGetOnlineId", np_signed_out},
    {"p-o74CnoNzY", "sceNpGetNpId", np_signed_out},
    {"VfRSmPmj8Q8", "sceNpRegisterStateCallback", np_register},
    {"mjjTXh+NHWY", "sceNpUnregisterStateCallback", ok},
    {"uFJpaKNBAj4", "sceNpRegisterGamePresenceCallback", np_register_void},
    {"GImICnh+boA", "sceNpRegisterPlusEventCallback", np_register},
    {"xViqJdDgKl0", "sceNpUnregisterPlusEventCallback", ok},
    {"3Zl8BePTh9Y", "sceNpCheckCallback", ok},
    {"Ec63y59l9tw", "sceNpSetNpTitleId", ok},
    {"Gaxrp3EWY-M", "sceNpNotifyPlusFeature", ok},
    {"A2CQ3kgSopQ", "sceNpSetContentRestriction", ok},
    {"eiqMCt9UshI", "sceNpCreateAsyncRequest", np_request},
    {"S7QTn72PrDw", "sceNpDeleteRequest", ok},
    {"OzKvTvg3ZYU", "sceNpAbortRequest", ok},
    {"uqcPJLWL08M", "sceNpPollAsync", np_poll},
    {"2rsFmlGWleQ", "sceNpCheckNpAvailability", np_signed_out},
    {"ilwLM4zOmu4", "sceNpGetParentalControlInfo", np_signed_out},
    {"r6MyYJkryz8", "sceNpCheckPlus", np_signed_out},
    {"IPb1hd1wAGc", "sceNpGetGamePresenceStatus", np_signed_out},
    {"i8UmXTSq7N4", "sceNpCmpNpId", np_compare},
    {"dj+O5aD2a0Q", "sceNpCmpOnlineId", np_compare},
    {"N+mr7GjTvr8", "sceNpAuthCreateAsyncRequest", np_request},
    {"H8wG9Bk-nPc", "sceNpAuthDeleteRequest", ok},
    {"gjSyfzSsDcE", "sceNpAuthPollAsync", np_poll},
    {"KxGkOrQJTqY", "sceNpAuthGetAuthorizationCode", np_signed_out},
    {"8533Q+LU7EQ", "sceNpLookupCreateTitleCtx", np_request},
    {"mtqDK9zkoIE", "sceNpLookupDeleteTitleCtx", ok},
    {"JA4+sS39GMs", "sceNpLookupCreateAsyncRequest", np_request_ctx},
    {"wLaxchvEEnk", "sceNpLookupDeleteRequest", ok},
    {"eYz4v5Uek9U", "sceNpLookupAbortRequest", ok},
    {"V4EVrruHuy8", "sceNpLookupPollAsync", np_poll},
    {"T6tnM1Uti4g", "sceNpLookupNpId", np_signed_out},
    {"KnNA1TEgtBI", "sceNpScoreCreateNpTitleCtx", np_request_ctx},
    {"G0pE+RNCwfk", "sceNpScoreDeleteNpTitleCtx", ok},
    {"gW8qyjYrUbk", "sceNpScoreCreateRequest", np_request},
    {"dK8-SgYf6r4", "sceNpScoreDeleteRequest", ok},
    {"1i7kmKbX6hk", "sceNpScoreAbortRequest", ok},
    {"G3AnLNdRBjE", "sceNpWebApiInitialize", lib_init_id},
    {"asz3TtIqGF8", "sceNpWebApiTerminate", ok},
    {"x1Y7yiYSk7c", "sceNpWebApiCreateContext", np_signed_out},
    {"rdgs5Z1MyFw", "sceNpWebApiCreateRequest", np_signed_out},
    {"kVbL4hL3K7w", "sceNpWebApiSendRequest", np_signed_out},
    {"noQgleu+KLE", "sceNpWebApiDeleteRequest", ok},
    {"JzhYTP2fG18", "sceNpWebApiAbortRequest", ok},
    {"XUjdsSTTZ3U", "sceNpWebApiDeleteContext", ok},
    {"CQtPRSF6Ds8", "sceNpWebApiReadData", np_signed_out},
    {"k210oKgP80Y", "sceNpWebApiGetHttpStatusCode", np_signed_out},
    {"VwJ5L0Higg0", "sceNpWebApiGetHttpResponseHeaderValue", np_signed_out},
    {"743ZzEBzlV8", "sceNpWebApiGetHttpResponseHeaderValueLength", np_signed_out},
    {"y5Ta5JCzQHY", "sceNpWebApiCreatePushEventFilter", np_signed_out},
    {"zE+R6Rcx3W0", "sceNpWebApiDeletePushEventFilter", ok},
    {"PfSTDCgNMgc", "sceNpWebApiRegisterPushEventCallback", np_signed_out},
    {"qK4o2656W4w", "sceNpWebApiUnregisterPushEventCallback", ok},
    {"or0e885BlXo", "sceNpWebApiUtilityParseNpId", np_signed_out},
    {"7vjNQ6Z1op0", "sceNpMatching2ContextStart", np_signed_out},
    {"-f6M4caNe8k", "sceNpMatching2ContextStop", ok},
    {"Nz-ZE7ur32I", "sceNpMatching2DestroyContext", ok},
    {"fQQfP87I7hs", "sceNpMatching2RegisterContextCallback", ok},
    {"4Nj7u5B5yCA", "sceNpMatching2RegisterLobbyEventCallback", ok},
    {"p+2EnxmaAMM", "sceNpMatching2RegisterRoomEventCallback", ok},
    {"0UMeWRGnZKA", "sceNpMatching2RegisterSignalingCallback", ok},
    {"+8e7wXLmjds", "sceNpMatching2SetDefaultRequestOptParam", ok},
    {"zCWZmXXN600", "sceNpMatching2CreateJoinRoom", np_signed_out},
    {"CSIMDsVjs-g", "sceNpMatching2JoinRoom", np_signed_out},
    {"BD6kfx442Do", "sceNpMatching2LeaveRoom", np_signed_out},
    {"VqZX7POg2Mk", "sceNpMatching2SearchRoom", np_signed_out},
    {"LhCPctIICxQ", "sceNpMatching2GetServerId", np_signed_out},
    {"rJNPJqDCpiI", "sceNpMatching2GetWorldInfoList", np_signed_out},
    {"wyvlEgZ-55w", "sceNpMatching2GetLobbyInfoList", np_signed_out},
    {"n5JmImxTiZU", "sceNpMatching2JoinLobby", np_signed_out},
    {"BBbJ92uUdCg", "sceNpMatching2LeaveLobby", np_signed_out},
    {"NCP3bLGPt+o", "sceNpMatching2GrantRoomOwner", np_signed_out},
    {"AUVfU6byg3c", "sceNpMatching2KickoutRoomMember", np_signed_out},
    {"q7GK98-nYSE", "sceNpMatching2SetRoomDataExternal", np_signed_out},
    {"S9D8JSYIrjE", "sceNpMatching2SetRoomDataInternal", np_signed_out},
    {"HoqTrkS9c5Q", "sceNpMatching2SetRoomMemberDataInternal", np_signed_out},
    {"tHD5FPFXtu4", "sceNpMatching2SignalingGetConnectionStatus", np_signed_out},
    {"wUmwXZHaX1w", "sceNpMatching2SignalingGetPingInfo", np_signed_out},
    {"hx+LIg-1koI", "sceNpSignalingDeleteContext", ok},
    {"0UvTFeomAUM", "sceNpSignalingActivateConnection", np_signed_out},
    {"6UEembipgrM", "sceNpSignalingDeactivateConnection", ok},
    {"bD-JizUb3JM", "sceNpSignalingGetConnectionStatus", np_signed_out},
    {"2b3TI0mDYiI", "sceNpScoreCensorComment", np_signed_out},
    {"r4oAo9in0TA", "sceNpScoreSanitizeComment", np_signed_out},
    {"LoVMVrijVOk", "sceNpScoreGetBoardInfo", np_signed_out},
    {"zKoVok6FFEI", "sceNpScoreGetGameData", np_signed_out},
    {"ETS-uM-vH9Q", "sceNpScoreGetRankingByNpIdPcId", np_signed_out},
    {"KBHxDjyk-jA", "sceNpScoreGetRankingByRange", np_signed_out},
    {"bcoVwcBjQ9E", "sceNpScoreRecordGameData", np_signed_out},
    {"zT0XBtgtOSI", "sceNpScoreRecordScore", np_signed_out},
    {"bygbKdHmjn4", "sceNpScoreSetPlayerCharacterId", ok},
    {"10t3e5+JPnU", "sceNpMatching2Initialize", ok},
    {"Mqp3lJ+sjy4", "sceNpMatching2Terminate", ok},
    {"YfmpW719rMo", "sceNpMatching2CreateContext", np_signed_out},
    {"3KOuC4RmZZU", "sceNpSignalingInitialize", ok},
    {"NPhw0UXaNrk", "sceNpSignalingTerminate", ok},
    {"5yYjEdd4t8Y", "sceNpSignalingCreateContext", np_signed_out},
};

static int is_online_name(const char *name) {
    static const char *const online[] = {"sceNet", "sceNp", "sceHttp", "sceSsl"};
    static const char *const local[] = {"sceNpTrophy", "sceNpCommerce", "sceNpProfileDialog"};
    int found = 0;
    for (size_t i = 0; i < sizeof(online) / sizeof(*online); ++i)
        if (!strncmp(name, online[i], strlen(online[i]))) found = 1;
    for (size_t i = 0; i < sizeof(local) / sizeof(*local); ++i)
        if (!strncmp(name, local[i], strlen(local[i]))) found = 0;
    return found;
}

uintptr_t sys_net_offline_resolve(const char *nid) {
    size_t length = strcspn(nid, "#");
    const SysExport *found = NULL;
    for (size_t i = 0; !found && i < sizeof(offline_exports) / sizeof(*offline_exports); ++i)
        if (strlen(offline_exports[i].nid) == length && !memcmp(offline_exports[i].nid, nid, length)) found = &offline_exports[i];
    const char *enabled = getenv("BB_ONLINE");
    if (enabled && enabled[0] == '1') {
        const char *name = sys_import_symbol(nid);
        if (name && is_online_name(name)) {
            uintptr_t function = bbgpu_resolve(nid);
            if (function) return function;
            printf("Online: %s unavailable; retaining its offline contract\n", name);
        }
    }
    return found ? (uintptr_t)found->function : 0;
}
