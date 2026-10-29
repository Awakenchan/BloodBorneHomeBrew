#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define htons(v) __builtin_bswap16(v)
#define ntohs(v) __builtin_bswap16(v)
#define htonl(v) __builtin_bswap32(v)
#define ntohl(v) __builtin_bswap32(v)
#else
#include <arpa/inet.h>
#endif

#define ORBIS_OK 0
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)

static HostMutex lock=HOST_MUTEX_INIT;
static int next_id=1;
static int new_id(void) { host_lock(&lock); int id=next_id++; host_unlock(&lock); return id; }

static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
#ifdef _WIN32
    return runtime_win_inet_pton4(src,dst);
#else
    return inet_pton(AF_INET,src,dst);
#endif
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
#ifdef _WIN32
    return runtime_win_inet_ntop4(src,dst,size);
#else
    return inet_ntop(AF_INET,src,dst,size);
#endif
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0;
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077;
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
    if (timeout>0) host_sleep_ns((uint64_t)timeout*1000);
    return 0;
}

static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1;
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0;
}

static const RuntimeExport exports[]={
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl},
    {"sceNetNtohl",net_ntohl}, {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable},
    {"sceNetEpollAbort",net_unreachable}, {"sceNetResolverStartNtoa",net_unreachable},
    {"sceNetResolverStartAton",net_unreachable}, {"sceNetCtlGetState",netctl_state},
    {"sceNetCtlGetInfo",netctl_info}, {"sceNetCtlRegisterCallback",netctl_register},
    {"sceNetCtlCheckCallback",netctl_check}, {"sceNetCtlUnregisterCallback",netctl_unregister},
    {"sceNetCtlGetNatInfo",netctl_nat}, {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void}, {"sceHttpCreateTemplate",http_object},
    {"sceHttpDeleteTemplate",ok_void}, {"sceHttpCreateConnectionWithURL",http_object},
    {"sceHttpCreateRequestWithURL",http_object}, {"sceHttpSendRequest",http_fail},
    {"sceHttpCreateEpoll",http_epoll}, {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void}, {"sceHttpAddRequestHeader",ok_void},
    {"sceHttpSetRequestContentLength",ok_void}, {"sceHttpDeleteConnection",ok_void},
    {"sceHttpDeleteRequest",ok_void}, {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail}, {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out},
    {"sceNpGetNpId",np_signed_out}, {"sceNpRegisterStateCallback",np_register},
    {"sceNpUnregisterStateCallback",ok_void}, {"sceNpRegisterGamePresenceCallback",np_register_void},
    {"sceNpRegisterPlusEventCallback",np_register}, {"sceNpUnregisterPlusEventCallback",ok_void},
    {"sceNpCheckCallback",ok_void}, {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void},
    {"sceNpSetContentRestriction",ok_void}, {"sceNpCreateAsyncRequest",np_request},
    {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void}, {"sceNpPollAsync",np_poll},
    {"sceNpCheckNpAvailability",np_signed_out}, {"sceNpGetParentalControlInfo",np_signed_out},
    {"sceNpCheckPlus",np_signed_out}, {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare}, {"sceNpAuthCreateAsyncRequest",np_request},
    {"sceNpAuthDeleteRequest",ok_void}, {"sceNpAuthPollAsync",np_poll},
    {"sceNpAuthGetAuthorizationCode",np_signed_out}, {"sceNpLookupCreateTitleCtx",np_request},
    {"sceNpLookupDeleteTitleCtx",ok_void}, {"sceNpLookupCreateAsyncRequest",np_request_ctx},
    {"sceNpLookupDeleteRequest",ok_void}, {"sceNpLookupAbortRequest",ok_void},
    {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void},
    {"sceNpScoreAbortRequest",ok_void}, {"sceNpWebApiInitialize",lib_init_id},
    {"sceNpWebApiTerminate",ok_void}, {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out},
    {"sceNpWebApiUnregisterPushEventCallback",ok_void}, {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void}, {"sceNpMatching2RegisterContextCallback",ok_void},
    {"sceNpMatching2RegisterLobbyEventCallback",ok_void}, {"sceNpMatching2RegisterRoomEventCallback",ok_void},
    {"sceNpMatching2RegisterSignalingCallback",ok_void}, {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out},
    {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out},
    {"sceNpMatching2SignalingGetPingInfo",np_signed_out}, {"sceNpSignalingDeleteContext",ok_void},
    {"sceNpSignalingActivateConnection",np_signed_out}, {"sceNpSignalingDeactivateConnection",ok_void},
    {"sceNpSignalingGetConnectionStatus",np_signed_out}, {"sceNpScoreCensorComment",np_signed_out},
    {"sceNpScoreSanitizeComment",np_signed_out}, {"sceNpScoreGetBoardInfo",np_signed_out},
    {"sceNpScoreGetGameData",np_signed_out}, {"sceNpScoreGetRankingByNpIdPcId",np_signed_out},
    {"sceNpScoreGetRankingByRange",np_signed_out}, {"sceNpScoreRecordGameData",np_signed_out},
    {"sceNpScoreRecordScore",np_signed_out}, {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out}, {"sceNpSignalingInitialize",ok_void},
    {"sceNpSignalingTerminate",ok_void}, {"sceNpSignalingCreateContext",np_signed_out},
};
static int online_symbol(const char *symbol) {
    static const char *const prefixes[]={"sceNet","sceNp","sceHttp","sceSsl"};
    static const char *const local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    int online=0;
    for (size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);++i)
        if (!strncmp(symbol,prefixes[i],strlen(prefixes[i]))) online=1;
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i)
        if (!strncmp(symbol,local[i],strlen(local[i]))) online=0;
    return online;
}
uintptr_t runtime_services_resolve(const char *name) {
    const char *enabled=getenv("BB_ONLINE");
    if (enabled && enabled[0]=='1') {
        const char *symbol=runtime_symbol(name);
        if (symbol && online_symbol(symbol)) {
            const uintptr_t function=bbgpu_resolve(name);
            if (function) return function;
            printf("Online: %s unavailable; retaining its offline contract\n",symbol);
        }
    }
    return RUNTIME_LOOKUP(exports,name);
}
